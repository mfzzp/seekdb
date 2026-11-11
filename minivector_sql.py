"""Parse the supported SQL subset into commands and typed expressions."""

import json
import math
import operator
import re
import struct


class SQLError(ValueError):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def require(condition, code, message):
    if not condition:
        raise SQLError(code, message)


TOKEN = re.compile(
    r"(?P<skip>\s+|--[^\n]*|/\*[\s\S]*?\*/)"
    r"|(?P<name>[a-zA-Z_][a-zA-Z_0-9]*|`(?:[^`]|\x60\x60)+`)"
    # Consume each run of non-quote characters once, even in long vector literals.
    r"|(?P<string>'(?:[^']++|'')*')"
    r"|(?P<number>[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?)"
    r"|(?P<symbol><=|>=|[(),;*=<>\[\]])"
)
COMPARISONS = {
    "=": operator.eq, "<": operator.lt, "<=": operator.le,
    ">": operator.gt, ">=": operator.ge,
}
DISTANCE = "l2_distance"


def vector(value, dimension):
    try:
        numbers = json.loads(value) if isinstance(value, str) else value
        require(isinstance(numbers, list), "invalid_vector", "expected a vector array")
        require(len(numbers) == dimension, "dimension_mismatch", "wrong vector dimension")
        require(all(type(number) in (int, float) and math.isfinite(number)
                    for number in numbers), "invalid_vector", "expected finite numbers")
        packed = struct.pack(f"<{dimension}f", *numbers)
        require(all(map(math.isfinite, struct.unpack(f"<{dimension}f", packed))),
                "invalid_vector", "float32 overflow")
        return packed
    except (OverflowError, json.JSONDecodeError, struct.error) as exc:
        raise SQLError("invalid_vector", "expected a finite float32 array") from exc


def typed(value, column):
    name, kind, width = column
    if kind == "vector":
        return vector(value, width)
    if kind == "bigint":
        require(type(value) is int and -(1 << 63) <= value < (1 << 63),
                "invalid_value", f"{name} requires a signed BIGINT")
    else:
        require(isinstance(value, str) and len(value) <= width,
                "invalid_value", f"{name} requires a VARCHAR({width})")
        require(not any(0xD800 <= ord(char) <= 0xDFFF for char in value),
                "invalid_value", "string contains a surrogate code point")
    return value


class Parser:
    def __init__(self, sql):
        self.tokens = []
        position = 0
        while position < len(sql):
            match = TOKEN.match(sql, position)
            require(match is not None, "unsupported_sql", f"unexpected token at {position}")
            if match.lastgroup != "skip":
                self.tokens.append((match.lastgroup, match[0]))
            position = match.end()
        self.tokens.append(("end", ""))
        self.position = 0

    def peek(self):
        return self.tokens[self.position]

    def accept(self, text):
        kind, value = self.peek()
        if kind in ("name", "symbol") and value.lower() == text:
            self.position += 1
            return True
        return False

    def expect(self, text):
        require(self.accept(text), "unsupported_sql", f"expected {text}")

    def take(self, kind):
        actual, value = self.peek()
        require(actual == kind, "unsupported_sql", f"expected {kind}")
        self.position += 1
        return value

    def name(self):
        value = self.take("name")
        if value.startswith("`"):
            return value[1:-1].replace("``", "`")
        return value

    def integer(self):
        value = self.take("number")
        require(re.fullmatch(r"[+-]?\d+", value), "invalid_value", "expected integer")
        try:
            return int(value)
        except ValueError as exc:
            raise SQLError("invalid_value", "integer is too large") from exc

    def value(self):
        kind, value = self.peek()
        if kind == "string":
            return self.take("string")[1:-1].replace("''", "'")
        if self.accept("["):
            return self.separated(lambda: float(self.take("number")), "]")
        return self.integer()

    def separated(self, read, end=None):
        result = [read()]
        while self.accept(","):
            result.append(read())
        if end:
            self.expect(end)
        return result

    def where(self):
        terms = []
        if self.accept("where"):
            while True:
                column = self.name()
                op = self.take("symbol")
                require(op in COMPARISONS, "unsupported_sql", "unsupported comparison")
                terms.append((column, op, self.value()))
                if not self.accept("and"):
                    break
        return terms

    def create(self):
        if self.accept("vector"):
            self.expect("index")
            index = self.name()
            self.expect("on")
            table = self.name()
            self.expect("(")
            column = self.name()
            self.expect(")")
            return "create_index", (table, index, column)
        self.expect("table")
        table = self.name()
        self.expect("(")
        columns = []
        primary = []
        while True:
            if self.accept("primary"):
                self.expect("key")
                self.expect("(")
                primary.append(self.name())
                self.expect(")")
            else:
                name = self.name()
                kind = self.name().lower()
                require(kind in ("bigint", "varchar", "vector"),
                        "unsupported_sql", "unsupported column type")
                width = 0
                if kind != "bigint":
                    self.expect("(")
                    width = self.integer()
                    require(width > 0, "invalid_schema", "nonpositive column width")
                    self.expect(")")
                if self.accept("primary"):
                    self.expect("key")
                    primary.append(name)
                columns.append((name, kind, width))
            if not self.accept(","):
                break
        self.expect(")")
        return "create_table", (table, columns, primary)

    def insert(self):
        self.expect("into")
        table = self.name()
        names = self.separated(self.name, ")") if self.accept("(") else None
        self.expect("values")

        def row():
            self.expect("(")
            return self.separated(self.value, ")")

        return table, names, self.separated(row)

    def update(self):
        table = self.name()
        self.expect("set")

        def assignment():
            name = self.name()
            self.expect("=")
            return name, self.value()

        return table, self.separated(assignment), self.where()

    def delete(self):
        self.expect("from")
        return self.name(), self.where()

    def expression(self):
        if self.peek()[1].lower() == DISTANCE and self.tokens[self.position + 1][1] == "(":
            self.expect(DISTANCE)
            self.expect("(")
            column = self.name()
            self.expect(",")
            target = self.value()
            self.expect(")")
            return column, target
        return self.name()

    def projection(self):
        expr = self.expression()
        alias = self.name() if self.accept("as") else None
        return expr, alias

    def ordering(self):
        expr = self.expression()
        descending = self.accept("desc")
        if not descending:
            self.accept("asc")
        return expr, descending

    def select(self):
        projections = None if self.accept("*") else self.separated(self.projection)
        self.expect("from")
        table = self.name()
        terms = self.where()
        order = []
        if self.accept("order"):
            self.expect("by")
            order = self.separated(self.ordering)
        limit = self.integer() if self.accept("limit") else None
        require(limit is None or limit >= 0, "invalid_value", "negative LIMIT")
        return table, projections, terms, order, limit

    def parse(self):
        command = self.name().lower()
        if command == "create":
            command, args = self.create()
        elif command in ("insert", "update", "delete", "select"):
            args = getattr(self, command)()
        elif command in ("begin", "commit", "rollback"):
            args = ()
        else:
            raise SQLError("unsupported_sql", f"unsupported command {command}")
        self.accept(";")
        require(self.peek()[0] == "end", "unsupported_sql", "unexpected trailing SQL")
        return command, args
