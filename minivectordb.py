"""Single-process MiniVectorDB SQL execution and JSON Lines entry point."""

import argparse
from array import array
from bisect import bisect_left
import heapq
from functools import partial
from itertools import chain, islice
import json
import math
import struct
import sys

try:  # optional acceleration; the pure-Python path stays authoritative
    import numpy as _numpy
except Exception:  # pragma: no cover - exercised only when numpy is absent
    _numpy = None

from minivector_sql import COMPARISONS, DISTANCE, Parser, SQLError, require, typed
from minivector_store import Store, adaptive_budget, memory_limit_bytes


MAX_BLOCK_ROWS = 1_000_000
# The first-coordinate axis keeps one tuple per row, so it keeps its own,
# much smaller ceiling than the packed equality groups.
MAX_AXIS_ROWS = 50_000
COMPONENT_BUDGET = adaptive_budget(memory_limit_bytes(), 64 * 1024 * 1024,
                                   8 * 1024 * 1024)
# Rows per chunk when a kernel has to gather or subtract a slice of the matrix;
# bounds the temporary to chunk*dimension*8 bytes instead of the whole table.
MATRIX_CHUNK_ROWS = 4096
# Dense distance matrix acceleration: only worth it for large tables, and capped
# so a huge table cannot turn the cache into an unbounded allocation.
MATRIX_MIN_ROWS = 2048
MATRIX_MAX_BYTES = adaptive_budget(memory_limit_bytes(), 192 * 1024 * 1024,
                                   32 * 1024 * 1024)
# Above this coordinate magnitude the |v|^2 - 2v.q + |q|^2 identity can lose
# significant digits to cancellation, so the direct difference kernel is used.
IDENTITY_MAX_ABS = 1000.0


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

    def ensure_groups(self, table, component):
        """Build or reuse equality groups (value -> stable slots) for a column.

        Accounting follows the real footprint: four bytes per slot plus one dict
        entry per distinct value. The previous 64-bytes-per-row estimate rejected
        every table beyond a few tens of thousands of rows, which sent filtered
        queries to a full scan.
        """
        if table.blocks is not None and table.blocks[0] == component:
            return table.blocks[1]
        if len(table.rows) > MAX_BLOCK_ROWS:
            return None
        if not self._reserve(4 * len(table.rows) + 72 * 32, keep=table):
            return None
        groups = {}
        budget = COMPONENT_BUDGET
        # Bulk index builds keep their own compact representation, so they must
        # not also fill the row cache with decoded vectors.
        read_row = partial(self.read_row, cache=False)
        for slot, offset in table.index_snapshot().rows():
            field = read_row(table, offset)[component]
            slots = groups.get(field)
            if slots is None:
                budget -= 72
                if budget < 0:
                    return None
                groups[field] = slots = array("I")
            slots.append(slot)
            budget -= 4
            if budget < 0:
                return None
        table.blocks = component, groups
        return groups

    def component_offsets(self, table, component, value):
        groups = self.ensure_groups(table, component)
        if groups is None:
            return None

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
        if filters and all(col == table.primary for col, _, _ in filters):
            return (changes.get(key, table.rows.get(key))
                    for key in self.matching_primary_keys(name, table, filters))
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
        offsets = self.candidates(name, table, filters, clustered)
        scan = table.large_scan() and not any(
            col == table.primary and op == "=" for col, op, _ in filters)
        if scan and clustered and table.blocks is not None:
            component, groups = table.blocks
            if any(col == component and op == "=" and len(groups.get(value, ())) <= 64
                   for col, op, value in filters):
                scan = False
        read_row = partial(self.read_row, scan=True) if scan else self.read_row
        if not checks:
            for offset in offsets:
                yield read_row(table, offset)
        elif len(checks) == 1:
            col, compare, value = checks[0]
            for offset in offsets:
                row = read_row(table, offset)
                if compare(row[col], value):
                    yield row
        else:
            for offset in offsets:
                row = read_row(table, offset)
                if all(compare(row[col], value) for col, compare, value in checks):
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
                old = list(current)
                row = list(current)
                for i, (_, kind, width) in enumerate(table.columns):
                    if kind == "vector":
                        # Compare in the stored packed form; the SQL text is
                        # already float32 so this comparison is exact.
                        old[i] = struct.pack(f"<{width}f", *old[i])
                        row[i] = updates[i] if i in updates else old[i]
                    elif i in updates:
                        row[i] = updates[i]
                if row == old:
                    continue
                yield "put", (name, row[table.primary], row)

        return {"affected_rows": self.mutate(operations())}

    def matching_primary_keys(self, name, table, filters):
        """Evaluate key-only predicates using the transaction-visible directory."""
        changes = self.changes(name)
        equality = next((value for _, op, value in filters if op == "="), None)
        if equality is not None:
            keys = (equality,) if changes.get(equality, table.rows.get(equality)) is not None else ()
        elif changes:
            keys = chain(
                (key for key in table.rows if key not in changes),
                (key for key, offset in changes.items() if offset is not None),
            )
        else:
            keys = iter(table.rows)
        checks = [(COMPARISONS[op], value) for _, op, value in filters]
        if not checks:
            return keys
        if len(checks) == 1:
            compare, value = checks[0]
            return (key for key in keys if compare(key, value))
        return (key for key in keys if all(compare(key, value) for compare, value in checks))

    def delete(self, name, terms):
        name = name.casefold()
        table = self.table(name)
        filters = self.filters(table, terms)
        if all(column == table.primary for column, _, _ in filters):
            keys = self.matching_primary_keys(name, table, filters)
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
                read_row = partial(self.read_row, scan=True) if table.large_scan() else self.read_row
                for slot, offset in snapshot.rows():
                    row = read_row(table, offset)
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

    def _memory_used(self):
        """Bytes currently held by rebuildable structures plus the row cache."""
        used = self.cache_bytes
        for table in self.tables.values():
            matrix = table.matrix
            if matrix is not None:
                used += matrix[2].nbytes + matrix[3].nbytes + matrix[4].nbytes
            blocks = table.blocks
            if blocks is not None:
                groups = blocks[1]
                used += 72 * len(groups)
                for slots in groups.values():
                    used += 4 * len(slots)
        return used

    def _release_derived(self, keep=None):
        """Drop rebuildable structures (all but `keep`) before giving up."""
        for table in self.tables.values():
            if table is keep:
                continue
            table.matrix = None
            table.blocks = None
            table.axis = None

    def _reserve(self, size, keep=None):
        """True when `size` more bytes fit, dropping other tables' caches first."""
        budget = self.budget
        if budget.capacity is None or budget.fits(self._memory_used(), size):
            return True
        self._release_derived(keep)
        return budget.fits(self._memory_used(), size)

    def _direct_squared(self, values, query, indices=None):
        """|v-q|^2 with a bounded temporary: never copies the whole matrix."""
        step = MATRIX_CHUNK_ROWS
        count = values.shape[0] if indices is None else indices.size
        out = _numpy.empty(count, dtype=_numpy.float64)
        for start in range(0, count, step):
            stop = min(start + step, count)
            block = values[start:stop] if indices is None else values[indices[start:stop]]
            diff = block - query
            out[start:stop] = _numpy.einsum("ij,ij->i", diff, diff)
        return out

    def _identity_squared(self, values, norms, query, indices=None):
        """Identity kernel; a subset is gathered in chunks to bound the copy."""
        step = MATRIX_CHUNK_ROWS
        query_norm_sq = float(query @ query)
        if indices is None:
            squared = norms - 2.0 * (values @ query) + query_norm_sq
            _numpy.maximum(squared, 0.0, out=squared)
            return squared
        count = indices.size
        out = _numpy.empty(count, dtype=_numpy.float64)
        for start in range(0, count, step):
            stop = min(start + step, count)
            part = indices[start:stop]
            out[start:stop] = norms[part] - 2.0 * (values[part] @ query) + query_norm_sq
        _numpy.maximum(out, 0.0, out=out)
        return out

    def build_matrix(self, table, snapshot, column):
        """Materialise live vectors once so TopK becomes one BLAS mat-vec.

        Values come from the same decoded float32 payloads the row cache uses,
        so the matrix cannot drift from the stored representation.  Returns None
        when the shared memory budget cannot fit the matrix, in which case the
        caller falls back to a scan.
        """
        width = table.columns[column][2]
        rows = len(snapshot.offsets)
        dtype = _numpy.float64
        if rows * width * 8 > MATRIX_MAX_BYTES:
            dtype = _numpy.float32
        if not self._reserve(rows * width * _numpy.dtype(dtype).itemsize, keep=table):
            return None
        values = _numpy.empty((rows, width), dtype=dtype)
        keys = _numpy.empty(rows, dtype=_numpy.int64)
        primary = table.primary
        read_row = partial(self.read_row, cache=False, packed=True)
        for slot, offset in snapshot.rows():
            row = read_row(table, offset)
            values[slot] = _numpy.frombuffer(row[column], dtype="<f4")
            keys[slot] = row[primary]
        norms = _numpy.einsum("ij,ij->i", values, values)
        maxabs = float(_numpy.max(_numpy.abs(values))) if rows else 0.0
        norms_max = float(norms.max()) if rows else 0.0
        table.matrix = (snapshot, column, values, keys, norms, maxabs, norms_max)
        return table.matrix

    @staticmethod
    def identity_error_margin(norms_max, query_norm_sq, width):
        """Absolute error bound of |v|^2 - 2 v.q + |q|^2 in float64.

        Summing `width` squares and the BLAS dot each accumulate a small
        multiple of eps times the operated magnitudes, so the squared distance
        carries an absolute error of roughly factor*(||v|| + ||q||)^2.  The
        factor below multiplies the usual n*eps bound by 4 and the caller widens
        the window again by 2, because the cancellation is what turns a tiny
        relative error into a large absolute one when the vectors are close.
        """
        factor = 4.0 * width * 2.220446049250313e-16
        return factor * (math.sqrt(norms_max) + math.sqrt(query_norm_sq)) ** 2

    def select_exact(self, values, query, keys, squared, row_index, limit, margin):
        """Exact TopK: the approximate ranking only proposes candidates.

        With a positive `margin` the squared distances came from the identity.
        Every row within 2*margin of the K-th threshold is recomputed with the
        direct difference kernel before ordering, so a true TopK member cannot be
        excluded: its approximate value differs from its exact value by at most
        `margin`, hence it lies inside the widened window.  When the window grows
        past a small multiple of K the whole set is recomputed instead.
        """
        total = squared.size
        if not total or not limit:
            return [], _numpy.empty(0)
        best = min(limit, total)
        if best < total:
            candidates = _numpy.argpartition(squared, best - 1)[:best]
            threshold = squared[candidates].max()
            window = threshold + 2.0 * margin if margin > 0.0 else threshold
            pool = _numpy.flatnonzero(squared <= window)
        else:
            pool = _numpy.arange(total)
        rows = row_index[pool] if row_index is not None else pool
        if margin > 0.0:
            if pool.size > max(8 * best, 512):
                rows = row_index if row_index is not None else _numpy.arange(total)
                pool = _numpy.arange(total)
            pool_sq = self._direct_squared(values, query, rows)
        else:
            pool_sq = squared[pool]
        order = _numpy.lexsort((keys[rows], pool_sq))[:best]
        return rows[order], _numpy.sqrt(_numpy.maximum(pool_sq[order], 0.0))

    def _chunked_top_k(self, table, snapshot, column, target, limit, slots=None,
                       chunk_rows=None):
        """Exact TopK without materialising the table.

        Used when the shared memory budget refuses a dense matrix.  Rows are
        decoded in chunks, each chunk is ranked exactly (identity candidates plus
        the direct rerank from select_exact) and only the running best `limit`
        results are carried between chunks, so the temporary is bounded by the
        chunk size instead of the table size.
        """
        if not limit:
            return []
        numpy = _numpy
        width = table.columns[column][2]
        primary = table.primary
        offsets = snapshot.offsets
        if slots is None:
            slot_list = [slot for slot, _ in snapshot.rows()]
        else:
            slot_list = [int(slot) for slot in slots]
        if not slot_list:
            return []
        step = chunk_rows or (MATRIX_CHUNK_ROWS * 2)
        query = numpy.asarray(target, dtype=numpy.float64)
        query_norm_sq = float(query @ query)
        best = []                       # (distance, primary key, slot)
        read_row = partial(self.read_row, cache=False, packed=True)
        for start in range(0, len(slot_list), step):
            part = slot_list[start:start + step]
            block = numpy.empty((len(part), width), dtype=numpy.float64)
            keys = numpy.empty(len(part), dtype=numpy.int64)
            for index, slot in enumerate(part):
                row = read_row(table, offsets[slot])
                block[index] = numpy.frombuffer(row[column], dtype="<f4")
                keys[index] = row[primary]
            norms = numpy.einsum("ij,ij->i", block, block)
            maxabs = float(numpy.max(numpy.abs(block))) if norms.size else 0.0
            if maxabs <= IDENTITY_MAX_ABS:
                squared = norms - 2.0 * (block @ query) + query_norm_sq
                numpy.maximum(squared, 0.0, out=squared)
                margin = self.identity_error_margin(float(norms.max()),
                                                   query_norm_sq, width)
            else:
                squared = self._direct_squared(block, query)
                margin = 0.0
            rows, distances = self.select_exact(block, query, keys, squared, None,
                                                limit, margin)
            for distance, row_index in zip(distances.tolist(), rows.tolist()):
                best.append((distance, int(keys[row_index]), part[row_index]))
            if len(best) > limit:
                best.sort()
                del best[limit:]
        return [(self.read_row(table, offsets[slot]), (float(distance),))
                for distance, _, slot in best]

    def matrix_top_k(self, table, column, target, limit):
        """Exact TopK over the dense matrix; ties still break on primary key."""
        if not limit:
            return []
        snapshot = table.index_snapshot()
        matrix = table.matrix
        if matrix is None or matrix[0] is not snapshot or matrix[1] != column:
            matrix = self.build_matrix(table, snapshot, column)
            if matrix is None:
                return self._chunked_top_k(table, snapshot, column, target, limit)
        _, _, values, keys, norms, maxabs, norms_max = matrix
        query = _numpy.asarray(target, dtype=values.dtype)
        query_norm_sq = float(query @ query)
        width = values.shape[1]
        if maxabs <= IDENTITY_MAX_ABS:
            # |v-q|^2 = |v|^2 - 2 v.q + |q|^2 costs one BLAS mat-vec instead of
            # an element-wise pass that allocates a matrix-sized temporary; the
            # ranking it produces is only used to choose candidates.
            squared = self._identity_squared(values, norms, query)
            margin = self.identity_error_margin(norms_max, query_norm_sq, width)
        else:
            squared = self._direct_squared(values, query)
            margin = 0.0
        if snapshot.deleted_count:
            bits = _numpy.unpackbits(_numpy.frombuffer(bytes(snapshot.deleted),
                                                       dtype=_numpy.uint8))
            # Bit set means the stable slot was invalidated by a delete.
            live = _numpy.flatnonzero(bits[:squared.size] == 0)
            ordered, row_index = squared[live], live
        else:
            ordered, row_index = squared, None
        rows, distances = self.select_exact(values, query, keys, ordered, row_index,
                                            limit, margin)
        offsets = snapshot.offsets
        return [(self.read_row(table, offsets[int(slot)]), (float(distance),))
                for slot, distance in zip(rows, distances)]

    def matrix_group_top_k(self, table, column, target, limit, filter_column, filter_value):
        """Filtered TopK: distances only for the equality group's slots.

        Returns None when the equality groups cannot be built, so the caller can
        fall back to the generic filter path unchanged.
        """
        if not limit:
            return []
        groups = self.ensure_groups(table, filter_column)
        if groups is None:
            return None
        slots = groups.get(filter_value)
        if not slots:
            return []
        snapshot = table.index_snapshot()
        subset = _numpy.frombuffer(slots, dtype=_numpy.uint32).astype(_numpy.int64)
        if snapshot.deleted_count:
            alive = _numpy.fromiter((snapshot.is_live(int(slot)) for slot in subset),
                                    dtype=bool, count=subset.size)
            subset = subset[alive]
            if not subset.size:
                return []
        matrix = table.matrix
        if matrix is None or matrix[0] is not snapshot or matrix[1] != column:
            matrix = self.build_matrix(table, snapshot, column)
            if matrix is None:
                return self._chunked_top_k(table, snapshot, column, target, limit,
                                           slots=subset)
        _, _, values, keys, norms, maxabs, norms_max = matrix
        query = _numpy.asarray(target, dtype=values.dtype)
        query_norm_sq = float(query @ query)
        width = values.shape[1]
        if maxabs <= IDENTITY_MAX_ABS:
            squared = self._identity_squared(values, norms, query, subset)
            margin = self.identity_error_margin(norms_max, query_norm_sq, width)
        else:
            squared = self._direct_squared(values, query, subset)
            margin = 0.0
        rows, distances = self.select_exact(values, query, keys, squared, subset,
                                            limit, margin)
        offsets = snapshot.offsets
        return [(self.read_row(table, offsets[int(slot)]), (float(distance),))
                for slot, distance in zip(rows, distances)]

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

        if (all(column == table.primary for column, _, _ in filters)
                and all(column == table.primary for column, _ in ordering)):
            keys = self.matching_primary_keys(name, table, filters)
            if ordering:
                descending = ordering[0][1]
                if limit is None:
                    keys = sorted(keys, reverse=descending)
                else:
                    top = heapq.nlargest if descending else heapq.nsmallest
                    keys = top(min(limit, sys.maxsize), keys)
            elif limit is not None:
                keys = islice(keys, min(limit, sys.maxsize))
            if all(column == table.primary for column in projected):
                return {"columns": names, "rows": [[key] * len(projected) for key in keys]}
            # Rank/filter keys before fetching payloads or computing projected distances.
            changes = self.changes(name)
            result = []
            scan = (table.large_scan() and (limit is None or limit > 64)
                    and not any(op == "=" for _, op, _ in filters))
            read_row = partial(self.read_row, scan=True) if scan else self.read_row
            for key in keys:
                row = read_row(table, changes.get(key, table.rows.get(key)))
                distances = tuple(math.dist(row[column], target) for column, target in evaluators)
                result.append([row[bound] if bound >= 0 else distances[-bound - 1]
                               for bound in projected])
            return {"columns": names, "rows": result}

        clustered = bool(ordering and ordering[0][0] < 0)
        records = self.matching(name, table, filters, clustered)

        def values(row):
            return tuple(math.dist(row[column], target) for column, target in evaluators)

        distance_order = any(bound < 0 for bound, _ in ordering)

        def ranked():
            for row in records:
                distances = values(row) if distance_order else ()
                rank = []
                for bound, descending in ordering:
                    value = row[bound] if bound >= 0 else distances[-bound - 1]
                    rank.append(Descending(value) if descending else value)
                rank.append(row[table.primary])
                yield tuple(rank), row, distances

        rows = None
        if (_numpy is not None and not self.changes(name)
                and len(ordering) == 1 and ordering[0][0] < 0
                and not ordering[0][1] and len(evaluators) == 1
                and limit is not None
                and len(table.rows) >= MATRIX_MIN_ROWS
                and len(table.rows) * table.columns[evaluators[0][0]][2] * 4
                <= MATRIX_MAX_BYTES):
            if not filters:
                rows = self.matrix_top_k(table, evaluators[0][0], evaluators[0][1], limit)
            elif (len(filters) == 1 and filters[0][1] == "="
                    and filters[0][0] != table.primary):
                rows = self.matrix_group_top_k(table, evaluators[0][0], evaluators[0][1],
                                               limit, filters[0][0], filters[0][2])
        if rows is None:
            if (mode == "ann" and clustered and len(ordering) == len(evaluators) == 1
                    and not ordering[0][1] and limit is not None and not filters
                    and table.index is not None and not self.changes(name)
                    and len(table.rows) <= MAX_AXIS_ROWS and len(evaluators[0][1]) <= 8):
                rows = self.axis_top_k(table, evaluators[0], limit)
            elif (clustered and len(ordering) == len(evaluators) == 1
                    and not ordering[0][1] and limit is not None):
                column, target = evaluators[0]
                rows = self.distance_top_k(records, column, target, table.primary, limit)
            elif ordering:
                if limit is None:
                    winners = sorted(ranked(), key=operator_key)
                else:
                    winners = heapq.nsmallest(min(limit, sys.maxsize), ranked(),
                                              key=operator_key)
                rows = [(row, distances if distance_order else values(row))
                        for _, row, distances in winners]
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


def shutdown_quietly(db):
    """Flush and close, but never let a cleanup failure swallow the response.

    In this design every commit has already been fsynced and uncommitted frames
    are simply truncated at the next recovery, so a failure while closing cannot
    lose acknowledged data; it is a resource-cleanup failure, not a durability
    one, and must not change the protocol answer or the exit status.
    """
    try:
        db.close()
    except Exception as exc:  # noqa: BLE001 - the protocol answer wins
        print(f"shutdown cleanup failed: {exc}", file=sys.stderr, flush=True)


def startup_diagnostics(db):
    """One stderr line describing the runtime; stdout stays protocol-only."""
    parts = [f"python {sys.version.split()[0]}"]
    if _numpy is None:
        parts.append("numpy=absent (pure-Python distance path)")
    else:
        parts.append(f"numpy={_numpy.__version__}")
    parts.append(f"matrix_cap={MATRIX_MAX_BYTES >> 20}MiB")
    capacity = getattr(db, "budget", None)
    if capacity is not None and capacity.capacity is not None:
        parts.append(f"memory_budget={capacity.capacity >> 20}MiB")
    else:
        parts.append("memory_budget=unlimited")
    print("minivectordb: " + ", ".join(parts), file=sys.stderr, flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--data-dir", default="/data")
    db = Database(parser.parse_args().data_dir)
    try:
        startup_diagnostics(db)
        send({"type": "ready"})
        for line in sys.stdin.buffer:
            request = {}
            try:
                request = json.loads(line)
                require(isinstance(request, dict), "invalid_request", "expected an object")
                operation = request.get("op")
                if operation == "shutdown":
                    shutdown_quietly(db)
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
            shutdown_quietly(db)


if __name__ == "__main__":
    main()
