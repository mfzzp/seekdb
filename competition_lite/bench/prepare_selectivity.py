#!/usr/bin/env python3
"""Reuse the same load and query vectors; vary only a primary-key range filter."""
import argparse
import json
from pathlib import Path
import shutil
import struct
from compare_run import sha

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--prepared', type=Path, required=True)
ap.add_argument('--out', type=Path, required=True)
ap.add_argument('--per-group', type=int, default=60)
a = ap.parse_args()
a.out.mkdir(parents=True, exist_ok=False)
meta = json.loads((a.prepared/'prepared.json').read_text())
original = json.loads((a.prepared/'cases.json').read_text())
n, dim = meta['rows'], meta['dim']
groups = [('small', min(n,10)), ('medium', n//10), ('large', n*8//10), ('none', n)]
shutil.copyfile(a.prepared/'insert.jsonl', a.out/'insert.jsonl')
cases = []
with (a.out/'queries.f32').open('wb') as qf, (a.out/'cases.tsv').open('w') as cf:
    for i in range(a.per_group):
        vector = original[i*2]['vector']
        for mask, (name, count) in enumerate(groups):
            case = {'id': len(cases), 'filter': name, 'vector': vector, 'k': 10}
            if name != 'none':case['where'] = f'id<={count}'
            cases.append(case)
            cf.write(f'{case["id"]} {mask} 10\n')
            qf.write(struct.pack(f'<{dim}f', *vector))
(a.out/'masks.u8').write_bytes(b''.join(bytes(int(i<count) for i in range(n)) for _,count in groups))
(a.out/'cases.json').write_text(json.dumps(cases))
meta['query_cases_sha256'] = sha(a.out/'cases.json')
meta['selectivity_groups'] = dict(groups)
(a.out/'prepared.json').write_text(json.dumps(meta, indent=2)+'\n')
