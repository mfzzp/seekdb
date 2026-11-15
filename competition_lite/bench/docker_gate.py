#!/usr/bin/env python3
"""Build and check the actual Docker entrypoint; public checks are not the hidden gate."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, required=True)
    ap.add_argument('--root', type=Path, required=True)
    ap.add_argument('--image', required=True)
    a = ap.parse_args()
    src, out, tag = a.source.resolve(), a.root.resolve(), a.image
    out.mkdir(parents=True, exist_ok=False)
    report = {'source_sha256': hashlib.sha256((src/'competition_lite/src/minivectordb_lite.cpp').read_bytes()).hexdigest(),
              'runtime_sources': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((src/'competition_lite/src').glob('*')) if p.suffix in {'.cpp','.hpp','.h'}},
              'image_tag': tag, 'passed': False, 'checks': []}

    def run(name, args, timeout=600):
        start = time.monotonic()
        with (out/(name+'.log')).open('w') as log:
            p = subprocess.run(args, cwd=src, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
        item = {'name': name, 'exit_code': p.returncode, 'seconds': time.monotonic()-start}
        report['checks'].append(item)
        (out/'gate.json').write_text(json.dumps(report, indent=2)+'\n')
        print(json.dumps(item), flush=True)
        if p.returncode:
            raise RuntimeError(f'{name} failed; see {out/(name+".log")}')

    docker = ['docker', 'run', '--rm', '--cpus=2', '--memory=2g', '--memory-swap=2g',
              '--pids-limit=128', '--network=none', '--read-only', '--cap-drop=ALL',
              '--security-opt=no-new-privileges', '--mount', f'type=bind,src={src},dst=/checks,readonly',
              '--mount', f'type=bind,src={out},dst=/data']
    try:
        run('line_budget', ['python3', 'competition_lite/tools/check_lines.py'])
        run('build', ['docker', 'build', '--pull=false', '-t', tag, '.'])
        run('image', ['docker', 'image', 'inspect', tag], 30)
        run('limits', docker+['--entrypoint', 'sh', tag, '-c',
            'cat /sys/fs/cgroup/cpu.max /sys/fs/cgroup/memory.max /sys/fs/cgroup/memory.swap.max; '
            'cat /sys/fs/cgroup/cgroup.controllers; g++ --version | head -1; '
            'sha256sum /app/minivectordb /app/minivectordb_lite.cpp /app/lsm_store.hpp'], 30)
        run('public', ['python3', 'protocol_check/run.py', '--image', tag, '--output', str(out/'public')])
        run('extra', ['python3', 'protocol_check/run.py', '--image', tag, '--package', 'competition_lite/tests/extra_cases', '--output', str(out/'extra')])
        for name, tool, root in [('edges', 'admission_edges_check.py', 'edges'),
                                 ('exact', 'exact_paths_check.py', 'exact'),
                                 ('bulk', 'bulk_recovery_check.py', 'zzp/bulk'),
                                 ('quotes', 'sql_literal_check.py', 'zzp/quotes'),
                                 ('lsm_model', 'lsm_model_check.py', 'lsm_model'),
                                 ('lsm_corruption', 'lsm_corruption_check.py', 'lsm_corruption')]:
            run(name, docker+['--entrypoint', 'python3', tag, '-B', '/checks/competition_lite/bench/'+tool,
                              '--binary', '/app/minivectordb', '--root', '/data/'+root])
        run('commit_faults', docker+['--tmpfs', '/tmp', '--entrypoint', 'sh', tag, '-c',
            'g++ -O1 -std=c++17 /checks/competition_lite/bench/write_commit_check.cpp '
            '-Wl,--wrap=write -Wl,--wrap=fdatasync -o /data/commit-check && '
            'python3 -B /checks/competition_lite/bench/write_commit_check.py '
            '--binary /data/commit-check --root /data/commit_faults'])
        run('lsm_faults', docker+['--tmpfs', '/tmp', '--entrypoint', 'sh', tag, '-c',
            'g++ -O1 -std=c++17 /checks/competition_lite/bench/lsm_storage_check.cpp '
            '-Wl,--wrap=write -Wl,--wrap=fsync -Wl,--wrap=rename -o /data/storage-check && '
            'python3 -B /checks/competition_lite/bench/lsm_storage_check.py '
            '--binary /data/storage-check --root /data/lsm_faults'])
        report['passed'] = True
    except BaseException as e:
        report['error'] = repr(e)
        raise
    finally:
        (out/'gate.json').write_text(json.dumps(report, indent=2)+'\n')


if __name__ == '__main__':
    main()
