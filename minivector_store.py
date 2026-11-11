"""Disk-backed row log with transaction commit markers and bounded row caching."""

from array import array
from bisect import bisect_left
from collections import OrderedDict
import marshal
import os
from pathlib import Path
import struct

from minivector_sql import require


LENGTH = struct.Struct("<Q")
COMMIT = b"!"
CACHE_BUDGET = 96 * 1024 * 1024
# Interpreter, numpy and BLAS scratch that exist before any structure is built.
BASELINE_BYTES = 96 * 1024 * 1024
# The cgroup also charges page cache and our own allocator slack, so only this
# fraction of the container limit is treated as available to the structures.
USABLE_FRACTION = 0.75


def _cgroup_memory_files(root="/sys/fs/cgroup", proc="/proc/self/cgroup"):
    """Yield the v2 memory-limit files for this process, innermost first.

    Reading only `/sys/fs/cgroup/memory.max` works in a plain Docker container, where
    the mount is the container's own cgroup root, but not when the process sits in a
    nested cgroup - a systemd user scope or a Kubernetes pod - where the file lives
    under the slice path and the root file does not exist.  The ancestors matter too:
    a scope often says "max" while the slice above it carries the limit.
    """
    relative = None
    try:
        with open(proc, encoding="ascii") as handle:
            for line in handle:
                fields = line.strip().split("::")
                if len(fields) == 2:
                    relative = fields[1] or "/"
                    break
    except OSError:
        relative = None
    if relative:
        path = relative
        while True:
            yield os.path.join(root, path.strip("/"), "memory.max")
            if path in ("", "/"):
                break
            path = os.path.dirname(path) or "/"


def memory_limit_bytes(root="/sys/fs/cgroup", proc="/proc/self/cgroup"):
    """Best-effort container memory limit (cgroup v2 from this process, then v1)."""
    limits = []
    for path in _cgroup_memory_files(root, proc):
        try:
            with open(path, encoding="ascii") as handle:
                text = handle.read().strip()
        except OSError:
            continue
        if text.isdigit():
            limits.append(int(text))
    if limits:
        # The effective limit is the tightest one on the way up.
        return min(limits)
    for legacy in (os.path.join(root, "memory.max"),
                   os.path.join(root, "memory", "memory.limit_in_bytes")):
        try:
            with open(legacy, encoding="ascii") as handle:
                text = handle.read().strip()
        except OSError:
            continue
        if text.isdigit():
            value = int(text)
            # cgroup v1 uses a near-2**63 number for "unlimited".
            if 0 < value < (1 << 60):
                return value
    return None


def adaptive_budget(limit, default, floor):
    """Keep a bounded structure at a quarter of the container budget."""
    if not limit:
        return default
    return max(floor, min(default, limit // 4))


class MemoryBudget:
    """One cap shared by the row cache, the dense matrices and the groups.

    The row cache, every per-table matrix and every equality group are
    rebuildable, so the executor asks this object before growing one of them and
    drops the others when the answer would be no.  Used bytes are derived from
    the live structures instead of being tracked incrementally, which keeps the
    number correct when a table is invalidated or dropped.
    """

    def __init__(self, limit=None):
        self.capacity = None
        if limit:
            usable = int(limit * USABLE_FRACTION) - BASELINE_BYTES
            self.capacity = max(8 * 1024 * 1024, usable)

    def fits(self, used, size):
        return self.capacity is None or used + size <= self.capacity

    def share(self, fraction, default, floor):
        if self.capacity is None:
            return default
        return max(floor, min(default, int(self.capacity * fraction)))


_MEMORY_LIMIT = memory_limit_bytes()
CACHE_BUDGET = adaptive_budget(_MEMORY_LIMIT, CACHE_BUDGET, 16 * 1024 * 1024)


class IndexSnapshot:
    # Stable offset slots shared by the scalar blocks and vector axis.
    def __init__(self, offsets):
        self.offsets = array("Q", sorted(offsets))
        self.deleted = bytearray((len(self.offsets) + 7) // 8)
        self.deleted_count = 0

    def discard(self, offset):
        slot = bisect_left(self.offsets, offset)
        self.deleted[slot >> 3] |= 1 << (slot & 7)
        self.deleted_count += 1

    def is_live(self, slot):
        return not self.deleted[slot >> 3] & (1 << (slot & 7))

    def rows(self):
        for slot, offset in enumerate(self.offsets):
            if not self.deleted_count or self.is_live(slot):
                yield slot, offset


class Table:
    def __init__(self, columns, primary):
        self.columns = columns
        self.primary = primary
        self.names = {column[0].casefold(): i for i, column in enumerate(columns)}
        self.rows = {}
        # Lower bound using the same units as cache accounting; strings only add weight.
        self.minimum_row_weight = 256 + 32 * len(columns) + sum(
            48 * width for _, kind, width in columns if kind == "vector")
        self.index = None
        self.blocks = None
        self.axis = None
        self.snapshot = None
        # Optional dense (snapshot, column, values, keys) acceleration built by
        # the executor; always invalidated together with the index snapshot.
        self.matrix = None
        # Compact per-column arrays over stable slots, used to turn scalar
        # predicates into candidate slots before any vector work.
        self.scalar_cache = None

    def large_scan(self):
        return len(self.rows) * self.minimum_row_weight > CACHE_BUDGET

    def index_snapshot(self):
        if self.snapshot is None:
            self.snapshot = IndexSnapshot(self.rows.values())
        return self.snapshot

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
            if table.snapshot is not None and any(offset is not None for offset in changes.values()):
                table.blocks = table.axis = table.snapshot = table.matrix = None
                table.scalar_cache = None
            for key, offset in changes.items():
                if offset is None:
                    previous = table.rows.pop(key, None)
                    if previous is not None and table.snapshot is not None:
                        table.snapshot.discard(previous)
                else:
                    table.rows[key] = offset
            if table.snapshot is not None and table.snapshot.deleted_count >= len(table.rows):
                table.blocks = table.axis = table.snapshot = table.matrix = None
                table.scalar_cache = None


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
        self.budget = MemoryBudget(memory_limit_bytes())
        # The row cache keeps its historical default unless the container limit
        # is small enough that it would crowd out the matrix and the groups.
        self.cache_budget = min(CACHE_BUDGET, self.budget.share(
            0.40, CACHE_BUDGET, 16 * 1024 * 1024))
        self.committed_end = self.recover()
        self.truncate(self.committed_end)

    def recover(self):
        transaction = Transaction()
        end = 0
        size = os.fstat(self.reader.fileno()).st_size
        offset = 0
        while offset + LENGTH.size <= size:
            length, = LENGTH.unpack(self.reader.read(LENGTH.size))
            if not 0 < length <= size - offset - LENGTH.size:
                break
            next_offset = offset + LENGTH.size + length
            tag = self.reader.read(1)
            if tag == COMMIT and length == 1:
                transaction.publish(self.tables)
                transaction = Transaction()
                end = next_offset
            elif tag == b"r":
                # Only the table and primary key are needed to rebuild row offsets.
                header_size, = LENGTH.unpack(self.reader.read(LENGTH.size))
                if header_size > length - LENGTH.size - 1:
                    raise ValueError("invalid log row header")
                name, key = marshal.loads(self.reader.read(header_size))
                transaction.apply((("row", (name, key, offset)),))
                self.reader.seek(next_offset)
            elif tag == b"m":
                transaction.apply((marshal.loads(self.reader.read(length - 1)),))
            else:
                raise ValueError("invalid log frame")
            offset = next_offset
        return end

    def truncate(self, offset):
        self.writer.flush()
        self.writer.truncate(offset)
        self.log_end = self.writer.seek(0, os.SEEK_END)
        # A buffered reader may retain bytes beyond the truncated position.
        self.reader.close()
        self.reader = self.path.open("rb", buffering=128 * 1024)

    def frame(self, payload):
        # The buffered writer is append-only; truncate resets the logical end.
        offset = self.log_end
        self.writer.write(LENGTH.pack(len(payload)))
        self.writer.write(payload)
        self.log_end += LENGTH.size + len(payload)
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
        start = self.log_end
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
            self.evict_replaced(updates)
            self.pending.tables.update(updates.tables)
            self.pending.indexes.update(updates.indexes)
            for name, changes in updates.rows.items():
                self.pending.rows.setdefault(name, {}).update(changes)
        return count

    def evict_replaced(self, transaction):
        # Obsolete offsets must not occupy the cache retained across scans.
        if not self.cache:
            return
        for name, changes in transaction.rows.items():
            table = self.tables.get(name)
            committed = table.rows if table is not None else {}
            pending = self.changes(name)
            for key in changes:
                old = pending.get(key, committed.get(key))
                cached = self.cache.pop(old, None)
                if cached is not None:
                    self.cache_bytes -= cached[0]

    def durable_commit(self, transaction, abort_offset):
        try:
            self.frame(COMMIT)
            self.writer.flush()
            os.fsync(self.writer.fileno())
        except OSError:
            self.truncate(abort_offset)
            raise
        self.committed_end = self.log_end
        if self.pending is None:
            self.evict_replaced(transaction)
        transaction.publish(self.tables)

    def begin(self):
        require(self.pending is None, "transaction_active", "transaction already active")
        self.pending = Transaction()

    def commit(self):
        if self.pending is None:
            # MySQL semantics: COMMIT without an active transaction is a no-op
            # success.  A driver that ends every batch with COMMIT must not see
            # an error, and the protocol only requires id/ok for transaction
            # control statements.
            return
        self.durable_commit(self.pending, self.log_end)
        self.pending = None

    def rollback(self):
        if self.pending is None:
            # Same reasoning as commit(): nothing is open, so there is nothing
            # to undo and no error to report.
            return
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

    def read_row(self, table, offset, scan=False, cache=True, packed=False):
        """Decode one row.  packed=True keeps vector columns as float32 bytes.

        The bulk builders feed numpy directly, so unpacking 128 floats into a
        Python tuple only to convert them back costs more than the whole scan;
        with packed=True they can call numpy.frombuffer on the stored bytes.
        """
        cached = self.cache.get(offset) if not packed else None
        if cached is not None:
            if not scan:
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
                if not packed:
                    values[i] = struct.unpack(f"<{width}f", values[i])
                weight += 32 * width
        # Large scans may fill spare capacity, but must not evict cached rows.
        # cache=False is used by bulk index builds that keep their own compact
        # representation, so the same vectors are not stored twice.
        limit = self.cache_budget
        if cache and not packed and weight <= limit and (
                not scan or self.cache_bytes + weight <= limit):
            while self.cache_bytes + weight > limit:
                _, (removed, _) = self.cache.popitem(last=False)
                self.cache_bytes -= removed
            self.cache[offset] = weight, values
            self.cache_bytes += weight
        return values
