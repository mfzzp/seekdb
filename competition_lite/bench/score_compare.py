#!/usr/bin/env python3
"""Local paired E comparison using measured device I/O, never an official score.

Each baseline/candidate pair is one independent round. Write B covers write_start
to write_end. Query B covers initial to after_shutdown, including load, prepare,
warmup and checkpoint; query T is query_wall_s. Query M is the whole-run peak.
These local harness boundaries are explicit assumptions, not the frozen scorer.
"""
import argparse
import json
from pathlib import Path
import statistics


def device_bytes(snapshot):
    io = snapshot['io']
    if io is None:
        raise ValueError('Physical device io.stat unavailable; cannot compute E')
    data = io.get(snapshot['device'], {})
    return data.get('rbytes', 0) + data.get('wbytes', 0)


def metrics(report):
    assert report['restart_pass'] and all(v['passed'] for v in report['verification'].values())
    r = report['resources']
    assert len({s['device'] for s in r.values()}) == 1
    result = {
        'write': {'T': report['insert_wall_s'],
                  'B': device_bytes(r['write_end']) - device_bytes(r['write_start']),
                  'M': r['write_end']['memory_peak_bytes']},
        'query': {'T': report['query_wall_s'],
                  'B': device_bytes(r['after_shutdown']) - device_bytes(r['initial']),
                  'M': r['after_shutdown']['memory_peak_bytes']}}
    assert all(v >= 0 for m in result.values() for v in m.values())
    return result


def compare(baseline, candidate):
    old, new = metrics(baseline), metrics(candidate)
    weights = {'T': .6, 'B': .2, 'M': .2}
    eps = {'T': .001, 'B': 1048576, 'M': 1048576}
    scores = {}
    for phase in old:
        parts = {k: (old[phase][k]+eps[k]) / (old[phase][k]+new[phase][k]+2*eps[k]) for k in weights}
        scores[phase] = {'f_ratios': parts, 'E': sum(weights[k]*parts[k] for k in weights)}
    return {'baseline': old, 'candidate': new, 'phases': scores,
            'E_local': (scores['write']['E']+2*scores['query']['E'])/3}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline', type=Path, action='append', required=True)
    ap.add_argument('--candidate', type=Path, action='append', required=True)
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    assert len(a.baseline) == len(a.candidate)
    rounds = [compare(json.loads(b.read_text()), json.loads(c.read_text())) for b,c in zip(a.baseline,a.candidate)]
    result = {'not_official_score': True, 'reference': 'paired local baseline; self E=0.5',
              'boundaries': __doc__, 'rounds': rounds,
              'median_E_local': round(statistics.median(r['E_local'] for r in rounds), 6)}
    a.out.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
