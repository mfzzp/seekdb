#!/usr/bin/env python3
"""Paired durable writes over byte-identical prebuilt JSONL; requires numpy.

Fresh DB per run, interleaved binary order; generation, writes, checkpoint and
restart verification are timed separately. Never changes commit batch size.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import time
import numpy as np
from exact_paths_bench import literal,peak_bytes


def cpu_seconds(pid):
    if os.name=='nt':
        from ctypes import wintypes
        kernel=ctypes.WinDLL('kernel32',use_last_error=True)
        kernel.OpenProcess.restype=wintypes.HANDLE
        kernel.CloseHandle.argtypes=[wintypes.HANDLE]
        kernel.GetProcessTimes.argtypes=[wintypes.HANDLE]+[ctypes.POINTER(wintypes.FILETIME)]*4
        h=kernel.OpenProcess(0x410,False,pid);times=[wintypes.FILETIME() for _ in range(4)]
        try:
            if kernel.GetProcessTimes(h,*[ctypes.byref(t) for t in times]):
                return sum((t.dwHighDateTime<<32)+t.dwLowDateTime for t in times[2:])/1e7
        finally:kernel.CloseHandle(h)
        return None
    parts=Path(f'/proc/{pid}/stat').read_text().split(') ')[1].split()
    return (int(parts[11])+int(parts[12]))/os.sysconf('SC_CLK_TCK')


def wire(sql,ident=1,op='execute'):
    return (json.dumps({'id':ident,'op':op,'sql':sql,'search_mode':'exact'},separators=(',',':'))+'\n').encode()


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',action='append',required=True,help='label=path; repeat for paired binaries')
    ap.add_argument('--root',type=Path,required=True)
    ap.add_argument('--rows',type=int,default=10000);ap.add_argument('--dim',type=int,default=128)
    ap.add_argument('--batch',type=int,default=100);ap.add_argument('--repeat',type=int,default=3)
    ap.add_argument('--transaction',choices=['auto','explicit'],default='auto')
    ap.add_argument('--verify',choices=['all','sample'],default='all')
    a=ap.parse_args();a.root.mkdir(parents=True,exist_ok=False)
    binaries={k:str(Path(v).resolve()) for k,v in (b.split('=',1) for b in a.binary)}
    start=time.perf_counter();rng=np.random.default_rng(20261114)
    vectors=rng.normal(0,.5,(a.rows,a.dim)).astype('float32');requests=[];ident=10
    for base in range(0,a.rows,a.batch):
        if a.transaction=='explicit':requests.append(wire('BEGIN;',ident));ident+=1
        values=[f"({i+1},'cat{i%20}','doc{i}','{literal(vectors[i])}')" for i in range(base,min(base+a.batch,a.rows))]
        requests.append(wire('INSERT INTO bench VALUES '+','.join(values)+';',ident));ident+=1
        if a.transaction=='explicit':requests.append(wire('COMMIT;',ident));ident+=1
    report={'platform':platform.platform(),'rows':a.rows,'dim':a.dim,'batch':a.batch,'transaction':a.transaction,
            'generation_s':time.perf_counter()-start,'seed':20261114,'request_count':len(requests),
            'commit_count':(a.rows+a.batch-1)//a.batch,'request_bytes':sum(map(len,requests)),
            'request_sha256':hashlib.sha256(b''.join(requests)).hexdigest(),
            'binaries':{k:hashlib.sha256(Path(v).read_bytes()).hexdigest() for k,v in binaries.items()},'runs':[]}
    def launch(binary,data,log):
        p=subprocess.Popen([binary,'--data-dir',data.as_posix()],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=log)
        assert json.loads(p.stdout.readline())=={'type':'ready'}
        return p
    def send(p,request):
        p.stdin.write(request);p.stdin.flush();line=p.stdout.readline()
        assert line,'engine exited'
        r=json.loads(line);assert r.get('ok'),r
        return r
    def close(p):
        send(p,wire('',op='shutdown'));p.stdin.close();assert p.wait(timeout=120)==0;p.stdout.close()
    for repeat in range(a.repeat):
        labels=list(binaries);labels=labels[repeat%len(labels):]+labels[:repeat%len(labels)]
        for label in labels:
            folder=(a.root/f'{repeat}-{label}').resolve();folder.mkdir()
            data=folder/'db'
            with (folder/'stderr.log').open('wb') as log:
                p=launch(binaries[label],data,log)
                send(p,wire(f'CREATE TABLE bench (id BIGINT PRIMARY KEY,category VARCHAR(16),content VARCHAR(32),embedding VECTOR({a.dim}));'))
                send(p,wire('CREATE VECTOR INDEX bench_vec ON bench(embedding);'))
                before=cpu_seconds(p.pid);elapsed=[];start=time.perf_counter()
                for req in requests:
                    t=time.perf_counter();send(p,req);elapsed.append(time.perf_counter()-t)
                seconds=time.perf_counter()-start;cpu=cpu_seconds(p.pid)-before
                peak=peak_bytes(p.pid)
                sizes={f.name:f.stat().st_size for f in data.iterdir() if f.is_file()}
                hashes={name:hashlib.sha256((data/name).read_bytes()).hexdigest() for name in ['wal.log','vectors.f32']}
                if report['runs']:
                    assert hashes==report['runs'][0]['hashes_before_checkpoint'],'WAL/vector bytes changed between paired runs'
                start=time.perf_counter();close(p);checkpoint=time.perf_counter()-start
            start=time.perf_counter()
            with (folder/'restart.stderr.log').open('wb') as log:
                p=launch(binaries[label],data,log)
                positions=list(range(a.rows)) if a.verify=='all' else [0,a.rows//2,a.rows-1]
                if a.verify=='all':
                    for base in range(0,a.rows,256):
                        rows=send(p,wire(f'SELECT id,category,content,embedding FROM bench WHERE id>{base} AND id<={min(base+256,a.rows)} ORDER BY id;'))['rows']
                        assert len(rows)==min(256,a.rows-base)
                        for i,row in enumerate(rows,base):
                            assert row[:3]==[i+1,f'cat{i%20}',f'doc{i}'],row[:3]
                            assert np.array_equal(np.asarray(row[3],dtype='float32'),vectors[i]),i
                else:
                    for i in positions:
                        row=send(p,wire(f'SELECT id,category,content,embedding FROM bench WHERE id={i+1};'))['rows'][0]
                        assert row[:3]==[i+1,f'cat{i%20}',f'doc{i}']
                        assert np.array_equal(np.asarray(row[3],dtype='float32'),vectors[i])
                close(p)
            run={'round':repeat,'binary':label,'write_s':seconds,'cpu_s':cpu,'checkpoint_s':checkpoint,
                 'write_plus_checkpoint_s':seconds+checkpoint,'peak_bytes':peak,'restart_verify_s':time.perf_counter()-start,
                 'verified_rows':len(positions),'p50_request_ms':statistics.median(elapsed)*1000,
                 'p95_request_ms':sorted(elapsed)[int(.95*(len(elapsed)-1))]*1000,'files_before_checkpoint':sizes,'hashes_before_checkpoint':hashes}
            report['runs'].append(run);print(json.dumps(run),flush=True)
            (a.root/'result.json').write_text(json.dumps(report,indent=2)+'\n')


if __name__=='__main__':main()
