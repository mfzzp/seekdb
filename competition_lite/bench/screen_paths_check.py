#!/usr/bin/env python3
"""128D screen versus original double path and independently computed Exact truth."""
import argparse,json,math,os,random,struct
from pathlib import Path
from protocol_adapter import Session
ap=argparse.ArgumentParser();ap.add_argument('--binary',required=True);ap.add_argument('--root',type=Path,required=True);ap.add_argument('--expect-screen',choices=['yes','no'],default='yes');a=ap.parse_args()
a.root.mkdir(parents=True,exist_ok=False);checks=screened=rejected=0
def f32(x):return struct.unpack('<f',struct.pack('<f',x))[0]
for mode in ['normal','ties','tiny','overflow']:
 rng=random.Random(741);n=2305;model={}
 for i in range(n):
  if mode=='normal':v=[f32(rng.gauss(0,1)) for _ in range(128)]
  elif mode=='ties':v=[float((j+i)%2) for j in range(128)];v[i%128]+=float(i%3)/1024
  else:v=[f32((i+1)*(1e-30 if mode=='tiny' else 1e20))]*128
  model[i-1100]=[i%5,v]
 os.environ['LITE_PROFILE']='1';os.environ['LITE_DISTANCE_SCREEN']='0';old=Session(a.binary,str(a.root/(mode+'-off')))
 os.environ['LITE_DISTANCE_SCREEN']='1';new=Session(a.binary,str(a.root/(mode+'-on')))
 def query(q,where,k):return f"SELECT pk,L2_DISTANCE(v,'{q}') AS d FROM t{where} ORDER BY d LIMIT {k};"
 def verify():
  global checks,screened,rejected
  for q in [[0.0]*128,model[0][1][:]]:
   # Warm the matrix explicitly through a full query; subsequent filtered queries must screen too.
   for where,pred in [('',lambda i:True),(' WHERE tag=2',lambda i:model[i][0]==2),(' WHERE pk<0',lambda i:i<0)]:
    truth=sorted((math.fsum((x-y)**2 for x,y in zip(v,q)),i) for i,(tag,v) in model.items() if pred(i))
    for k in [1,10,257,2500]:
     x=old.ask(query(q,where,k));y=new.ask(query(q,where,k));assert x['rows']==y['rows'],(mode,where,k,'baseline mismatch')
     assert [r[0] for r in y['rows']]==[i for d,i in truth[:k]],(mode,where,k,'truth IDs')
     assert all(math.isclose(r[1],math.sqrt(d),rel_tol=1e-12,abs_tol=0) for r,(d,i) in zip(y['rows'],truth[:k]))
     assert y['metrics']['filtered_candidates']==len(truth)
     screened+=y['metrics'].get('screened_vectors',0);rejected+=y['metrics'].get('screen_rejected',0);checks+=1
 try:
  for s in [old,new]:
   s.ask('CREATE TABLE t (pk BIGINT PRIMARY KEY,tag BIGINT,v VECTOR(128));')
   order=list(model);rng.shuffle(order)
   for b in range(0,n,100):s.ask('INSERT INTO t VALUES '+','.join(f"({i},{model[i][0]},'{model[i][1]}')" for i in order[b:b+100])+';')
  verify();saved={i:[tag,v[:]] for i,(tag,v) in model.items()}
  for s in [old,new]:
   s.ask('BEGIN;');s.ask("UPDATE t SET tag=2,v='"+str([0.0]*128)+"' WHERE pk=-1100;");s.ask('DELETE FROM t WHERE pk=1204;')
  model[-1100]=[2,[0.0]*128];del model[1204];verify()
  for s in [old,new]:s.ask('ROLLBACK;')
  model=saved;verify()
 finally:old.close();new.close()
 os.environ['LITE_DISTANCE_SCREEN']='0';old=Session(a.binary,str(a.root/(mode+'-off')))
 os.environ['LITE_DISTANCE_SCREEN']='1';new=Session(a.binary,str(a.root/(mode+'-on')))
 try:verify()
 finally:old.close();new.close()
 print(json.dumps({'mode':mode,'queries':checks,'screened':screened,'rejected':rejected}),flush=True)
assert (screened>0 and rejected>0) if a.expect_screen=='yes' else (screened==0 and rejected==0)
print(json.dumps({'passed':True,'queries':checks,'screened':screened,'rejected':rejected}))
