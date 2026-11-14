#!/usr/bin/env python3
"""Prebuild JSONL requests; measure durable acknowledgements, CPU and file bytes.

Each run creates a new directory below --root; no existing data is overwritten.
Generation and checkpoint times are reported separately from INSERT requests.
Use --trace for a diagnostic run only: strace changes timing substantially.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import statistics
import subprocess
import tempfile
import time


def snapshot(pid):
    root = Path('/proc') / str(pid)
    parts = (root / 'stat').read_text().split(') ')[1].split()
    result = {'cpu_s': (int(parts[11]) + int(parts[12])) / os.sysconf('SC_CLK_TCK')}
    for line in (root / 'io').read_text().splitlines():
        key, val = line.split(':')
        result[key] = int(val)
    for line in (root / 'status').read_text().splitlines():
        if line.startswith('VmHWM:'):
            result['peak_rss_kib'] = int(line.split()[1])
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', required=True)
    ap.add_argument('--root', required=True)
    ap.add_argument('--rows', type=int, default=10000)
    ap.add_argument('--dim', type=int, default=128)
    ap.add_argument('--batch', type=int, default=100)
    ap.add_argument('--repeat', type=int, default=3)
    ap.add_argument('--index', choices=['none', 'before', 'after'], default='before')
    ap.add_argument('--trace', action='store_true')
    args = ap.parse_args()
    root = Path(args.root).resolve()
    if '/zzp/' not in str(root):
        raise ValueError('--root must be inside zzp')
    root.mkdir(parents=True, exist_ok=True)
    binary = Path(args.binary).resolve()
    gen_start = time.perf_counter()
    rng = random.Random(20261111)
    requests, samples = [], {}
    for base in range(0, args.rows, args.batch):
        values = []
        for i in range(base, min(args.rows, base + args.batch)):
            vec = '[' + ','.join('%.6g' % rng.gauss(0, .5) for _ in range(args.dim)) + ']'
            if i in (0, args.rows // 2, args.rows - 1):
                samples[i + 1] = vec
            values.append("(%d,'cat%d','doc%d','%s')" % (i + 1, i % 20, i, vec))
        sql = 'INSERT INTO bench VALUES ' + ','.join(values) + ';'
        requests.append(json.dumps({'id': len(requests) + 3, 'op': 'execute', 'sql': sql,
                                    'search_mode': 'exact'}, separators=(',', ':')).encode() + b'\n')
    generation_s = time.perf_counter() - gen_start
    request_sha256 = hashlib.sha256(b''.join(requests)).hexdigest()
    for repeat in range(args.repeat):
        run = Path(tempfile.mkdtemp(prefix='write-', dir=root))
        cmd = [str(binary), '--data-dir', str(run / 'data')]
        if args.trace:
            cmd = ['strace', '-c', '-w', '-f', '-o', str(run / 'strace.txt')] + cmd
        with (run / 'stderr.txt').open('wb') as stderr:
            proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr)
            assert json.loads(proc.stdout.readline()).get('type') == 'ready'

            def send(wire):
                proc.stdin.write(wire)
                proc.stdin.flush()
                line = proc.stdout.readline()
                if not line:
                    raise RuntimeError('engine ended; inspect ' + str(run / 'stderr.txt'))
                response = json.loads(line)
                if not response.get('ok'):
                    raise RuntimeError(response)
                return response

            def ask(sql):
                return send(json.dumps({'id': 1, 'op': 'execute', 'sql': sql,
                                        'search_mode': 'exact'}).encode() + b'\n')

            t = time.perf_counter()
            ask('CREATE TABLE bench (id BIGINT PRIMARY KEY, category VARCHAR(16), '
                'content VARCHAR(32), embedding VECTOR(%d));' % args.dim)
            if args.index == 'before':
                ask('CREATE VECTOR INDEX bench_vec ON bench(embedding);')
            setup_s = time.perf_counter() - t
            pid = proc.pid
            if args.trace:
                children = Path('/proc/%d/task/%d/children' % (pid, pid)).read_text().split()
                pid = int(children[0])
            before = snapshot(pid)
            elapsed = []
            t = time.perf_counter()
            for request in requests:
                qt = time.perf_counter()
                response = send(request)
                elapsed.append(time.perf_counter() - qt)
            insert_s = time.perf_counter() - t
            after = snapshot(pid)
            sizes = {p.name: p.stat().st_size for p in (run / 'data').iterdir() if p.is_file()}
            index_s = 0
            if args.index == 'after':
                t = time.perf_counter()
                ask('CREATE VECTOR INDEX bench_vec ON bench(embedding);')
                index_s = time.perf_counter() - t
            for pk, vec in samples.items():
                found = ask("SELECT id,L2_DISTANCE(embedding,'%s') AS d FROM bench WHERE id=%d ORDER BY d LIMIT 1;" % (vec, pk))
                assert found['rows'][0][0] == pk and abs(found['rows'][0][1]) < 1e-9, found
            t = time.perf_counter()
            send(b'{"id":999999,"op":"shutdown"}\n')
            proc.stdin.close()
            proc.wait(timeout=60)
            assert proc.returncode == 0
            checkpoint_s = time.perf_counter() - t
        result = {
            'binary': str(binary), 'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
            'root': str(run), 'rows': args.rows, 'dim': args.dim, 'batch': args.batch,
            'index': args.index, 'repeat': repeat, 'traced': args.trace,
            'generation_s': generation_s, 'setup_s': setup_s, 'insert_s': insert_s,
            'index_after_s': index_s, 'checkpoint_s': checkpoint_s, 'rows_s': args.rows / insert_s,
            'request_count': len(requests), 'request_sha256': request_sha256,
            'request_bytes': sum(map(len, requests)), 'vector_bytes': args.rows * args.dim * 4,
            'p50_request_ms': statistics.median(elapsed) * 1000,
            'p95_request_ms': sorted(elapsed)[min(len(elapsed) - 1, len(elapsed) * 95 // 100)] * 1000,
            'server_cpu_s': after['cpu_s'] - before['cpu_s'], 'peak_rss_kib': after['peak_rss_kib'],
            'io_delta': {key: after[key] - before[key] for key in before
                         if key not in ('cpu_s', 'peak_rss_kib')},
            'files_before_checkpoint': sizes, 'sample_exact_pass': True,
        }
        (run / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result), flush=True)


if __name__ == '__main__':
    main()
