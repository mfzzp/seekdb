#!/usr/bin/env python3
"""Exercise post-index inserts, scalar/vector updates, deletes and rollback."""
import argparse,json,os,statistics,time
from protocol_adapter import Session

def q(s,sql,mode='exact'):
    return s.ask(sql,mode).get('rows',[])

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--binary',required=True); ap.add_argument('--data-dir',required=True); ap.add_argument('--rows',type=int,required=True); ap.add_argument('--dim',type=int,default=1024); ap.add_argument('--limit',type=int); a=ap.parse_args()
    n=a.rows; m=min(a.limit or max(1,n//100),max(1,n//100)); base=list(range(1,n+1)); new0=n+1; vec='['+','.join(['0.125']*a.dim)+']'; s=Session(a.binary,a.data_dir); times={}
    ins=[f"({i},'c19',9,99999,364,1,'mut_{i}','{vec}')" for i in range(new0,new0+m)]
    t=time.perf_counter()
    for b in range(0,len(ins),128): s.ask('INSERT INTO docs VALUES '+','.join(ins[b:b+128])+';')
    times['insert_seconds']=time.perf_counter()-t
    got=q(s,f'SELECT id FROM docs WHERE id>={new0} AND id<{new0+m} ORDER BY id;'); assert len(got)==m,(len(got),m)
    scalar_ids=base[:m]; vector_ids=base[m:2*m]; delete_ids=base[2*m:3*m]
    t=time.perf_counter()
    for i in scalar_ids: s.ask(f"UPDATE docs SET category='c19',price_cents=99999 WHERE id={i};")
    times['scalar_update_seconds']=time.perf_counter()-t
    t=time.perf_counter()
    for i in vector_ids: s.ask(f"UPDATE docs SET embedding='{vec}' WHERE id={i};")
    times['vector_update_seconds']=time.perf_counter()-t
    t=time.perf_counter()
    for i in delete_ids: s.ask(f'DELETE FROM docs WHERE id={i};')
    times['delete_seconds']=time.perf_counter()-t
    assert len(q(s,f"SELECT id FROM docs WHERE category='c19' AND id<{new0} ORDER BY id;"))>=m
    assert q(s,f'SELECT id FROM docs WHERE id={delete_ids[0]};')==[]
    rollback_id=3*m+1; old=q(s,f'SELECT id,category FROM docs WHERE id={rollback_id};')
    s.ask('BEGIN;'); s.ask(f"INSERT INTO docs VALUES ({n+m+1},'rollback',0,1,1,1,'rollback','{vec}');"); s.ask(f"UPDATE docs SET category='rollback' WHERE id={rollback_id};"); s.ask(f'DELETE FROM docs WHERE id={rollback_id+1};'); assert q(s,f'SELECT id FROM docs WHERE id={n+m+1};')
    s.ask('ROLLBACK;'); assert q(s,f'SELECT id FROM docs WHERE id={n+m+1};')==[] and q(s,f'SELECT id,category FROM docs WHERE id={rollback_id};')==old
    s.close(); s=Session(a.binary,a.data_dir)
    assert len(q(s,f'SELECT id FROM docs WHERE id>={new0} AND id<{new0+m} ORDER BY id;'))==m
    assert q(s,f'SELECT id FROM docs WHERE id={delete_ids[0]};')==[]
    assert q(s,f"SELECT id FROM docs WHERE id={scalar_ids[0]} AND category='c19';")
    s.close(); times['requests']=s.n; print(json.dumps({'rows':n,'mutation_count':m,'deleted':len(delete_ids),'vector_updated':len(vector_ids),'post_restart':'PASS',**times},sort_keys=True))
if __name__=='__main__': main()
