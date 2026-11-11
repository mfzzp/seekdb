"""Single-process MiniVectorDB SQL execution and JSON Lines entry point."""

import argparse
from array import array
from bisect import bisect_left
import heapq
from itertools import chain, islice
import json
import math
import struct
import sys

from minivector_sql import COMPARISONS, DISTANCE, Parser, SQLError, require, typed
from minivector_store import Store


MAX_BLOCK_ROWS = 50_000
MAX_COMPONENT_BYTES = 4 * 1024 * 1024


class Descending:
    def __init__(self, value):
        self.value = value

    def __eq__(self, other):
        return self.value == other.value

    def __lt__(self, other):
        return self.value > other.value


class Database(Store):
    def execute(self, sql, mode):
        command, args = Parser(sql).parse()
        handler = self.handlers[command]
        return handler(self, *args, mode=mode) if command == "select" else handler(self, *args)

    def create_table(self, table, columns, primary):
        name = table.casefold()
        require(name not in self.tables and
                (self.pending is None or name not in self.pending.tables),
                "table_exists", f"table {table} already exists")
        by_name = {column[0].casefold(): i for i, column in enumerate(columns)}
        require(len(by_name) == len(columns), "invalid_schema", "duplicate column")
        require(len(primary) == 1 and primary[0].casefold() in by_name,
                "invalid_schema", "one primary key is required")
        pk = by_name[primary[0].casefold()]
        require(columns[pk][1] == "bigint", "invalid_schema", "primary key must be BIGINT")
        require(sum(column[1] == "vector" for column in columns) <= 1,
                "invalid_schema", "at most one vector column")
        self.mutate((("table", (name, columns, pk)),))

    def create_index(self, name, index, column):
        name = name.casefold()
        table = self.table(name)
        col = table.column(column)
        require(table.columns[col][1] == "vector", "invalid_schema", "index requires a vector")
        require(table.index is None and (self.pending is None or name not in self.pending.indexes),
                "index_exists", "a vector index already exists on this table")
        self.mutate((("index", (name, index, col)),))

    def insert(self, name, names, rows):
        name = name.casefold()
        table = self.table(name)
        changes = self.changes(name)
        columns = list(range(len(table.columns))) if names is None else [
            table.column(column) for column in names
        ]
        require(len(set(columns)) == len(table.columns) == len(columns),
                "invalid_value", "all columns must appear exactly once")

        def operations():
            seen = set()
            for values in rows:
                require(len(values) == len(columns), "invalid_value", "wrong value count")
                row = [None] * len(columns)
                for index, value in zip(columns, values):
                    row[index] = typed(value, table.columns[index])
                key = row[table.primary]
                require(key not in seen and changes.get(key, table.rows.get(key)) is None,
                        "duplicate_key", "duplicate primary key")
                seen.add(key)
                yield "put", (name, key, row)

        return {"affected_rows": self.mutate(operations())}

    def filters(self, table, terms):
        bound = []
        for column, operation, value in terms:
            index = table.column(column)
            spec = table.columns[index]
            require(spec[1] != "vector", "unsupported_sql", "vector comparison")
            bound.append((index, operation, typed(value, spec)))
        return bound

    def component_offsets(self, table, component, value):
        if table.blocks is None or table.blocks[0] != component:
            if len(table.rows) > MAX_BLOCK_ROWS:
                return None
            groups = {}
            budget = MAX_COMPONENT_BYTES
            for slot, offset in table.index_snapshot().rows():
                field = self.read_row(table, offset)[component]
                budget -= 64 + (4 * len(field) if isinstance(field, str) else 0)
                if budget < 0:
                    return None
                slots = groups.get(field)
                if slots is None:
                    groups[field] = slots = array("I")
                slots.append(slot)
            table.blocks = component, groups

        slots = table.blocks[1].get(value, ())
        snapshot = table.snapshot
        if snapshot.deleted_count:
            return (snapshot.offsets[slot] for slot in slots if snapshot.is_live(slot))
        return (snapshot.offsets[slot] for slot in slots)

    def candidates(self, name, table, filters, clustered):
        changes = self.changes(name)
        for col, op, value in filters:
            if col == table.primary and op == "=":
                offset = changes.get(value, table.rows.get(value))
                return () if offset is None else (offset,)
        if clustered and table.index is not None and not changes:
            if table.blocks is not None:
                indexed_column = table.blocks[0]
                for col, op, value in filters:
                    if col == indexed_column and op == "=":
                        return self.component_offsets(table, col, value)
            for col, op, value in filters:
                if op == "=":
                    offsets = self.component_offsets(table, col, value)
                    if offsets is not None:
                        return offsets
        if not changes:
            return table.rows.values()

        def merged():
            for key, offset in table.rows.items():
                if key not in changes:
                    yield offset
            yield from (offset for offset in changes.values() if offset is not None)

        return merged()

    def matching(self, name, table, filters, clustered=False):
        checks = [(col, COMPARISONS[op], value) for col, op, value in filters]
        for offset in self.candidates(name, table, filters, clustered):
            row = self.read_row(table, offset)
            if not checks or all(compare(row[col], value) for col, compare, value in checks):
                yield row

    def update(self, name, assignments, terms):
        name = name.casefold()
        table = self.table(name)
        filters = self.filters(table, terms)
        updates = {}
        for column, value in assignments:
            index = table.column(column)
            require(index != table.primary and index not in updates,
                    "unsupported_sql", "duplicate assignment or primary key update")
            updates[index] = typed(value, table.columns[index])

        def operations():
            for current in self.matching(name, table, filters):
                row = list(current)
                for i, (_, kind, width) in enumerate(table.columns):
                    if i in updates:
                        row[i] = updates[i]
                    elif kind == "vector":
                        row[i] = struct.pack(f"<{width}f", *row[i])
                yield "put", (name, row[table.primary], row)

        return {"affected_rows": self.mutate(operations())}

    def delete(self, name, terms):
        name = name.casefold()
        table = self.table(name)
        filters = self.filters(table, terms)
        if all(column == table.primary for column, _, _ in filters):
            # Primary-key predicates can be evaluated without decoding row payloads.
            changes = self.changes(name)
            equality = next((value for _, op, value in filters if op == "="), None)
            if equality is not None:
                candidates = (equality,)
            else:
                candidates = chain(table.rows, (key for key in changes if key not in table.rows))
            checks = [(COMPARISONS[op], value) for _, op, value in filters]
            keys = (key for key in candidates
                    if changes.get(key, table.rows.get(key)) is not None
                    and (not checks or all(compare(key, value) for compare, value in checks)))
        else:
            keys = (row[table.primary] for row in self.matching(name, table, filters))
        return {"affected_rows": self.mutate(("delete", (name, key)) for key in keys)}

    def axis_top_k(self, table, bound, limit):
        """Visit rows in order of their first-coordinate distance lower bound."""
        if not limit:
            return []
        column, target = bound
        snapshot = table.index_snapshot()
        if table.axis is None:
            def entries():
                for slot, offset in snapshot.rows():
                    row = self.read_row(table, offset)
                    yield row[column][0], row[table.primary], slot

            table.axis = sorted(entries())
        axis = table.axis
        right = bisect_left(axis, (target[0],))
        left = right - 1
        best = []
        while left >= 0 or right < len(axis):
            low = target[0] - axis[left][0] if left >= 0 else math.inf
            high = axis[right][0] - target[0] if right < len(axis) else math.inf
            lower_bound = min(low, high)
            if len(best) == limit and math.nextafter(lower_bound, -math.inf) > -best[0][0]:
                break
            if low <= high:
                _, key, slot = axis[left]
                left -= 1
            else:
                _, key, slot = axis[right]
                right += 1
            if snapshot.deleted_count and not snapshot.is_live(slot):
                continue
            row = self.read_row(table, snapshot.offsets[slot])
            distance = math.dist(row[column], target)
            entry = (-distance, -key, row)
            if len(best) < limit:
                heapq.heappush(best, entry)
            elif entry[:2] > best[0][:2]:
                heapq.heapreplace(best, entry)
        return [(row, (-distance,)) for distance, _, row in sorted(best, reverse=True)]

    def distance_top_k(self, records, column, target, primary, limit):
        """Retain only the K best rows and avoid ranking objects for rejected rows."""
        if not limit:
            return []
        best = []
        distance_to = math.dist
        push = heapq.heappush
        replace = heapq.heapreplace
        for row in records:
            distance = distance_to(row[column], target)
            key = row[primary]
            if len(best) < limit:
                push(best, (-distance, -key, row))
            elif distance < -best[0][0] or (distance == -best[0][0] and key < -best[0][1]):
                replace(best, (-distance, -key, row))
        return [(row, (-distance,)) for distance, _, row in sorted(best, reverse=True)]

    def select(self, name, projections, terms, order, limit, mode):
        name = name.casefold()
        table = self.table(name)
        filters = self.filters(table, terms)
        aliases = {}
        evaluators = []
        slots = {}

        def bind(expr):
            if isinstance(expr, str):
                return table.column(expr)
            column, literal = expr
            index = table.column(column)
            spec = table.columns[index]
            require(spec[1] == "vector", "invalid_value", "distance requires a vector column")
            target = struct.unpack(f"<{spec[2]}f", typed(literal, spec))
            key = index, target
            if key not in slots:
                evaluators.append(key)
                slots[key] = -len(evaluators)
            return slots[key]

        names = []
        projected = []
        if projections is None:
            names = [column[0] for column in table.columns]
            projected = list(range(len(names)))
        else:
            for expr, alias in projections:
                bound = bind(expr)
                label = DISTANCE if isinstance(expr, tuple) else table.columns[bound][0]
                names.append(alias if alias is not None else label)
                projected.append(bound)
                if alias is not None:
                    aliases[alias.casefold()] = bound
        ordering = []
        for expr, descending in order:
            if isinstance(expr, str) and expr.casefold() in aliases:
                bound = aliases[expr.casefold()]
            elif expr == DISTANCE and len(evaluators) == 1 and DISTANCE not in table.names:
                bound = -1
            else:
                bound = bind(expr)
            require(bound < 0 or table.columns[bound][1] != "vector",
                    "unsupported_sql", "cannot order by a raw vector")
            ordering.append((bound, descending))

        clustered = bool(ordering and ordering[0][0] < 0)
        records = self.matching(name, table, filters, clustered)

        def values(row):
            return tuple(math.dist(row[column], target) for column, target in evaluators)

        def ranked():
            for row in records:
                distances = values(row)
                rank = []
                for bound, descending in ordering:
                    value = row[bound] if bound >= 0 else distances[-bound - 1]
                    rank.append(Descending(value) if descending else value)
                rank.append(row[table.primary])
                yield tuple(rank), row, distances

        if (mode == "ann" and clustered and len(ordering) == len(evaluators) == 1
                and not ordering[0][1] and limit is not None and not filters
                and table.index is not None and not self.changes(name)
                and len(table.rows) <= MAX_BLOCK_ROWS and len(evaluators[0][1]) <= 8):
            rows = self.axis_top_k(table, evaluators[0], limit)
        elif (clustered and len(ordering) == len(evaluators) == 1
                and not ordering[0][1] and limit is not None):
            column, target = evaluators[0]
            rows = self.distance_top_k(records, column, target, table.primary, limit)
        elif ordering:
            if limit is None:
                winners = sorted(ranked(), key=operator_key)
            else:
                winners = heapq.nsmallest(min(limit, sys.maxsize), ranked(), key=operator_key)
            rows = [(row, distances) for _, row, distances in winners]
        else:
            if limit is not None:
                records = islice(records, min(limit, sys.maxsize))
            rows = ((row, values(row)) for row in records)
        return {"columns": names, "rows": [
            [row[bound] if bound >= 0 else distances[-bound - 1] for bound in projected]
            for row, distances in rows
        ]}

    handlers = {
        "create_table": create_table, "create_index": create_index,
        "insert": insert, "update": update, "delete": delete, "select": select,
        "begin": Store.begin, "commit": Store.commit, "rollback": Store.rollback,
    }


def operator_key(item):
    return item[0]


def send(response):
    line = json.dumps(response, ensure_ascii=True, allow_nan=False, separators=(",", ":"))
    print(line, flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--data-dir", default="/data")
    db = Database(parser.parse_args().data_dir)
    try:
        send({"type": "ready"})
        for line in sys.stdin.buffer:
            request = {}
            try:
                request = json.loads(line)
                require(isinstance(request, dict), "invalid_request", "expected an object")
                operation = request.get("op")
                if operation == "shutdown":
                    db.close()
                    send({"id": request.get("id"), "ok": True})
                    return
                require(operation == "execute", "invalid_request", "expected execute")
                sql = request.get("sql")
                mode = request.get("search_mode", "exact")
                require(isinstance(sql, str) and mode in ("exact", "ann"),
                        "invalid_request", "expected SQL and a valid search mode")
                result = db.execute(sql, mode)
                send({"id": request.get("id"), "ok": True, **(result or {})})
            except (SQLError, ValueError) as exc:
                code = exc.code if isinstance(exc, SQLError) else "invalid_request"
                ident = request.get("id") if isinstance(request, dict) else None
                send({"id": ident, "ok": False, "error": {"code": code, "message": str(exc)}})
    finally:
        if not db.writer.closed:
            db.close()


if __name__ == "__main__":
    main()
