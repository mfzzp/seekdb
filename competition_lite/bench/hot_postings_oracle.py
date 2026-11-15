#!/usr/bin/env python3
"""Independent float32 truth for two seeds, selectivities and mutated views."""
import argparse,json,math,os,random,struct
from pathlib import Path
from protocol_adapter import Session
ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--binary',required=True);ap.add_argument('--root',type=Path,required=True);a=ap.parse_args()
a.root.mkdir(parents=True,exist_ok=False);os.environ['LITE_PROFILE']='1';checks=0
for seed in [41729,85103]:
 rng=random.Random(seed);n=1000;dim=128;model={}
 def f32(v):return list(struct.unpack('<128f',struct.pack('<128f',*v)))
 for i in range(n):
  tag=0 if i<800 else 1 if i<900 else 2 if i<910 else 3 if i==910 else 4
  model[i-500]=[tag,f32([rng.gauss(0,.5) for _ in range(dim)])]
 order=list(model);rng.shuffle(order);s=Session(a.binary,str(a.root/str(seed)))
 try:
  s.ask('CREATE TABLE t (pk BIGINT PRIMARY KEY,tag BIGINT,v VECTOR(128));')
  for base in range(0,n,100):s.ask('INSERT INTO t VALUES '+','.join(f"({i},{model[i][0]},'{model[i][1]}')" for i in order[base:base+100])+';')
  queries=[f32([rng.gauss(0,.5) for _ in range(dim)]),model[0][1][:]]
  def verify():
   global checks
   for q in queries:
    for tag in [None,0,1,2,3,9]:
     truth=sorted((math.fsum((x-y)**2 for x,y in zip(v,q)),pk) for pk,(g,v) in model.items() if tag is None or g==tag)
     for k in [1,10,17]:
      where='' if tag is None else f' WHERE tag={tag}'
      out=s.ask(f"SELECT pk,L2_DISTANCE(v,'{q}') AS d FROM t{where} ORDER BY d LIMIT {k};")
      assert [r[0] for r in out['rows']]==[pk for d,pk in truth[:k]],(seed,tag,k)
      assert all(math.isclose(row[1],math.sqrt(d),rel_tol=1e-12,abs_tol=1e-12) for row,(d,pk) in zip(out['rows'],truth[:k]))
      assert out['metrics']['filtered_candidates']==len(truth)
      checks+=1
  verify();saved={i:[tag,v[:]] for i,(tag,v) in model.items()};s.ask('BEGIN;')
  s.ask("UPDATE t SET tag=3,v='"+str([0.0]*dim)+"' WHERE pk=-500;");model[-500]=[3,[0.0]*dim]
  s.ask('DELETE FROM t WHERE pk=410;');del model[410];verify()
  s.ask('ROLLBACK;');model=saved;verify()
 finally:s.close()
print(json.dumps({'passed':True,'seeds':[41729,85103],'queries':checks,'candidate_counts_before_mutation':[1000,800,100,10,1,0]}))
