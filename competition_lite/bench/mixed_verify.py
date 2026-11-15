#!/usr/bin/env python3
"""Verify archived hashes, run identities and exact results before publication."""
import argparse,gzip,hashlib,json,math
from pathlib import Path

def load(p):
 return json.loads(gzip.decompress(p.read_bytes())) if p.suffix=='.gz' else json.loads(p.read_text())

def verify(root):
 inventory=load(root/'inventory.json')
 for item in inventory:
  data=gzip.decompress((root/(item['path']+'.gz')).read_bytes())
  assert len(data)==item['bytes'] and hashlib.sha256(data).hexdigest()==item['sha256'],item['path']
 builds=load(root/'builds.json.gz');passed=failed=queries=rows_checked=0;outcomes=[]
 for size in [100000,300000]:
  meta=load(root/f'input-{size}/prepared.json.gz');truth={}
  assert meta['rows']==size and meta['rows_per_dimension']==size//2 and meta['queries']==1000
  for dim in [128,1024]:
   for line in gzip.decompress((root/f'input-{size}/{dim}/oracle.jsonl.gz').read_bytes()).splitlines():
    case=json.loads(line);assert case['id'] not in truth;truth[case['id']]=case['rows']
  assert len(truth)==1000
  for label in ['new-24','scored-f885','cpp-449']:
   for repeat in [1,2,3]:
    name=f'{size}-{label}-r{repeat}';r=load(root/f'runs/{name}/result.json.gz')
    assert r['label']==name and r['binary_sha256']==builds['variants'][label]['binary_sha256']
    assert r['prepared_sha256']==meta['sha256'] and r['limits']=={'cpus':2,'engine_affinity':[0,1],'memory_bytes':2<<30,'swap':0}
    if not r['passed']:
     assert r.get('failure') and r['phase']!='complete';failed+=1;outcomes.append({'run':name,'passed':False,'phase':r['phase']});continue
    assert r['phase']=='complete' and r['commits_done']==size//100 and r['restart_pass'] and r['verified_rows']==size
    assert r['exact_check']['passed'] and r['exact_check']['queries']==1000 and len(r['queries'])==1000
    assert {q['id'] for q in r['queries']}==set(range(1000))
    for q in r['queries']:
     expected=truth[q['id']];assert [a[0] for a in q['rows']]==[a[0] for a in expected],(name,q['id'])
     for actual,target in zip(q['rows'],expected):assert math.isclose(actual[1],target[1],rel_tol=1e-5,abs_tol=1e-5),(name,q['id'],actual,target)
    assert all(v['count']==250 for v in r['query_groups'].values())
    passed+=1;queries+=len(r['queries']);rows_checked+=r['verified_rows'];outcomes.append({'run':name,'passed':True})
 diagnostic_counts={}
 for folder in ['diagnostic','stages']:
  paths=sorted((root/folder).glob('*/result.json.gz'));assert len(paths)==6,folder
  for path in paths:
   r=load(path)
   assert r['passed'] and r['phase']=='complete' and r['exact_check']['passed'],path
   assert r['exact_check']['queries']==96 and len(r['queries'])==32 and len(r['grouped_queries'])==64,path
  diagnostic_counts[folder]=len(paths)
 audit=load(root/'datasets-verified.json');assert audit['all_match'];payloads=0
 assert [d['rows'] for d in audit['datasets']]==[200,100000,300000]
 for dataset in audit['datasets']:
  meta=load(root/f'input-{dataset["rows"]}/prepared.json.gz')
  assert {f['path']:f['sha256'] for f in dataset['files']}==meta['sha256']
  assert all(f['matches_prepared'] for f in dataset['files']);payloads+=len(dataset['files'])
 return {'archive_files_verified':len(inventory),'main_runs':passed+failed,'passed':passed,'failed':failed,'exact_queries_rechecked':queries,'full_restore_rows_recorded':rows_checked,'diagnostic_passed':diagnostic_counts,'postrun_payload_files_recorded':payloads,'outcomes':outcomes}

if __name__=='__main__':
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('root',type=Path);ap.add_argument('--output',type=Path,required=True);a=ap.parse_args()
 result=verify(a.root);a.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8',newline='\n');print(json.dumps({k:v for k,v in result.items() if k!='outcomes'}))
