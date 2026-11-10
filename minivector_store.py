"""Disk-backed row log with transaction commit markers and bounded row caching."""

from collections import OrderedDict
import marshal
import os
from pathlib import Path
import struct

from minivector_sql import require


LENGTH = struct.Struct("<Q")
COMMIT = b"!"
CACHE_BUDGET = 96 * 1024 * 1024


class Table:
    def __init__(self, columns, primary):
        self.columns = columns
        self.primary = primary
        self.names = {column[0].casefold(): i for i, column in enumerate(columns)}
        self.rows = {}
        self.index = None
        self.blocks = None
        self.axis = None

    def column(self, name):
        key = name.casefold()
        require(key in self.names, "unknown_column", f"unknown column {name}")
        return self.names[key]


class Transaction:
    def __init__(self):
        self.tables = {}
        self.indexes = {}
        self.rows = {}

    def apply(self, operations):
        for action, args in operations:
            if action == "table":
                name, columns, primary = args
                self.tables[name] = Table(columns, primary)
            elif action == "index":
                name, index, column = args
                self.indexes[name] = (index, column)
            else:
                name, key, offset = args
                self.rows.setdefault(name, {})[key] = offset

    def publish(self, tables):
        tables.update(self.tables)
        for name, index in self.indexes.items():
            tables[name].index = index
        for name, changes in self.rows.items():
            table = tables[name]
            table.blocks = None
            table.axis = None
            for key, offset in changes.items():
                if offset is None:
                    table.rows.pop(key, None)
                else:
                    table.rows[key] = offset


class Store:
    def __init__(self, data_dir):
        root = Path(data_dir)
        root.mkdir(parents=True, exist_ok=True)
        self.path = root / "rows.log"
        self.writer = self.path.open("a+b", buffering=128 * 1024)
        self.reader = self.path.open("rb", buffering=128 * 1024)
        self.tables = {}
        self.pending = None
        self.cache = OrderedDict()
        self.cache_bytes = 0
        self.committed_end = self.recover()
        self.truncate(self.committed_end)

    def recover(self):
        transaction = Transaction()
        end = 0
        size = os.fstat(self.reader.fileno()).st_size
        while self.reader.tell() + LENGTH.size <= size:
            offset = self.reader.tell()
            length, = LENGTH.unpack(self.reader.read(LENGTH.size))
            if not 0 < length <= size - self.reader.tell():
                break
            tag = self.reader.read(1)
            if tag == COMMIT and length == 1:
                transaction.publish(self.tables)
                transaction = Transaction()
                end = self.reader.tell()
            elif tag == b"r":
                # Only the table and primary key are needed to rebuild row offsets.
                header_size, = LENGTH.unpack(self.reader.read(LENGTH.size))
                if header_size > length - LENGTH.size - 1:
                    raise ValueError("invalid log row header")
                name, key = marshal.loads(self.reader.read(header_size))
                transaction.apply((("row", (name, key, offset)),))
                self.reader.seek(offset + LENGTH.size + length)
            elif tag == b"m":
                transaction.apply((marshal.loads(self.reader.read(length - 1)),))
            else:
                raise ValueError("invalid log frame")
        return end

    def truncate(self, offset):
        self.writer.flush()
        self.writer.truncate(offset)
        self.writer.seek(0, os.SEEK_END)
        # A buffered reader may retain bytes beyond the truncated position.
        self.reader.close()
        self.reader = self.path.open("rb", buffering=128 * 1024)

    def frame(self, payload):
        offset = self.writer.tell()
        self.writer.write(LENGTH.pack(len(payload)))
        self.writer.write(payload)
        return offset

    def write(self, action, args):
        if action == "put":
            name, key, row = args
            header = marshal.dumps((name, key))
            offset = self.frame(b"r" + LENGTH.pack(len(header)) + header + marshal.dumps(row))
            return "row", (name, key, offset)
        operation = ("row", (*args, None)) if action == "delete" else (action, args)
        self.frame(b"m" + marshal.dumps(operation))
        return operation

    def mutate(self, operations):
        start = self.writer.tell()
        updates = Transaction()
        count = 0

        def logged_operations():
            nonlocal count
            for action, args in operations:
                count += 1
                yield self.write(action, args)

        try:
            updates.apply(logged_operations())
            self.writer.flush()
        except Exception:
            self.truncate(start)
            raise
        if not count:
            return 0
        if self.pending is None:
            self.durable_commit(updates, start)
        else:
            self.pending.tables.update(updates.tables)
            self.pending.indexes.update(updates.indexes)
            for name, changes in updates.rows.items():
                self.pending.rows.setdefault(name, {}).update(changes)
        return count

    def durable_commit(self, transaction, abort_offset):
        try:
            self.frame(COMMIT)
            self.writer.flush()
            os.fsync(self.writer.fileno())
        except OSError:
            self.truncate(abort_offset)
            raise
        self.committed_end = self.writer.tell()
        transaction.publish(self.tables)

    def begin(self):
        require(self.pending is None, "transaction_active", "transaction already active")
        self.pending = Transaction()

    def commit(self):
        require(self.pending is not None, "no_transaction", "no active transaction")
        self.durable_commit(self.pending, self.writer.tell())
        self.pending = None

    def rollback(self):
        require(self.pending is not None, "no_transaction", "no active transaction")
        self.truncate(self.committed_end)
        self.pending = None
        # Discard cached uncommitted offsets before the log can reuse them.
        self.cache.clear()
        self.cache_bytes = 0

    def close(self):
        if self.pending is not None:
            self.rollback()
        self.reader.close()
        self.writer.close()

    def table(self, name):
        table = self.pending.tables.get(name) if self.pending is not None else None
        if table is None:
            table = self.tables.get(name)
        require(table is not None, "unknown_table", f"unknown table {name}")
        return table

    def changes(self, name):
        return self.pending.rows.get(name, {}) if self.pending is not None else {}

    def read_row(self, table, offset):
        cached = self.cache.get(offset)
        if cached is not None:
            self.cache.move_to_end(offset)
            return cached[1]
        self.reader.seek(offset)
        length, = LENGTH.unpack(self.reader.read(LENGTH.size))
        self.reader.read(1)
        header_size, = LENGTH.unpack(self.reader.read(LENGTH.size))
        self.reader.seek(header_size, os.SEEK_CUR)
        row = marshal.loads(self.reader.read(length - 1 - LENGTH.size - header_size))
        weight = 256 + len(row) * 32 + 4 * length
        values = list(row)
        for i, (_, kind, width) in enumerate(table.columns):
            if kind == "vector":
                values[i] = struct.unpack(f"<{width}f", values[i])
                weight += 32 * width
        if weight <= CACHE_BUDGET:
            while self.cache_bytes + weight > CACHE_BUDGET:
                _, (removed, _) = self.cache.popitem(last=False)
                self.cache_bytes -= removed
            self.cache[offset] = weight, values
            self.cache_bytes += weight
        return values
