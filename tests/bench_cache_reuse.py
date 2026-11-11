import argparse
import hashlib
import json
import os
import pathlib
import random
import statistics
import struct
import sys
import time

parser = argparse.ArgumentParser()
parser.add_argument('source')
parser.add_argument('data')
parser.add_argument('--rows', type=int, default=12000)
parser.add_argument('--width', type=int, default=128)
parser.add_argument('--distribution', choices=['uniform', 'clustered'], default='uniform')
args = parser.parse_args()
sys.path.insert(0, args.source)
from minivectordb import Database


def io():
    return {k: int(v) for k, v in (line.split(':') for line in pathlib.Path('/proc/self/io').read_text().splitlines())}


os.sched_setaffinity(0, sorted(os.sched_getaffinity(0))[:2])
root = pathlib.Path(args.data)
root.mkdir(exist_ok=False)
db = Database(root)
db.execute(f'CREATE TABLE t (id BIGINT PRIMARY KEY,g BIGINT,v VECTOR({args.width}))', 'exact')
rng = random.Random(43827)
def vector(i):
    if args.distribution == 'clustered':
        return [rng.gauss((i % 13) * 0.3, 0.05) for _ in range(args.width)]
    return [rng.uniform(-1, 1) for _ in range(args.width)]
db.mutate(('put', ('t', i, [i, i % 17, struct.pack(f'<{args.width}f', *vector(i))])) for i in range(args.rows))
db.execute('CREATE VECTOR INDEX vi ON t(v)', 'exact')
counts = dict(read_row_calls=0, decoded_rows=0, cache_hits=0, matrix_builds=0)
original_read = db.read_row
original_build = db.build_matrix


def read(*a, **kw):
    counts['read_row_calls'] += 1
    hit = a[1] in db.cache and not kw.get('packed', False)
    counts['cache_hits' if hit else 'decoded_rows'] += 1
    return original_read(*a, **kw)


def build(*a, **kw):
    counts['matrix_builds'] += 1
    return original_build(*a, **kw)


db.read_row, db.build_matrix = read, build
query = "SELECT id,L2_DISTANCE(v,'" + json.dumps([0.13] * args.width) + "') AS d FROM t {filter} ORDER BY d LIMIT 10"
results = []
for name, where, repeats, mutate in [('first_query', '', 1, False), ('warm', '', 20, False),
                                    ('first_filtered', 'WHERE g=7', 1, False),
                                    ('warm_filtered', 'WHERE g=7', 20, False),
                                    ('write_query', '', 10, True),
                                    ('vector_write_query', '', 10, True)]:
    before, count_before = io(), counts.copy()
    timings, answers = [], []
    for n in range(repeats):
        if mutate:
            assignment = ("v='" + json.dumps([0.13 + n * 0.001] * args.width) + "'"
                          if name == 'vector_write_query' else f'g={n+50}')
            db.execute(f"UPDATE t SET {assignment} WHERE id={n}", 'exact')
        start = time.perf_counter()
        answer = db.execute(query.format(filter=where), 'exact')
        timings.append((time.perf_counter() - start) * 1000)
        answers.append(answer)
    after = io()
    results.append({'case': name, 'queries': repeats, 'median_ms': statistics.median(timings),
                    'max_ms': max(timings), 'io_delta': {k: after[k]-before[k] for k in after},
                    'counts': {k: counts[k]-count_before[k] for k in counts},
                    'answer_digest': hashlib.sha256(json.dumps(answers, sort_keys=True).encode()).hexdigest()})
db.close()
print(json.dumps({'source': args.source, 'data': args.data, 'rows': args.rows, 'width': args.width, 'distribution': args.distribution,
                  'note': 'Process I/O counters: read_bytes is storage I/O; rchar is logical reads. First query is not a system-wide cold-cache test.',
                  'results': results}), flush=True)
