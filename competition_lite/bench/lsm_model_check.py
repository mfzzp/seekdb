#!/usr/bin/env python3
"""Independent row/vector oracle through spills, compaction, rollback and GC."""
import argparse
import copy
import json
import math
import os
from pathlib import Path
import random
import subprocess
from protocol_adapter import Session

ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--binary',required=True);ap.add_argument('--root',type=Path,required=True)
ap.add_argument('--legacy',help='previous production binary, for STATE/WAL migration')
a=ap.parse_args();a.root.mkdir(parents=True,exist_ok=False)
os.environ.update(LITE_ROW_CACHE_MB='0',LITE_MEMTABLE_BYTES='16384',LITE_SST_BYTES='32768',LITE_MMAP_MB='4')
rng=random.Random(20261115);checks=0
def sqlstr(s):return "'"+s.replace('\\','\\\\').replace("'","''")+"'"
def values(i,r):return f"({i},{r[0]},{sqlstr(r[1])},'{r[2]}','{r[3]}',{sqlstr(r[4])})"
def kill(s):s.p.kill();s.p.wait(timeout=30)
def verify(s,model):
    global checks
    expected=[[i]+r for i,r in sorted(model.items())]
    assert s.ask('SELECT id,n,tag,v,w,tail FROM t ORDER BY id;')['rows']==expected
    assert s.ask('SELECT id,n,tag,v,w,tail FROM t ORDER BY n DESC,id LIMIT 5;')['rows']==sorted(expected,key=lambda r:(-r[1],r[0]))[:5]
    for where,pred in [('',lambda i,r:True),(" WHERE tag='odd'",lambda i,r:r[1]=='odd'),(' WHERE n>=-3 AND n<5',lambda i,r:-3<=r[0]<5),(' WHERE id>=-30 AND id<50',lambda i,r:-30<=i<50)]:
        for c,idx,q in [('v',2,[1.0,-2.0,0.5,3.0]),('w',3,[0.0,1.0])]:
            want=sorted((math.fsum((x-y)**2 for x,y in zip(r[idx],q)),i) for i,r in model.items() if pred(i,r))[:10]
            result=s.ask(f"SELECT id,L2_DISTANCE({c},'{q}') AS d FROM t{where} ORDER BY d LIMIT 10;")
            assert result['rows']==[[i,math.sqrt(d)] for d,i in want],(where,c,result,want)
            assert result['metrics']['storage_path']=='lsm'
            checks+=1
    checks+=2

data=str((a.root/'model').resolve());s=Session(a.binary,data)
s.ask('CREATE TABLE t (id BIGINT PRIMARY KEY,n BIGINT,tag VARCHAR(64),v VECTOR(4),w VECTOR(2),tail VARCHAR(100));')
model={}
for start in range(0,512,32):
    rows=[]
    for j in range(start,start+32):
        i=(j//2+1)*(-1 if j%2 else 1)
        r=[j%17-8,'odd' if j%2 else 'even',[float(j%13),-2.0,float(j%9)/4,3.0],[float(j%7),1.0],"last\x1f\x00字段 ' \\"+str(j)]
        model[i]=r;rows.append(values(i,r))
    s.ask('INSERT INTO t VALUES '+','.join(rows)+';')
verify(s,model)
for epoch in range(4):
    before=copy.deepcopy(model);s.ask('BEGIN;')
    for j in range(12):
        i=rng.choice(sorted(model));r=model[i]
        if j%3==0:
            s.ask(f'DELETE FROM t WHERE id={i};')
            r=[9,'odd',[7.0,8.0,9.0,10.0],[3.0,4.0],'reinsert-last']
            s.ask('INSERT INTO t VALUES '+values(i,r)+';');model[i]=r
        else:
            s.ask(f"UPDATE t SET n={j-5},v='[4,3,2,1]',tail='updated-last' WHERE id={i};")
            r[0]=j-5;r[2]=[4.0,3.0,2.0,1.0];r[4]='updated-last'
    verify(s,model)
    if epoch%2:s.ask('COMMIT;')
    else:s.ask('ROLLBACK;');model=before
    verify(s,model)
    if epoch==1:kill(s)
    else:s.close()
    s=Session(a.binary,data);verify(s,model)
# A rejected statement must preserve previously buffered vectors in this tx.
s.ask('BEGIN;');extra=[1,'odd',[101.0,102.0,103.0,104.0],[105.0,106.0],'before-error']
s.ask('INSERT INTO t VALUES '+values(9999,extra)+';')
try:s.ask("INSERT INTO t VALUES (9998,0,'odd','[0,1,2,3]','[4,5]','valid'),(9997,0,'odd','[1]','[1,2]','invalid');")
except RuntimeError:pass
else:raise AssertionError('bad vector accepted')
s.ask('COMMIT;');model[9999]=extra;verify(s,model);s.close()
for maintenance in ['flush','compact','gc']:
    subprocess.run([a.binary,'--data-dir',data,'--maintenance',maintenance],check=True)
    s=Session(a.binary,data);verify(s,model);s.close()
assert not (Path(data)/'STATE').exists()
assert len(list(Path(data).glob('lsm-*.wal')))==1
assert (Path(data)/'LSM-MANIFEST').is_file()
# WAL-tail rollback across a hard kill, including spilled private transaction files.
s=Session(a.binary,data);s.ask('BEGIN;');s.ask("UPDATE t SET v='[9,9,9,9]',tail='uncommitted';");kill(s)
s=Session(a.binary,data);verify(s,model);s.close()
assert not list(Path(data).glob('*.txn'))

migrations=[]
if a.legacy:
    for style in ['state','wal','state-wal']:
        path=str((a.root/('legacy-'+style)).resolve());s=Session(a.legacy,path)
        s.ask('CREATE TABLE t (id BIGINT PRIMARY KEY,n BIGINT,tag VARCHAR(64),v VECTOR(4),w VECTOR(2),tail VARCHAR(100));')
        m={i:[i%3,'odd',[float(i),2.0,3.0,4.0],[5.0,float(i)],'legacy-last'] for i in range(1,65)}
        s.ask('INSERT INTO t VALUES '+','.join(values(i,r) for i,r in m.items())+';')
        s.ask('CREATE VECTOR INDEX vx ON t(v);')
        if style=='state':s.close()
        elif style=='wal':kill(s)
        else:
            s.close();s=Session(a.legacy,path);s.ask('BEGIN;')
            s.ask("UPDATE t SET v='[7,8,9,10]',tail='new-last' WHERE id>=50;")
            for i in range(50,65):m[i][2]=[7.0,8.0,9.0,10.0];m[i][4]='new-last'
            s.ask('COMMIT;');kill(s)
        s=Session(a.binary,path);verify(s,m);s.close()
        # The new format must no longer depend on either legacy recovery input.
        (Path(path)/'STATE').write_bytes(b'obsolete STATE intentionally unreadable')
        (Path(path)/'wal.log').write_bytes(b'obsolete WAL intentionally unreadable')
        s=Session(a.binary,path);verify(s,m);s.close();migrations.append(style)
report={'passed':True,'oracle_checks':checks,'rows':len(model),'migrations':migrations}
(a.root/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report),flush=True)
