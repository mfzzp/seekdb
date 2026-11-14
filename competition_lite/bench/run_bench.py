#!/usr/bin/env python3
import argparse,csv,json,os,struct,time,statistics
from protocol_adapter import Session

def esc(s): return "'"+str(s).replace('\\','\\\\').replace("'","\\'")+"'"
def filt(q):
 f=q.get('filter','none')
 return {'none':'','category':" WHERE category='c01'",'tenant':' WHERE tenant_id=1','category_tenant':" WHERE category='c01' AND tenant_id=1",'price_range':' WHERE price_cents>=25000 AND price_cents<50000','active_day':' WHERE active=1 AND created_day>=100 AND created_day<200','missing_category':" WHERE category='missing'",'id_small':' WHERE id<100','title_exact':" WHERE title='item_00000001'"}[f]
def vec(raw,i,d): return '['+','.join(format(x,'.7g') for x in struct.unpack_from('<%df'%d,raw,i*d*4))+']'
def row_sql(r,v): return '('+','.join([str(r['id']),esc(r['category']),str(r['tenant_id']),str(r['price_cents']),str(r['created_day']),str(r['active']),esc(r['title']),esc(v)])+')'
def main():
 ap=argparse.ArgumentParser(); ap.add_argument('--binary',required=True); ap.add_argument('--data',required=True); ap.add_argument('--run-root',required=True); ap.add_argument('--mode',choices=['S1','B256','B1024','T256'],default='B256'); ap.add_argument('--queries',type=int,default=64); ap.add_argument('--rows',type=int); a=ap.parse_args()
 os.makedirs(a.run_root,exist_ok=True); rows=[json.loads(x) for x in open(a.data+'/rows.jsonl')]; raw=open(a.data+'/vectors.f32','rb').read(); d=json.load(open(a.data+'/manifest.json'))['dimension']; n=min(a.rows or len(rows),len(rows)); rows=rows[:n]
 dat=os.path.join(a.run_root,'data'); os.system('rm -rf '+dat); s=Session(a.binary,dat); s.ask('CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), tenant_id BIGINT, price_cents BIGINT, created_day BIGINT, active BIGINT, title VARCHAR(64), embedding VECTOR('+str(d)+'));')
 order=list(range(n)); batch={'S1':1,'B256':256,'B1024':1024,'T256':1}[a.mode]; t0=time.perf_counter(); commits=0
 for begin in range(0,n,batch):
  end=min(n,begin+batch)
  if a.mode=='T256': s.ask('BEGIN;')
  if a.mode in ('B256','B1024'):
   values=','.join(row_sql(rows[i],vec(raw,i,d)) for i in range(begin,end))
   s.ask('INSERT INTO docs VALUES '+values+';')
  else:
   for i in range(begin,end):
    s.ask('INSERT INTO docs VALUES '+row_sql(rows[i],vec(raw,i,d))+';')
  if a.mode=='T256': s.ask('COMMIT;'); commits+=1
 t1=time.perf_counter()
 if a.mode in ('B256','B1024'):
  # the loop above intentionally exercises protocol; record its request count for traceability
  pass
 ti=time.perf_counter(); s.ask('CREATE VECTOR INDEX idx ON docs(embedding);'); tindex=time.perf_counter()-ti
 qs=[json.loads(x) for x in open(a.data+'/query_cases.jsonl')][:a.queries]; qrows=[]; ta=time.perf_counter()
 for q in qs:
  sql="SELECT id,L2_DISTANCE(embedding,"+esc(vec(raw,0,d))+') AS distance FROM docs'+filt(q)+' ORDER BY distance LIMIT '+str(q.get('k',10))+';'
  # use the case vector, not row zero
  qv='['+','.join(format(float(x),'.7g') for x in q['vector'])+']'; sql="SELECT id,L2_DISTANCE(embedding,"+esc(qv)+') AS distance FROM docs'+filt(q)+' ORDER BY distance LIMIT '+str(q.get('k',10))+';'
  for mode in ('exact','ann'):
   z=time.perf_counter(); r=s.ask(sql,mode); dt=time.perf_counter()-z; ids=[x[0] for x in r.get('rows',[])]; qrows.append({'id':q['id'],'mode':mode,'filter':q['filter'],'k':q.get('k',10),'seconds':dt,'ids':ids})
 ta2=time.perf_counter(); s.close(); out={'rows':n,'dimension':d,'mode':a.mode,'insert_seconds':t1-t0,'index_seconds':tindex,'query_seconds':ta2-ta,'query_count':len(qs)*2,'request_count':s.n,'vector_bytes':os.path.getsize(a.data+'/vectors.f32'),'wal_bytes':os.path.getsize(dat+'/wal.log') if os.path.exists(dat+'/wal.log') else 0,'ann_mean_seconds':statistics.mean(x['seconds'] for x in qrows if x['mode']=='ann'),'exact_mean_seconds':statistics.mean(x['seconds'] for x in qrows if x['mode']=='exact')}
 json.dump(out,open(os.path.join(a.run_root,'summary.json'),'w'),indent=2); json.dump(qrows,open(os.path.join(a.run_root,'queries.json'),'w'),indent=2); print(json.dumps(out,ensure_ascii=False))
if __name__=='__main__': main()
