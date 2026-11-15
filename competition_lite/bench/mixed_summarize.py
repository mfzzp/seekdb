#!/usr/bin/env python3
"""Summarize compressed mixed benchmark evidence; keep failures visible."""
import argparse,gzip,json,statistics
from pathlib import Path

LABELS=['new-24','scored-f885','cpp-449']
PROC='process_diagnostics_not_official_B'

def load(path):
 return json.loads(gzip.decompress(path.read_bytes())) if path.suffix=='.gz' else json.loads(path.read_text())

def sample(values):
 values=[v for v in values if v is not None]
 return {'n':len(values),'median':statistics.median(values),'min':min(values),'max':max(values)} if values else None

def delta(r,start,end,key,process=False):
 a=r.get('resources',{}).get(start,{});b=r.get('resources',{}).get(end,{})
 if process:a=a.get(PROC) or {};b=b.get(PROC) or {}
 return b[key]-a[key] if key in a and key in b else None

def summarize(root):
 result={'primary':{},'sync_diagnostic':{},'internal_diagnostic':{},'failures':[]}
 for size in [100000,300000]:
  for label in LABELS:
   paths=list(root.glob(f'runs/{size}-{label}-r*/result.json.gz'))
   records=[load(p) for p in sorted(paths)];good=[r for r in records if r.get('passed')]
   item={'attempted':len(records),'passed':len(good),'labels':[r['label'] for r in records]}
   for r in records:
    if not r.get('passed'):result['failures'].append({'label':r['label'],'phase':r['phase'],'failure':r.get('failure'),'at_failure':r.get('resources',{}).get('at_failure')})
   for key in ['insert_wall_s','query_wall_s','index_s','warmup_s','checkpoint_s','primary_stage_sum_s','startup_s','restart_s','restore_check_s','final_file_bytes']:
    item[key]=sample([r.get(key) for r in good])
   item['insert_rows_s']=sample([size/r['insert_wall_s'] for r in good]);item['query_qps']=sample([len(r['queries'])/r['query_wall_s'] for r in good])
   item['container_peak_bytes']=sample([r['resources']['after_shutdown']['memory_peak_bytes'] for r in good])
   item['restore_container_peak_bytes']=sample([r['resources']['after_restore']['memory_peak_bytes'] for r in good])
   item['rss_peak_kib']=sample([r['resources']['query_end'][PROC]['peak_rss_kib'] for r in good])
   item['write_dim_s']={str(d):sample([r['write_by_dim_s'][str(d)] for r in good]) for d in [128,1024]}
   item['resources']={}
   for phase,start,end in [('write','write_start','write_end'),('query','query_start','query_end')]:
    fields={'cpu_s':sample([delta(r,start,end,'cpu_s') for r in good])}
    fields.update({k:sample([delta(r,start,end,k,True) for r in good]) for k in ['read_bytes','write_bytes','rchar','wchar','syscr','syscw']})
    fields['device_io_available']=bool(good) and all(r['resources'][end].get('io') for r in good)
    item['resources'][phase]=fields
   item['query_groups']={}
   for key in ['128/none','128/category','1024/none','1024/category']:
    g={name:sample([r['query_groups'][key].get(name) for r in good]) for name in ['total_s','mean_ms','p95_ms','qps','filtered_candidates_mean','full_distance_evaluations_mean','distance_dimensions_mean']}
    g['storage_paths']=sorted(set(p for r in good for p in r['query_groups'][key]['storage_paths']))
    item['query_groups'][key]=g
   item['exact_verified']=sum(r.get('exact_check',{}).get('queries',0) for r in good)
   item['restored_rows_verified']=sum(r.get('verified_rows',0) for r in good)
   result['primary'][f'{size}/{label}']=item
   for directory,section,suffix in [('diagnostic','sync_diagnostic','diagnostic'),('stages','internal_diagnostic','stages')]:
    path=root/f'{directory}/{size}-{label}-{suffix}/result.json.gz'
    if not path.exists():continue
    r=load(path);entry={k:r.get(k) for k in ['passed','phase','failure','insert_wall_s','query_wall_s','query_groups','grouped_query_groups','sync_diagnostic','exact_check']}
    entry['per_group']={}
    if r.get('passed'):
     for order,qs in [('alternating',r['queries']),('grouped',r.get('grouped_queries',[]))]:
      for key in ['128/none','128/category','1024/none','1024/category']:
       group=[q for q in qs if f'{q["dim"]}/{q["filter"]}'==key];fields={}
       for family in ['function_ms','function_counts','lsm_counters']:
        if family=='lsm_counters' and label!='new-24':continue
        metrics=[q['metrics'].get(family,{}) for q in group]
        if metrics and all(metrics):fields[family]={name:sample([m[name] for m in metrics]) for name in metrics[0]}
       entry['per_group'][order+'/'+key]=fields
    result[section][f'{size}/{label}']=entry
 return result

if __name__=='__main__':
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('root',type=Path);ap.add_argument('--output',type=Path,required=True);a=ap.parse_args()
 a.output.write_text(json.dumps(summarize(a.root),indent=2)+'\n',encoding='utf-8',newline='\n')
