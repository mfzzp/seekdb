#!/usr/bin/env python3
"""Serial, rotating-order comparisons with independent result verification per run."""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--prepared', type=Path, required=True)
    ap.add_argument('--oracle', type=Path, required=True)
    ap.add_argument('--root', type=Path, required=True)
    ap.add_argument('--image', required=True)
    ap.add_argument('--variant', action='append', required=True, help='label=/absolute/path/to/binary')
    ap.add_argument('--rounds', type=int, default=3)
    ap.add_argument('--block-variant', help='Also measure this label with LITE_EXACT_PATH=block')
    ap.add_argument('--variant-env',action='append',default=[],help='label:LITE_NAME=value for a specific variant')
    a = ap.parse_args()
    variants = [v.split('=', 1)+[None] for v in a.variant]
    if a.block_variant:
        selected, = [v for v in variants if v[0] == a.block_variant]
        variants.append([selected[0]+'-block', selected[1], 'block'])
    assert a.rounds > 0 and len({v[0] for v in variants}) == len(variants)
    settings={v[0]:[] for v in variants}
    for setting in a.variant_env:
        label,sep,value=setting.partition(':')
        assert sep and label in settings,(label,setting)
        settings[label].append(value)
    if a.block_variant:settings[a.block_variant+'-block']=settings[a.block_variant]+settings[a.block_variant+'-block']
    a.root.mkdir(parents=True, exist_ok=False)
    tool = Path(__file__).with_name('compare_run.py')
    runs = []
    for round_no in range(a.rounds):
        order = variants[round_no % len(variants):]+variants[:round_no % len(variants)]
        for label, binary, exact_path in order:
            run_label = f'{label}-r{round_no+1}'
            with (a.root/(run_label+'.log')).open('w') as log:
                subprocess.run([sys.executable, str(tool), 'run', '--binary', binary,
                    '--prepared', str(a.prepared), '--root', str(a.root/'raw'),
                    '--label', run_label, '--image', a.image]+(['--exact-path', exact_path] if exact_path else [])+
                    [item for setting in settings[label] for item in ['--engine-env',setting]],
                    check=True, stdout=log, stderr=subprocess.STDOUT, timeout=900)
                result, = (a.root/'raw').glob(run_label+'-*/result.json')
                checked = a.root/(run_label+'.json')
                subprocess.run([sys.executable, str(tool), 'verify', '--result', str(result),
                    '--oracle', str(a.oracle), '--out', str(checked)], check=True, stdout=log, stderr=subprocess.STDOUT, timeout=300)
            r = json.loads(checked.read_text())
            assert r['restart_pass'] and all(g['passed'] for g in r['verification'].values()), checked
            runs.append({'variant': label, 'round': round_no+1, 'report': str(checked),'engine_env':settings[label]})
            print(json.dumps({'label': run_label, 'insert_wall_s': r['insert_wall_s'],
                              'query_wall_s': r['query_wall_s'], 'exact_mean_ms': r['verification']['exact']['mean_ms'],
                              'peak_bytes': r['resources']['after_shutdown']['memory_peak_bytes'], 'verified': True}), flush=True)
            (a.root/'progress.json').write_text(json.dumps(runs, indent=2)+'\n')
    summary = {}
    for label, _, _ in variants:
        group = [json.loads(Path(r['report']).read_text()) for r in runs if r['variant'] == label]
        summary[label] = {k: statistics.median(r[k] for r in group)
                          for k in ['insert_wall_s', 'insert_request_s', 'index_s', 'warmup_s', 'query_wall_s', 'checkpoint_s', 'final_file_bytes']}
        summary[label]['exact_mean_ms'] = statistics.median(r['verification']['exact']['mean_ms'] for r in group)
        summary[label]['filters_mean_ms'] = {k: statistics.median(r['verification']['exact']['filters'][k]['mean_ms'] for r in group)
                                             for k in group[0]['verification']['exact']['filters']}
        summary[label]['peak_bytes'] = statistics.median(r['resources']['after_shutdown']['memory_peak_bytes'] for r in group)
        summary[label]['ranges'] = {k: [min(r[k] for r in group), max(r[k] for r in group)] for k in ['insert_wall_s', 'query_wall_s', 'index_s']}
        summary[label]['filters'] = {name: {k: statistics.median(r['verification']['exact']['filters'][name][k] for r in group)
                                            for k in group[0]['verification']['exact']['filters'][name]}
                                    for name in group[0]['verification']['exact']['filters']}
    report = {'runs': runs, 'summary_medians': summary,
              'caveats': ['Local synthetic workload, not the official scorer.',
                          'Timing includes JSONL transport and response decoding; query_wall_s also includes driver SQL generation.',
                          'Memory includes the container and page cache; unavailable io.stat is not zero I/O.']}
    (a.root/'summary.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(summary), flush=True)


if __name__ == '__main__':
    main()
