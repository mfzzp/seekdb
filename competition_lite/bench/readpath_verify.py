#!/usr/bin/env python3
"""Verify archived bytes and ensure headline runs use the frozen inputs/runtime."""
import argparse,gzip,hashlib,json
from pathlib import Path

ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('root',type=Path);ap.add_argument('--baseline',type=Path,required=True)
a=ap.parse_args();root=a.root
def load(p):return json.loads(gzip.decompress(p.read_bytes())) if p.suffix=='.gz' else json.loads(p.read_bytes())
manifest=load(root/'release-evidence-SHA256.json')
for name,item in manifest.items():
 data=gzip.decompress((root/(name+'.gz')).read_bytes())
 assert len(data)==item['bytes'] and hashlib.sha256(data).hexdigest()==item['sha256'],name
meta=load(root/'release/runtime-identity.json.gz');checked=[]
for rows in [100000,300000]:
 old=load(a.baseline/f'runs/{rows}-scored-f885-r1/result.json.gz')
 for repeat in range(1,4):
  name=f'{rows}-readpath-r{repeat}';r=load(root/f'release/runs/{name}/result.json.gz')
  assert r['binary_sha256']==meta['binary_sha256'],name
  assert r['prepared_sha256']==old['prepared_sha256'],name
  assert r['limits']==old['limits'] and r['batch']==old['batch']==100,name
  assert not r['diagnostic'] and r['engine_env']==[],name
  assert r['passed'] and r['restart_pass'] and r['verified_rows']==rows,name
  assert r['exact_check']['passed'] and r['exact_check']['queries']==1000,name
  assert [q['id'] for q in r['queries']]==[q['id'] for q in old['queries']],name
  checked.append({'label':name,'binary_sha256':r['binary_sha256'],'full_restore_rows':rows,'restore_verifier':r.get('restore_verifier','open-ended-primary-range-v1')})
for p in sorted((root/'release/diagnostics').glob('*/result.json.gz')):
 r=load(p);old=load(a.baseline/f'runs/{r["rows"]}-scored-f885-r1/result.json.gz')
 assert r['prepared_sha256']==old['prepared_sha256'] and r['diagnostic'] and r['passed'],p
 assert r['exact_check']['passed'] and r['exact_check']['queries']==96,p
 assert 'LITE_ROW_CACHE_MB=0' in r['engine_env'],p
assert load(root/'release/gate/gate.json.gz')['passed']
assert load(root/'release/complete.json.gz')['passed']
result={'passed':True,'archived_files_verified':len(manifest),'same_frozen_input_and_main_query_order':True,'main_runs':checked,'official_device_io':'unavailable; not replaced with process counters'}
(root/'verification.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8',newline='\n')
print(json.dumps({'passed':True,'files':len(manifest),'main_runs':len(checked)}))
