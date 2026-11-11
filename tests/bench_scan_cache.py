"""Compare source revisions with native query timings and separate cache counters.

Usage: python3 tests/bench_scan_cache.py SOURCE_DIRECTORY [ROWS]
Run each revision in a fresh process and alternate their order across repetitions.
"""
import hashlib
import json
import resource
import struct
import sys
import tempfile
import time

sys.path.insert(0, sys.argv[1])
from minivectordb import Database

size = int(sys.argv[2]) if len(sys.argv) > 2 else 30000
with tempfile.TemporaryDirectory() as root:
    db = Database(root)
    try:
        db.execute('CREATE TABLE t (id BIGINT PRIMARY KEY,g BIGINT,v VECTOR(128))', 'exact')
        db.mutate(('put', ('t', k, [k, k % 20, struct.pack('<128f', *([k % 997 / 997] * 128))]))
                  for k in range(size))
        query = "SELECT id,L2_DISTANCE(v,'" + str([0] * 128) + "') AS d FROM t ORDER BY d LIMIT 10"
        db.execute(query, 'exact')
        runs = []
        for _ in range(7):
            start = time.perf_counter()
            result = db.execute(query, 'exact')
            runs.append({'seconds': time.perf_counter() - start,
                         'digest': hashlib.sha256(json.dumps(result).encode()).hexdigest()})
        # Count decoding on a separate, untimed query to avoid measuring wrapper overhead.
        original = db.read_row
        misses = [0]
        def observed(table, offset, **kwargs):
            if offset not in db.cache:
                misses[0] += 1
            return original(table, offset, **kwargs)
        db.read_row = observed
        observed_result = db.execute(query, 'exact')
        assert observed_result == result
        db.read_row = original
        print(json.dumps({'source': sys.argv[1], 'rows': size, 'runs': runs,
                          'untimed_misses': misses[0], 'cached_rows': len(db.cache),
                          'cache_bytes': db.cache_bytes,
                          'maxrss_kib': resource.getrusage(resource.RUSAGE_SELF).ru_maxrss}), flush=True)
    finally:
        db.close()
