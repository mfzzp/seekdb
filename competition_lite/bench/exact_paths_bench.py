#!/usr/bin/env python3
"""Serial, paired JSONL exact benchmark; clocks off for timings, on in a separate pass.

Uses local synthetic float32 data, not the official generator. Requires numpy.
All modes reuse one checkpoint and byte-identical queries; no server is needed.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import time
import numpy as np
from protocol_adapter import Session


def peak_bytes(pid):
    if os.name == 'nt':
        from ctypes import wintypes
        class Counters(ctypes.Structure):
            _fields_ = [('cb', wintypes.DWORD), ('faults', wintypes.DWORD)] + [
                (name, ctypes.c_size_t) for name in ['peak', 'working', 'pqpeak', 'pq', 'nppeak', 'np', 'page', 'pagepeak']]
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        get = ctypes.WinDLL('psapi').GetProcessMemoryInfo
        get.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
        h = kernel.OpenProcess(0x410, False, pid)
        c = Counters(); c.cb = ctypes.sizeof(c)
        try:
            return int(c.peak) if get(h, ctypes.byref(c), c.cb) else None
        finally:
            kernel.CloseHandle(h)
    try:
        for line in Path(f'/proc/{pid}/status').read_text().splitlines():
            if line.startswith('VmHWM:'): return int(line.split()[1])*1024
    except OSError:
        pass
    return None


def literal(v):
    return '['+','.join(format(float(x), '.9g') for x in v)+']'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline', required=True)
    ap.add_argument('--candidate', required=True)
    ap.add_argument('--root', type=Path, required=True)
    ap.add_argument('--rows', type=int, default=10000)
    ap.add_argument('--dim', type=int, default=128)
    ap.add_argument('--queries', type=int, default=1000)
    ap.add_argument('--rounds', type=int, default=3)
    ap.add_argument('--checkpoint', type=Path, help='Reuse a checkpoint made by this script with identical rows/dim/seed')
    a = ap.parse_args(); a.root.mkdir(parents=True, exist_ok=False)
    data = (a.checkpoint or a.root/'db').resolve().as_posix()
    rng = np.random.default_rng(20261114)
    vectors = rng.normal(0,.5,(a.rows,a.dim)).astype('float32')
    ids = np.arange(1,a.rows+1)
    kinds = [('none', '', np.ones(a.rows, dtype=bool)),
             ('category', " WHERE category='c3'", ids%20==3)]
    if a.dim != 128:
        kinds += [('tenant', ' WHERE tenant=2', ids%7==2),
                  ('joint', " WHERE category='c3' AND tenant=2", (ids%20==3)&(ids%7==2)),
                  ('range', f' WHERE id>={a.rows//4} AND id<{a.rows*3//4}', (ids>=a.rows//4)&(ids<a.rows*3//4)),
                  ('empty', " WHERE category='absent'", np.zeros(a.rows, dtype=bool))]
    queries = []
    for i in range(a.queries):
        kind, where, mask = kinds[i%len(kinds)]
        v = rng.normal(0,.5,a.dim).astype('float32')
        sql = f"SELECT id,L2_DISTANCE(embedding,'{literal(v)}') AS distance FROM docs{where} ORDER BY distance LIMIT 10;"
        truth = None
        if i < 3*len(kinds):
            distances = np.sum((vectors[mask].astype('float64')-v.astype('float64'))**2,axis=1)
            order = np.lexsort((ids[mask], distances))[:10]
            truth = [[int(ids[mask][j]), float(np.sqrt(distances[j]))] for j in order]
        queries.append((kind,sql,truth))
    for name in ['LITE_PROFILE','LITE_BLOCK_EXACT','LITE_BLOCK_ORDER','LITE_EXACT_PATH','LITE_EARLY']:
        os.environ.pop(name,None)
    if not a.checkpoint:
        s = Session(str(Path(a.baseline).resolve()),data)
        s.ask(f'CREATE TABLE docs (id BIGINT PRIMARY KEY,category VARCHAR(32),tenant BIGINT,embedding VECTOR({a.dim}));')
        for start in range(0,a.rows,100):
            values = [f"({i+1},'c{(i+1)%20}',{(i+1)%7},'{literal(vectors[i])}')" for i in range(start,min(a.rows,start+100))]
            s.ask('INSERT INTO docs VALUES '+','.join(values)+';')
        s.close()
    binaries = {'baseline':str(Path(a.baseline).resolve()), 'scan':str(Path(a.candidate).resolve()), 'block':str(Path(a.candidate).resolve())}
    report = {'platform':platform.platform(), 'rows':a.rows,'dimension':a.dim,'queries':a.queries,'seed':20261114,
              'sha256':{k:hashlib.sha256(Path(v).read_bytes()).hexdigest() for k,v in binaries.items()},
              'vector_sha256':hashlib.sha256(vectors.tobytes()).hexdigest(),
              'query_sha256':hashlib.sha256('\n'.join(q[1] for q in queries).encode()).hexdigest(),
              'memory_scope':'query process peak RSS/working set, excludes loader and is not cgroup peak', 'runs':[]}
    reference = []
    for round_id in range(a.rounds):
        order = ['baseline','scan','block']
        order = order[round_id%3:]+order[:round_id%3]
        for mode in order:
            os.environ['LITE_EXACT_PATH'] = 'auto' if mode=='baseline' else mode
            s = Session(binaries[mode],data)
            first_start=time.perf_counter(); first=s.ask(queries[0][1]); first_ms=(time.perf_counter()-first_start)*1000
            for _,sql,_ in queries[:len(kinds)*2]: s.ask(sql)
            by_kind = {k[0]:[] for k in kinds}; metrics = {k[0]:[] for k in kinds}
            for i,(kind,sql,truth) in enumerate(queries):
                start=time.perf_counter(); result=s.ask(sql); ms=(time.perf_counter()-start)*1000
                rows=result['rows']
                if round_id==0 and mode=='baseline': reference.append(rows)
                else: assert rows==reference[i], (mode,i,rows,reference[i])
                if truth is not None:
                    assert [r[0] for r in rows]==[r[0] for r in truth], (mode,i,rows,truth)
                    assert all(abs(x[1]-y[1])<=2e-6*max(1,y[1]) for x,y in zip(rows,truth))
                by_kind[kind].append(ms); metrics[kind].append(result['metrics'])
            run={'round':round_id,'mode':mode,'first_query_ms':first_ms,'peak_bytes':peak_bytes(s.p.pid),
                 'groups':{k:{'mean_ms':statistics.mean(v),'median_ms':statistics.median(v),'p95_ms':sorted(v)[int(.95*(len(v)-1))],
                              'mean_evaluations':statistics.mean(m['exact_evaluations'] for m in metrics[k]),
                              'mean_blocks_pruned':statistics.mean(m.get('blocks_pruned',0) for m in metrics[k])} for k,v in by_kind.items()}}
            s.close();report['runs'].append(run)
            (a.root/'result.json').write_text(json.dumps(report,indent=2))
            print(json.dumps(run),flush=True)
    report['profiles']={}
    os.environ['LITE_PROFILE']='1'
    for mode in ['scan','block']:
        os.environ['LITE_EXACT_PATH']=mode
        s=Session(binaries[mode],data)
        cold=s.ask(queries[0][1])['metrics']
        for _,sql,_ in queries[:len(kinds)*2]: s.ask(sql)
        warm=[{'kind':kind,**s.ask(sql)['metrics']} for kind,sql,_ in queries[:len(kinds)*3]]
        report['profiles'][mode]={'cold':cold,'warm':warm}
        s.close()
    report['correctness']='all path results identical; first 3 queries per filter checked with independent float64 oracle'
    (a.root/'result.json').write_text(json.dumps(report,indent=2)+'\n')


if __name__=='__main__': main()
