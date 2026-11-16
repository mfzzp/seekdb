from pathlib import Path
import json,os,statistics,subprocess,time
b=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/composite-20261116/json1');assert json.loads((b/'acceptance.json').read_text())['passed']
bench=b/'source/competition_lite/bench';prep=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/iteration-20261115-ce21bb9/prepared10k')
env=dict(os.environ,PATH='/home/zzp/.local/bin:'+os.environ['PATH'],DOCKER_HOST='unix:///run/user/1002/docker-competition.sock')
cases=json.loads((prep/'cases.json').read_text());meta=json.loads((prep/'prepared.json').read_text());out={}
for flag in ['0','1']:
 label='json-write-'+flag
 with (b/(label+'.log')).open('w') as log:
  subprocess.run(['python3',str(bench/'compare_run.py'),'run','--prepared',str(prep),'--root',str(b/'diagnostics'),'--binary',str(b/'bin/diagnostic'),'--label',label,'--image','lite:json-scan-20261116','--engine-env','LITE_JSON_SCAN='+flag],env=env,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=300)
  result,= (b/'diagnostics').glob(label+'-*/result.json')
  subprocess.run(['python3',str(bench/'compare_run.py'),'verify','--result',str(result),'--oracle',str(prep.parent/'oracle10k.jsonl'),'--out',str(b/(label+'-verified.json'))],env=env,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=300)
 checked=json.loads((b/(label+'-verified.json')).read_text());assert checked['verification']['exact']['passed']
 traces=[json.loads(s) for s in result.with_name('stderr.log').read_text().splitlines() if s.startswith('{"diagnostic":')]
 start=1+meta['requests']+1+18;queries=traces[start:start+len(cases)];assert len(queries)==1000
 groups={}
 for f in ['none','category']:
  ts=[t for c,t in zip(cases,queries) if c['filter']==f]
  groups[f]={n:statistics.median(t['stats'].get(n,{}).get('ms',0) for t in ts) for n in ['json_parse','sql_lex','vector_parse','json_encode']}
 write={}
 for t in traces[1:1+meta['requests']]:
  for n,s in t['stats'].items():
   if n not in ['json_parse','sql_lex','json_encode','vector_sync','wal_sync','vector_parse','io_wal','io_vector','io_sst_flush','io_manifest','io_sst_compact','begin_copy','statement_copy']:continue
   d=write.setdefault(n,{k:0 for k in s})
   for k,v in s.items():d[k]+=v
 out[flag]={'query_groups_median_ms':groups,'write':write,'passed':True,'binary_sha256':checked['binary_sha256'],'source_profile':'LITE_PROFILE unset; normal response shape, separate instrumented timing'}
(b/'json-write-diagnostic.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(out))

