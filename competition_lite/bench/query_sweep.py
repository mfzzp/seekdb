#!/usr/bin/env python3
import argparse,json,os,time,statistics
from protocol_adapter import Session

def qfilter(f):
 return {'none':'','category':" WHERE category='c01'",'tenant':' WHERE tenant_id=1','category_tenant':" WHERE category='c01' AND tenant_id=1",'price_range':' WHERE price_cents>=25000 AND price_cents<50000','active_day':' WHERE active=1 AND created_day>=100 AND created_day<200','missing_category':" WHERE category='missing'",'id_small':' WHERE id<100','title_exact':" WHERE title='item_00000001'"}[f]
def main():
 ap=argparse.ArgumentParser(); ap.add_argument('--binary',required=True); ap.add_argument('--data-dir',required=True); ap.add_argument('--cases',required=True); ap.add_argument('--out',required=True); a=ap.parse_args()
 s=Session(a.binary,a.data_dir); qs=[json.loads(x) for x in open(a.cases)]; times=[]; counts={}
 for q in qs:
  v='['+','.join(format(float(x),'.7g') for x in q['vector'])+']'; sql="SELECT id,L2_DISTANCE(embedding,'"+v+"') AS distance FROM docs"+qfilter(q['filter'])+' ORDER BY distance LIMIT '+str(q.get('k',10))+';'
  for mode in ('exact','ann'):
   t=time.perf_counter(); r=s.ask(sql,mode); dt=time.perf_counter()-t; m=r.get('metrics',{}); times.append({'id':q['id'],'mode':mode,'seconds':dt,'rows':len(r.get('rows',[])),'planner_path':m.get('planner_path'),'candidate_count':m.get('candidate_count',0),'raw_candidates':m.get('raw_candidates',0),'probed_lists':m.get('probed_lists',0)}); counts[(mode,q['filter'])]=counts.get((mode,q['filter']),0)+1
 s.close(); out={'cases':len(qs),'requests':len(times),'exact_mean_s':statistics.mean(x['seconds'] for x in times if x['mode']=='exact'),'ann_mean_s':statistics.mean(x['seconds'] for x in times if x['mode']=='ann'),'ann_raw_candidates_mean':statistics.mean(x['raw_candidates'] for x in times if x['mode']=='ann'),'ann_planner_paths':sorted(set(x['planner_path'] for x in times if x['mode']=='ann')),'filter_counts':{str(k):v for k,v in counts.items()}}; json.dump({'summary':out,'requests':times},open(a.out,'w'),indent=2); print(json.dumps(out))
if __name__=='__main__': main()
