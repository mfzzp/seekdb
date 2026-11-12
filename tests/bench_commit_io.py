"""Equal-data SQL write workload; optional strace records rows.log syscalls."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import sys
import time

parser = argparse.ArgumentParser()
parser.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[1])
parser.add_argument('--data-root', type=Path, required=True)
parser.add_argument('--rows', type=int, default=1000)
parser.add_argument('--batch', type=int, default=100)
parser.add_argument('--dimensions', type=int, default=128)
args = parser.parse_args()
sys.path.insert(0, str(args.source_root))
from minivectordb import Database
assert args.rows > 0 and args.batch > 0 and args.dimensions > 0
args.data_root.mkdir(parents=True, exist_ok=False)
values = []
for i in range(args.rows):
    vector = ','.join(str((i * 17 + j * 7) % 101) for j in range(args.dimensions))
    values.append(f"({i},'text-{i}','[{vector}]')")
db = Database(args.data_root)
started = time.perf_counter()
db.execute(f'CREATE TABLE t(id BIGINT PRIMARY KEY,content VARCHAR(64),v VECTOR({args.dimensions}))', 'exact')
for offset in range(0, args.rows, args.batch):
    result = db.execute('INSERT INTO t VALUES ' + ','.join(values[offset:offset + args.batch]), 'exact')
    assert result['affected_rows'] == min(args.batch, args.rows - offset)
seconds = time.perf_counter() - started
db.close()
db = Database(args.data_root)
ids = db.execute('SELECT id FROM t ORDER BY id', 'exact')['rows']
assert ids == [[i] for i in range(args.rows)]
db.close()
print(json.dumps({'rows': args.rows, 'batch': args.batch, 'dimensions': args.dimensions,
                  'seconds_including_create': seconds, 'rows_per_second': args.rows / seconds,
                  'log_bytes': (args.data_root / 'rows.log').stat().st_size,
                  'log_sha256': hashlib.sha256((args.data_root / 'rows.log').read_bytes()).hexdigest(),
                  'peak_rss_kib': resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
                  'recovered_rows': len(ids)}))
