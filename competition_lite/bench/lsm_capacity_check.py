#!/usr/bin/env python3
"""Host-driven capacity test; the database alone is inside the memory limit."""
import argparse
import json
import math
from pathlib import Path
import subprocess
import time
import os

ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--image',required=True);ap.add_argument('--root',type=Path,required=True)
ap.add_argument('--rows',type=int,default=150000);ap.add_argument('--memory-mb',type=int,default=128)
a=ap.parse_args();a.root.mkdir(parents=True,exist_ok=False);data=a.root/'data';data.mkdir()
name='lite-lsm-capacity-'+str(os.getpid());n=0;report={'rows':a.rows,'memory_limit_bytes':a.memory_mb<<20,'passed':False}
def ask(sql):
    global n
    n+=1;p.stdin.write(json.dumps({'id':n,'sql':sql})+'\n');p.stdin.flush()
    line=p.stdout.readline();assert line,('database exited',p.poll())
    r=json.loads(line);assert r.get('id')==n and r.get('ok'),r;return r
def resources():
    r={}
    for key,path in [('memory_peak_bytes','/sys/fs/cgroup/memory.peak'),('memory_current_bytes','/sys/fs/cgroup/memory.current'),('process_io','/proc/1/io'),('process_status','/proc/1/status')]:
        s=subprocess.check_output(['docker','exec',name,'cat',path],text=True)
        r[key]=int(s) if key.endswith('_bytes') else s
    return r
def start():
    global p
    args=['docker','run','--name',name,'-i','--cpus=2',f'--memory={a.memory_mb}m',f'--memory-swap={a.memory_mb}m','--pids-limit=64','--network=none','--read-only','--cap-drop=ALL','--security-opt=no-new-privileges',
          '-e','LITE_ROW_CACHE_MB=0','-e','LITE_MEMTABLE_BYTES=1048576','-e','LITE_SST_BYTES=2097152','-e','LITE_MMAP_MB=8','--mount',f'type=bind,src={data.resolve()},dst=/data',a.image]
    p=subprocess.Popen(args,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=log,text=True,bufsize=1)
    assert json.loads(p.stdout.readline())=={'type':'ready'}
def stop():
    global n
    n+=1;p.stdin.write(json.dumps({'id':n,'op':'shutdown'})+'\n');p.stdin.flush();r=json.loads(p.stdout.readline());assert r['ok'] and r['id']==n,r
    assert p.wait(timeout=120)==0
    state=json.loads(subprocess.check_output(['docker','inspect',name],text=True))[0]['State'];assert not state['OOMKilled'],state
    subprocess.run(['docker','rm',name],check=True,stdout=subprocess.DEVNULL)
def verify():
    q='['+','.join(['0']*128)+']'
    want=sorted((128*(i%997)**2,i) for i in range(a.rows))[:10]
    for where,pred in [('',lambda i:True),(' WHERE id>=1000 AND id<4000',lambda i:1000<=i<4000)]:
        truth=sorted((128*(i%997)**2,i) for i in range(a.rows) if pred(i))[:10]
        r=ask(f"SELECT id,L2_DISTANCE(v,'{q}') AS d FROM t{where} ORDER BY d LIMIT 10;")
        assert r['rows']==[[i,math.sqrt(d)] for d,i in truth],(r,truth)
        assert r['metrics']['storage_path']=='lsm',r
    for i in [0,1,996,997,a.rows//2,a.rows-1]:
        r=ask(f'SELECT id,payload,v,tail FROM t WHERE id={i};')['rows']
        assert r==[[i,'x'*1024,[float(i%997)]*128,'last-'+str(i)]],(i,r)

with (a.root/'stderr.log').open('w') as log:
    try:
        start();ask('CREATE TABLE t (id BIGINT PRIMARY KEY,payload VARCHAR(1024),v VECTOR(128),tail VARCHAR(64));')
        begin=time.perf_counter();ask('BEGIN;')
        for start_row in range(0,a.rows,200):
            rows=[]
            for i in range(start_row,min(a.rows,start_row+200)):
                vec='['+','.join([str(i%997)]*128)+']'
                rows.append(f"({i},'{'x'*1024}','{vec}','last-{i}')")
            ask('INSERT INTO t VALUES '+','.join(rows)+';')
        report['before_commit']=resources();ask('COMMIT;');report['write_commit_s']=time.perf_counter()-begin
        report['after_commit']=resources();begin=time.perf_counter();verify();report['verify_query_s']=time.perf_counter()-begin;report['after_queries']=resources();stop()
        report['file_bytes']=sum(p.stat().st_size for p in data.iterdir() if p.is_file())
        report['primary_payload_lower_bound_bytes']=a.rows*(1024+16+8+12)
        assert report['primary_payload_lower_bound_bytes']>report['memory_limit_bytes'],'workload must exceed the memory limit even without vectors or secondary keys'
        begin=time.perf_counter();start();report['restart_s']=time.perf_counter()-begin;verify();report['after_restart_queries']=resources();stop()
        report['passed']=True
    finally:
        subprocess.run(['docker','rm','-f',name],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        (a.root/'result.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report),flush=True)
