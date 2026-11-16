#!/usr/bin/env python3
"""Borrowed vector span lifetime, noncontiguous fallback and mutation checks."""
import argparse,json,math,os,subprocess
from pathlib import Path
from protocol_adapter import Session
ap=argparse.ArgumentParser();ap.add_argument('--binary',required=True);ap.add_argument('--root',type=Path,required=True);a=ap.parse_args();a.root.mkdir(parents=True,exist_ok=False)
os.environ.update(LITE_PROFILE='1',LITE_MMAP_MB='1',LITE_MMAP_KEEP_MB='0');checks=borrowed=owned=0
for columns in [1,2]:
 n=2305;dim=128;model={i:[[float((i*17+j*31)%127)/16 for j in range(dim)],[float(i%17)]*dim] for i in range(n)};sessions=[]
 def open_sessions():
  out=[]
  for flag in [0,1]:os.environ['LITE_MATRIX_VIEW']=str(flag);out.append(Session(a.binary,str(a.root/f'c{columns}-{flag}')))
  return out
 def both(sql):return [s.ask(sql) for s in sessions]
 def verify(expect=None):
  global checks,borrowed,owned
  for col,idx in [('v',0)]+([('w',1)] if columns==2 else []):
   q=[1.0]*dim
   for where,pred in [('',lambda i:True),(' WHERE tag=2',lambda i:i%5==2)]:
    truth=sorted((math.fsum((x-y)**2 for x,y in zip(v[idx],q)),i) for i,v in model.items() if pred(i))[:10]
    out=both(f"SELECT id,L2_DISTANCE({col},'{q}') AS d FROM t{where} ORDER BY d LIMIT 10;")
    assert out[0]['rows']==out[1]['rows']==[[i,math.sqrt(d)] for d,i in truth]
    mt=out[1]['metrics'];borrowed+=bool(mt['matrix_mapped']);owned+=mt['matrix_owned_bytes']>0
    assert out[0]['metrics']['matrix_mapped'] is False
    if expect is not None:assert mt['matrix_mapped']==expect,(columns,where,mt)
    checks+=1
   # Project raw vectors between scans; subsequent queries must reacquire the mapping.
   sql='SELECT id,v'+(',w' if columns==2 else '')+' FROM t WHERE id=0;';out=both(sql);assert out[0]['rows']==out[1]['rows'];checks+=1
 try:
  sessions=open_sessions();both('CREATE TABLE prefix (id BIGINT PRIMARY KEY,v VECTOR(128));');both("INSERT INTO prefix VALUES (0,'"+str([0.0]*dim)+"');")
  both('CREATE TABLE t (id BIGINT PRIMARY KEY,tag BIGINT,v VECTOR(128)'+(',w VECTOR(128)' if columns==2 else '')+');')
  for b in range(0,n,100):both('INSERT INTO t VALUES '+','.join(f"({i},{i%5},'{model[i][0]}'"+(f",'{model[i][1]}'" if columns==2 else '')+')' for i in range(b,min(n,b+100)))+';')
  verify(columns==1);both('BEGIN;');saved=model[0][0];model[0][0]=[0.0]*dim;both("UPDATE t SET v='"+str(model[0][0])+"' WHERE id=0;");verify(columns==1)
  middle=model[777][0];model[777][0]=[0.0]*dim;both("UPDATE t SET v='"+str(model[777][0])+"' WHERE id=777;");verify(False)
  both('ROLLBACK;');model[0][0]=saved;model[777][0]=middle;verify(columns==1)
  both('DELETE FROM t WHERE id=1000;');del model[1000];verify(False)
  for s in sessions:s.close()
  sessions=open_sessions();verify(False)
  for s in sessions:s.close()
  sessions=[]
  for flag in [0,1]:subprocess.run([a.binary,'--data-dir',str(a.root/f'c{columns}-{flag}'),'--maintenance','gc'],check=True,stdout=subprocess.PIPE)
  sessions=open_sessions();verify(columns==1)
 finally:
  for s in sessions:s.close()
assert borrowed>0 and owned>0
print(json.dumps({'passed':True,'queries':checks,'borrowed_queries':borrowed,'owned_queries':owned,'mapping_window_mb':1,'retention_mb':0}))
