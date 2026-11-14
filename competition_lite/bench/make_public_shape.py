#!/usr/bin/env python3
"""Local synthetic data with published benchmark dimensions, NOT the official generator."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import struct


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--rows', type=int, default=100000)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--seed', type=int, default=20261111)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    rng = random.Random(args.seed)
    with (args.out / 'vectors.f32').open('wb') as vf, (args.out / 'rows.jsonl').open('w') as rf:
        for i in range(args.rows):
            vf.write(struct.pack('<128f', *[rng.gauss(0, .5) for _ in range(128)]))
            rf.write(json.dumps({'id': i + 1, 'category': 'cat%d' % (i % 20), 'content': 'doc%d' % i}) + '\n')
    (args.out / 'load_order.u32').write_bytes(b''.join(struct.pack('<I', i + 1) for i in range(args.rows)))
    rng = random.Random(args.seed + 1)
    with (args.out / 'query_cases.jsonl').open('w') as qf:
        for i in range(500):
            vector = [rng.gauss(0, .5) for _ in range(128)]
            for filtered in [False, True]:
                qf.write(json.dumps({'id': 2*i + int(filtered), 'filter': 'category' if filtered else 'none',
                                     'category': 'cat%d' % (i % 20), 'vector': vector, 'k': 10}) + '\n')
    manifest = {'rows': args.rows, 'dimension': 128, 'schema_columns': 4, 'seed': args.seed,
                'distribution': 'local independent N(0,0.5^2), 20 balanced categories; official generator unavailable',
                'source': 'published parameters and SQL example; synthetic generator is ours, not organizer code',
                'spec_url': 'https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/lg8enxlmwg69unt6'}
    for name in ['vectors.f32', 'rows.jsonl', 'load_order.u32', 'query_cases.jsonl']:
        manifest.setdefault('sha256', {})[name] = hashlib.sha256((args.out/name).read_bytes()).hexdigest()
    (args.out/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print(json.dumps(manifest), flush=True)


if __name__ == '__main__':
    main()
