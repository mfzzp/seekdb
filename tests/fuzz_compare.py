"""Differential fuzz test: same seed, two source revisions, compare results.

Usage:
    python3 scripts/fuzz_compare.py <source_dir> <seed> <output.json>
    ...
    python3 scripts/compare_fuzz.py <a.json> <b.json>
"""

import json
import math
import random
import struct
import sys
import tempfile

sys.path.insert(0, sys.argv[1])
from minivectordb import Database  # noqa: E402

SEED = int(sys.argv[2])
OUT = sys.argv[3]
DIM = 4
CATEGORIES = ["red", "green", "blue", "black"]

rng = random.Random(SEED)
root = tempfile.mkdtemp(prefix="fuzz-")
db = Database(root)
db.execute(f"CREATE TABLE t (id BIGINT PRIMARY KEY,g BIGINT,s VARCHAR(16),"
           f"v VECTOR({DIM}))", "exact")
db.execute("CREATE VECTOR INDEX vi ON t(v)", "exact")

# Duplicated vectors on purpose so distance ties and the primary-key tie-break
# are exercised, including exact duplicates and symmetric points.
base_vectors = [[0.0] * DIM, [1.0, 0.0, 0.0, 0.0], [-1.0, 0.0, 0.0, 0.0],
                [0.0, 1.0, 0.0, 0.0], [0.5, 0.5, 0.0, 0.0]]
rows = []
for key in range(3000):
    if key % 7 == 0:
        vector = list(base_vectors[key % len(base_vectors)])
    else:
        vector = [round(rng.gauss(0, 1), 6) for _ in range(DIM)]
    rows.append((key - 1500, key % 5, CATEGORIES[key % len(CATEGORIES)], vector))
batch = []
for pk, group, text, vector in rows:
    literal = "[" + ",".join(str(value) for value in vector) + "]"
    batch.append(f"({pk},{group},'{text}','{literal}')")
    if len(batch) == 500:
        db.execute("INSERT INTO t VALUES " + ",".join(batch), "exact")
        batch = []
if batch:
    db.execute("INSERT INTO t VALUES " + ",".join(batch), "exact")

db.execute("DELETE FROM t WHERE id < -1400", "exact")

targets = [base_vectors[0], base_vectors[1],
           [round(rng.gauss(0, 1), 6) for _ in range(DIM)],
           [0.25, -0.25, 0.5, 0.5]]

results = []
for step in range(120):
    target = targets[step % len(targets)]
    literal = "[" + ",".join(str(value) for value in target) + "]"
    limit = [0, 1, 3, 10, 50, 400][step % 6]
    kind = step % 8
    if kind == 0:
        sql = f"SELECT id,L2_DISTANCE(v,'{literal}') AS d FROM t ORDER BY d LIMIT {limit}"
    elif kind == 1:
        sql = (f"SELECT id,L2_DISTANCE(v,'{literal}') AS d FROM t "
               f"WHERE g = {step % 5} ORDER BY d LIMIT {limit}")
    elif kind == 2:
        sql = (f"SELECT id,L2_DISTANCE(v,'{literal}') AS d FROM t "
               f"WHERE s = '{CATEGORIES[step % 4]}' ORDER BY d LIMIT {limit}")
    elif kind == 3:
        sql = f"SELECT id,g FROM t ORDER BY id DESC LIMIT {limit}"
    elif kind == 4:
        sql = (f"SELECT * FROM t WHERE g = {step % 5} ORDER BY id LIMIT {limit}")
    elif kind == 5:
        sql = f"SELECT id,L2_DISTANCE(v,'{literal}') AS d FROM t ORDER BY d LIMIT {limit}"
    elif kind == 6:
        sql = (f"SELECT id,L2_DISTANCE(v,'{literal}') AS dx,L2_DISTANCE(v,'{literal}') AS dy "
               f"FROM t WHERE g >= {step % 5} ORDER BY dx LIMIT {limit}")
    else:
        sql = (f"SELECT id,L2_DISTANCE(v,'{literal}') AS d FROM t "
               f"WHERE s <> '{CATEGORIES[step % 4]}' ORDER BY d LIMIT {limit}")
    mode = "ann" if step % 3 == 0 else "exact"
    try:
        results.append({"sql": sql, "mode": mode, "result": db.execute(sql, mode)})
    except Exception as exc:  # noqa: BLE001
        results.append({"sql": sql, "mode": mode,
                        "error": getattr(exc, "code", type(exc).__name__)})
db.close()
json.dump(results, open(OUT, "w"), ensure_ascii=False)
print(f"wrote {len(results)} query results to {OUT}")
