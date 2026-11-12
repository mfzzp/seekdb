#!/usr/bin/env python3
"""Reproducible SQL benchmark; times have no pass/fail performance threshold.

Run the same file with --source pointing at each version. Every answer is
checked against a full-distance oracle outside the timed interval. RSS includes
the harness and its float32 fixtures; it is not database-only resident memory.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import struct
import subprocess
import sys
import tempfile
import time

for variable in ('OPENBLAS_NUM_THREADS', 'OMP_NUM_THREADS', 'MKL_NUM_THREADS'):
    os.environ.setdefault(variable, '1')


def digest(value):
    return hashlib.sha256(json.dumps(value, separators=(',', ':'), sort_keys=True).encode()).hexdigest()


def process_io():
    try:
        return {key: int(value) for key, value in
                (line.split(':') for line in Path('/proc/self/io').read_text().splitlines())}
    except OSError:
        return {}


def peak_rss_bytes():
    try:
        import resource
        value = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        return int(value if sys.platform == 'darwin' else value * 1024)
    except ImportError:
        return None


def cgroup_settings():
    """Record the kernel settings actually visible to the benchmark process."""
    try:
        relative = next(line.split('::', 1)[1] for line in
                        Path('/proc/self/cgroup').read_text().splitlines()
                        if line.startswith('0::'))
        roots = [Path('/sys/fs/cgroup') / relative.lstrip('/'), Path('/sys/fs/cgroup')]
        for root in roots:
            if (root / 'memory.max').is_file():
                return {name: (root / name).read_text().strip()
                        for name in ('memory.max', 'memory.swap.max', 'cpu.max', 'pids.max')
                        if (root / name).is_file()}
    except (OSError, StopIteration):
        pass
    return {}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--rows', type=int, default=16384)
    parser.add_argument('--dimensions', type=int, default=128)
    parser.add_argument('--data-root', type=Path, required=True)
    parser.add_argument('--repeats', type=int, default=12)
    parser.add_argument('--seed', type=int, default=582391)
    parser.add_argument('--distribution', choices=['all', 'uniform', 'clustered', 'correlated_filters', 'near_duplicates'], default='all')
    args = parser.parse_args()
    if min(args.rows, args.dimensions, args.repeats) < 1:
        parser.error('rows, dimensions and repeats must be positive')
    if args.output.exists():
        parser.error('--output must name a new file')
    source = args.source.resolve(strict=True)
    args.data_root.mkdir(parents=True, exist_ok=True)
    data_root = args.data_root.resolve(strict=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(source))
    import minivectordb as module
    import numpy as np
    distributions = ['uniform', 'clustered', 'correlated_filters', 'near_duplicates']
    if args.distribution != 'all':
        distributions = [args.distribution]
    try:
        revision = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
        dirty = bool(subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain'], text=True).strip())
    except (OSError, subprocess.CalledProcessError):
        revision, dirty = None, None
    report = {'source': str(source), 'revision': revision, 'dirty': dirty,
        'source_sha256': {name: hashlib.sha256((source / name).read_bytes()).hexdigest()
                         for name in ('minivectordb.py', 'minivector_store.py', 'minivector_sql.py')},
        'parameters': dict(rows=args.rows, dimensions=args.dimensions, seed=args.seed,
                           repeats=args.repeats, distributions=distributions, data_root=str(data_root)),
        'python': sys.version, 'numpy': np.__version__, 'database_numpy': module._numpy is not None,
        'cpu_affinity': sorted(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity') else None,
        'memory_limit_bytes': module.memory_limit_bytes(),
        'cgroup_v2': cgroup_settings(),
        'note': 'First query is process-cache cold, not OS-cache cold. Query timings exclude mutations, oracle checks and fixture generation. Phase I/O includes mutations. Peak RSS includes the harness and input vectors. No OS cache flush; read_bytes is actual storage I/O, rchar logical reads. metric_stats counts only the block-index attempt: a nonempty fallback_reason means the original vectorized path subsequently scanned candidates too.',
        'results': []}

    def save():
        report['peak_process_rss_bytes_including_harness'] = peak_rss_bytes()
        args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')

    for name in distributions:
        distribution_seed = args.seed + ['uniform', 'clustered', 'correlated_filters', 'near_duplicates'].index(name)
        rng = np.random.default_rng(distribution_seed)
        count, width = args.rows, args.dimensions
        groups = np.arange(count, dtype=np.int64) % 8
        if name == 'uniform':
            vectors = rng.uniform(-1.0, 1.0, (count, width)).astype(np.float32)
        elif name in ('clustered', 'correlated_filters'):
            vectors = (groups[:, None] * 16.0 + rng.normal(0.0, 0.06, (count, width))).astype(np.float32)
        else:
            prototypes = rng.normal(0.0, 1.0, (16, width)).astype(np.float32)
            vectors = prototypes[np.arange(count) % len(prototypes)].copy()
            # Half the rows are exact duplicates; half differ by a few float32 ULPs.
            vectors[1::2] += rng.normal(0.0, 1e-7, vectors[1::2].shape).astype(np.float32)
        categories = (groups if name == 'correlated_filters' else np.arange(count) % 4).copy()
        scores = (np.arange(count) % 100).astype(np.int64)
        row = {'distribution': name, 'fixture_sha256': hashlib.sha256(vectors.tobytes() + categories.tobytes() + scores.tobytes()).hexdigest(), 'phases': []}
        report['results'].append(row)
        with tempfile.TemporaryDirectory(prefix='metric-' + name + '-', dir=data_root) as directory:
            Path(directory).resolve().relative_to(data_root)
            db = module.Database(directory)
            try:
                before = process_io()
                start = time.perf_counter()
                db.execute(f'CREATE TABLE t (id BIGINT PRIMARY KEY,category VARCHAR(32),score BIGINT,v VECTOR({width}))', 'exact')
                for base in range(0, count, 256):
                    values = [f"({key},'c{int(categories[key])}',{int(scores[key])},'" + json.dumps(vectors[key].tolist(), separators=(',', ':')) + "')"
                              for key in range(base, min(base + 256, count))]
                    db.execute('INSERT INTO t VALUES ' + ','.join(values), 'exact')
                db.execute('CREATE VECTOR INDEX vi ON t(v)', 'exact')
                row['load_ms'] = (time.perf_counter() - start) * 1000
                after = process_io()
                row['load_io_delta'] = {key: after[key] - before[key] for key in after}

                def query_vector(index):
                    if name in ('clustered', 'correlated_filters'):
                        return np.full(width, 16.0 * (index % 8) + 0.01, dtype=np.float32)
                    if name == 'near_duplicates':
                        return prototypes[index % len(prototypes)].copy()
                    # Different deterministic inputs prevent result caching from
                    # masquerading as an index improvement.
                    return np.full(width, 0.1 + index * 0.007, dtype=np.float32)

                def run_phase(phase, repeats, filtered=False, mutate=None):
                    before = process_io()
                    timings, identifiers, canonical, index_stats = [], [], [], []
                    largest_distance_error = 0.0
                    for iteration in range(repeats):
                        query = query_vector(iteration)
                        if mutate == 'scalar':
                            key = iteration % count
                            categories[key] = (int(categories[key]) + 1) % 8
                            db.execute(f"UPDATE t SET category='c{int(categories[key])}' WHERE id={key}", 'exact')
                        elif mutate == 'vector':
                            key = iteration % count
                            vectors[key] = query
                            db.execute("UPDATE t SET v='" + json.dumps(query.tolist()) + f"' WHERE id={key}", 'exact')
                        where = "WHERE category='c3' AND score>=20" if filtered else ''
                        sql = "SELECT id,L2_DISTANCE(v,'" + json.dumps(query.tolist()) + f"') AS d FROM t {where} ORDER BY d LIMIT 10"
                        start = time.perf_counter()
                        answer = db.execute(sql, 'exact')['rows']
                        timings.append((time.perf_counter() - start) * 1000)
                        # Independent exhaustive FP64 differences; never uses the
                        # candidate's pruning, candidate ordering or index cache.
                        distances = np.empty(count, dtype=np.float64)
                        for begin in range(0, count, 4096):
                            delta = vectors[begin:begin + 4096].astype(np.float64) - query.astype(np.float64)
                            distances[begin:begin + delta.shape[0]] = np.sqrt(np.einsum('ij,ij->i', delta, delta))
                        keys = np.flatnonzero((categories == 3) & (scores >= 20)) if filtered else np.arange(count)
                        expected = keys[np.lexsort((keys, distances[keys]))[:10]].tolist()
                        actual = [item[0] for item in answer]
                        if actual != expected:
                            raise AssertionError((name, phase, iteration, actual, expected))
                        for key, distance in answer:
                            wanted = float(distances[key])
                            largest_distance_error = max(largest_distance_error, abs(distance - wanted))
                            if not math.isclose(distance, wanted, rel_tol=2e-12, abs_tol=1e-300):
                                raise AssertionError(('distance', name, phase, key, distance, wanted))
                        identifiers.append(actual)
                        canonical.append([[key, format(distance, '.9g')] for key, distance in answer])
                        cache = getattr(db.table('t'), 'metric_blocks', None)
                        if isinstance(cache, dict):
                            stats = {key: int(cache[key]) for key in ('last_scanned_rows', 'seed_scanned_rows', 'last_pruned_blocks')
                                     if cache.get(key) is not None}
                            stats['fallback_reason'] = cache.get('fallback_reason')
                        else:
                            stats = {'cache': False if cache is False else None}
                        index_stats.append(stats)
                    after = process_io()
                    cached_matrix = getattr(db.table('t'), 'matrix', None)
                    item = {'phase': phase, 'queries': repeats, 'median_ms': statistics.median(timings),
                            'matrix_dtype': str(cached_matrix[2].dtype) if cached_matrix is not None else None,
                            'min_ms': min(timings), 'max_ms': max(timings), 'timings_ms': timings,
                            'ranking_sha256': digest(identifiers), 'result_9sig_sha256': digest(canonical),
                            'max_abs_distance_error': largest_distance_error, 'oracle_passed': True,
                            'io_delta': {key: after[key] - before[key] for key in after},
                            'metric_stats': index_stats}
                    row['phases'].append(item)
                    save()
                    print(json.dumps({'distribution': name, 'phase': phase, 'median_ms': item['median_ms'], 'oracle_passed': True}), flush=True)

                run_phase('first_query', 1)
                run_phase('warmup_and_index_build', 3)
                run_phase('warm_query', args.repeats)
                run_phase('first_filtered', 1, filtered=True)
                run_phase('warm_filtered', args.repeats, filtered=True)
                run_phase('after_scalar_write', 4, mutate='scalar')
                run_phase('after_vector_write', 4, mutate='vector')
                row['database_accounted_cache_bytes'] = db._memory_used()
            finally:
                db.close()
        save()
    report['passed'] = True
    save()
    print('Report: ' + str(args.output), flush=True)


if __name__ == '__main__':
    main()
