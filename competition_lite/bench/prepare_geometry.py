#!/usr/bin/env python3
"""Deterministic clustered inputs with shuffled physical load order; not official."""
import argparse,json,random,struct
from pathlib import Path
from compare_run import literal,row_sql,wire,sha
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--template',type=Path,required=True);ap.add_argument('--out',type=Path,required=True);ap.add_argument('--dim',type=int,required=True)
a=ap.parse_args();root=a.out;root.mkdir(parents=True,exist_ok=False);data=root/'data';data.mkdir();prep=root/'prepared';prep.mkdir()
n=10000;d=a.dim;rng=random.Random(77112);order=list(range(1,n+1));rng.shuffle(order)
rows=[{'id':i,'category':f'cat{i%20}','content':f'row{i}'} for i in range(1,n+1)]
vectors=[[float((i%32)*32)+rng.randint(-16,16)/32 for _ in range(d)] for i in range(1,n+1)]
with (data/'vectors.f32').open('wb') as f:
 for v in vectors:f.write(struct.pack(f'<{d}f',*v))
(data/'rows.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in rows))
(data/'load_order.u32').write_bytes(struct.pack(f'<{n}I',*order))
with (prep/'insert.jsonl').open('wb') as f:
 for begin in range(0,n,100):f.write(wire('INSERT INTO docs VALUES '+','.join(row_sql(rows[i-1],literal(vectors[i-1])) for i in order[begin:begin+100])+';',1000+begin//100))
cases=[]
with (prep/'queries.f32').open('wb') as qf,(prep/'cases.tsv').open('w') as cf:
 for i in range(100):
  q=[float((i%32)*32)+rng.randint(-16,16)/32 for _ in range(d)]
  for mask,kind in enumerate(['none','category','large']):
   c={'id':len(cases),'filter':kind,'vector':q,'k':10}
   if kind=='category':c['category']='cat3'
   if kind=='large':c['where']='id<=8000'
   cases.append(c);qf.write(struct.pack(f'<{d}f',*q));cf.write(f'{c["id"]} {mask} 10\n')
(prep/'cases.json').write_text(json.dumps(cases))
(prep/'masks.u8').write_bytes(b''.join(bytes(int(flag) for flag in group) for group in [[True]*n,[i%20==3 for i in range(1,n+1)],[i<=8000 for i in range(1,n+1)]]))
meta=json.loads((a.template/'prepared.json').read_text());meta.update(data=str(data),rows=n,dim=d,batch=100,requests=100,modes=['exact'],insert_sha256=sha(prep/'insert.jsonl'),query_cases_sha256=sha(prep/'cases.json'),schema_sql=f'CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(32), content VARCHAR(1024), embedding VECTOR({d}));',input_manifest={'kind':'shuffled-clustered','rows':n,'dimension':d,'seed':77112,'clusters':32})
meta['request_bytes']=(prep/'insert.jsonl').stat().st_size;meta['max_request_bytes']=max(map(len,(prep/'insert.jsonl').read_bytes().splitlines()))+1
(prep/'prepared.json').write_text(json.dumps(meta,indent=2)+'\n')
print(json.dumps({'prepared':str(prep),'input_sha256':meta['insert_sha256']}))
