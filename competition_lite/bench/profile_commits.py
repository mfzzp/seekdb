#!/usr/bin/env python3
"""Measure diagnostic sync/copy costs on identical B100 auto/explicit loads."""
import argparse
import json
from pathlib import Path
import statistics
import time
from compare_run import Client, LIVE_CLIENTS, query_sql


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', type=Path, required=True)
    ap.add_argument('--prepared', type=Path, required=True)
    ap.add_argument('--root', type=Path, required=True)
    ap.add_argument('--image', required=True)
    ap.add_argument('--engine-env',action='append',default=[])
    a = ap.parse_args()
    a.root.mkdir(parents=True, exist_ok=False)
    meta = json.loads((a.prepared/'prepared.json').read_text())
    requests = (a.prepared/'insert.jsonl').read_bytes().splitlines(keepends=True)
    cases = json.loads((a.prepared/'cases.json').read_text())
    report = {'batch': meta['batch'], 'rows': meta['rows'], 'insert_sha256': meta['insert_sha256'], 'engine_env':a.engine_env,'runs': {}}
    try:
        for mode in ['auto', 'explicit']:
            root = a.root/mode
            root.mkdir()
            phases = []
            with (root/'trace.jsonl').open('wb') as log:
                c = Client(str(a.binary.resolve()), root/'data', log, a.image, engine_env=a.engine_env)
                c.ask(meta['schema_sql']);phases.append('schema')
                start_stats = c.metrics();start = time.perf_counter()
                for line in requests:
                    if mode == 'explicit':c.ask('BEGIN;');phases.append('write')
                    c.send(line);phases.append('write')
                    if mode == 'explicit':c.ask('COMMIT;');phases.append('write')
                write_s = time.perf_counter()-start;end_stats = c.metrics()
                for case in cases[:20]:c.ask(query_sql(case));phases.append('query')
                before_begin = c.metrics();c.ask('BEGIN;');phases.append('copy_probe')
                after_begin = c.metrics();c.ask('COMMIT;');phases.append('copy_probe')
                after_commit = c.metrics();c.close();phases.append('shutdown')
            traces = [json.loads(line) for line in (root/'trace.jsonl').read_text().splitlines() if line.startswith('{"diagnostic":')]
            assert len(traces) == len(phases), (mode, len(traces), len(phases))
            groups = {}
            for phase in sorted(set(phases)):
                selected = [t for p,t in zip(phases,traces) if p == phase]
                stats = {}
                for t in selected:
                    for name, item in t['stats'].items():
                        s = stats.setdefault(name, {k: 0 for k in item})
                        for k,v in item.items():s[k] += v
                groups[phase] = {'requests': len(selected), 'engine_request_ms': sum(t['request_ms'] for t in selected), 'stats': stats}
            report['runs'][mode] = {'write_wall_s': write_s, 'rows_per_s': meta['rows']/write_s,
                'groups': groups, 'resources': {'write_start': start_stats, 'write_end': end_stats,
                    'before_begin': before_begin, 'after_begin': after_begin, 'after_commit': after_commit}}
            (a.root/'result.json').write_text(json.dumps(report, indent=2)+'\n')
            print(json.dumps({'mode': mode, 'write_wall_s': write_s, 'write': groups['write']}), flush=True)
    finally:
        for client in list(LIVE_CLIENTS):client.abort()


if __name__ == '__main__':
    main()
