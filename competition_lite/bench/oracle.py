#!/usr/bin/env python3
import argparse,json,math,struct

def load(a):
 rows=[]
 with open(a+'/rows.jsonl') as f:
  for x in f: rows.append(json.loads(x))
 with open(a+'/vectors.f32','rb') as f: raw=f.read()
 d=len(raw)//4//len(rows); vs=[struct.unpack_from('<%df'%d,raw,i*d*4) for i in range(len(rows))]
 return rows,vs,d

def ok(r,q):
 f=q.get('filter','none')
 if f=='category': return r['category']=='c01'
 if f=='tenant': return r['tenant_id']==1
 if f=='category_tenant': return r['category']=='c01' and r['tenant_id']==1
 if f=='price_range': return 25000<=r['price_cents']<50000
 if f=='active_day': return r['active']==1 and 100<=r['created_day']<200
 if f=='missing_category': return r['category']=='missing'
 if f=='id_small': return r['id']<100
 if f=='title_exact': return r['title']=='item_00000001'
 return True

def main():
 ap=argparse.ArgumentParser(); ap.add_argument('--data',required=True); ap.add_argument('--out',required=True); a=ap.parse_args()
 rows,vs,d=load(a.data); qs=[json.loads(x) for x in open(a.data+'/query_cases.jsonl')]
 with open(a.out,'w') as out:
  for q in qs:
   qv=q['vector']; z=[]
   for r,v in zip(rows,vs):
    if ok(r,q): z.append((sum((float(x)-float(y))**2 for x,y in zip(v,qv)),r['id']))
   z.sort(key=lambda x:(x[0],x[1])); k=q.get('k',10)
   out.write(json.dumps({'id':q['id'],'ids':[x[1] for x in z[:k]],'distances':[x[0] for x in z[:k]]},separators=(',',':'))+'\n')
if __name__=='__main__': main()
