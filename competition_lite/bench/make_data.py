#!/usr/bin/env python3
"""Generate the fixed eight-column benchmark without reading engine internals."""
import argparse, hashlib, json, math, random, struct
from pathlib import Path

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--rows',type=int,required=True); ap.add_argument('--out',type=Path,required=True); ap.add_argument('--dim',type=int,default=1024); ap.add_argument('--seed',type=int,default=20261113); ap.add_argument('--vector-mode',choices=['mixed','random'],default='mixed')
    a=ap.parse_args(); a.out.mkdir(parents=True,exist_ok=True); d=a.dim
    center_rng=random.Random((a.seed<<16)^a.rows^11); vec_rng=random.Random((a.seed<<16)^a.rows^17)
    attr_rng=random.Random((a.seed<<16)^a.rows^23); order_rng=random.Random((a.seed<<16)^a.rows^29)
    centers=[[center_rng.gauss(0,1) for _ in range(d)] for _ in range(128)]
    order=list(range(1,a.rows+1)); order_rng.shuffle(order)
    cols=[]; vf=open(a.out/'vectors.f32','wb'); rowsf=open(a.out/'rows.jsonl','w',encoding='utf-8')
    for i in range(1,a.rows+1):
        category=f'c{attr_rng.randrange(20):02d}'; tenant=attr_rng.randrange(10); price=attr_rng.randrange(100000); day=attr_rng.randrange(365); active=1 if attr_rng.random()<.85 else 0
        title=f'item_{i:08d}' if i%1000 else f"item_{i:08d}_quote_'_semi"
        if a.vector_mode=='mixed' and i*10<=a.rows*7:
            c=centers[vec_rng.randrange(len(centers))]; v=[float(c[j]+.15*vec_rng.gauss(0,1)) for j in range(d)]
        else: v=[float(vec_rng.gauss(0,1)) for _ in range(d)]
        vf.write(struct.pack('<%df'%d,*v)); cols.append((i,category,tenant,price,day,active,title)); rowsf.write(json.dumps({'id':i,'category':category,'tenant_id':tenant,'price_cents':price,'created_day':day,'active':active,'title':title},ensure_ascii=False)+'\n')
    vf.close(); rowsf.close(); (a.out/'load_order.u32').write_bytes(b''.join(struct.pack('<I',x) for x in order))
    q_rng=random.Random((a.seed<<16)^a.rows^41); queries=[]
    for qi in range(1024): queries.append({'id':qi,'filter':['none','category','tenant','category_tenant','price_range','active_day','missing_category','id_small','title_exact'][qi%9],'k':10,'vector':[float(q_rng.gauss(0,1)) for _ in range(d)]})
    with open(a.out/'query_cases.jsonl','w') as f:
        for q in queries:f.write(json.dumps(q,separators=(',',':'))+'\n')
    manifest={'rows':a.rows,'dimension':d,'seed':a.seed,'vector_mode':a.vector_mode,'schema_columns':8,'vector_file':'vectors.f32','rows_file':'rows.jsonl'}
    for p in ['vectors.f32','rows.jsonl','load_order.u32','query_cases.jsonl']:
        h=hashlib.sha256((a.out/p).read_bytes()).hexdigest();manifest.setdefault('sha256',{})[p]=h
    (a.out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
if __name__=='__main__':main()
