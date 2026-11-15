from pathlib import Path
import json,os,shutil,subprocess,sys,time
b=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/readpath-20261115/phase3');sys.path.insert(0,str(b/'source/competition_lite/bench'))
from mixed_compare import MixedClient,sql,stats,verify_queries
os.environ.update(PATH='/home/zzp/.local/bin:'+os.environ['PATH'],DOCKER_HOST='unix:///run/user/1002/docker-competition.sock')
prep=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/mixed-100k-300k-20261115/input-100000');cases=json.loads((prep/'cases.json').read_text());warm=json.loads((prep/'warmup.json').read_text());requests=(prep/'queries.jsonl').read_bytes().splitlines(keepends=True)
for mb in [32,1024]:
 root=b/f'map-probe-{mb}';root.mkdir(exist_ok=False);shutil.copytree(b/'mixed100k-r1/data',root/'data');out={'window_mb':mb,'scope':'query-only on identical copies of restarted data; not primary score'}
 with (root/'stderr.log').open('wb') as err:
  client=MixedClient(str(b/'engine'),root/'data',err,'lite:linear-20261115',engine_env=[f'LITE_MMAP_MB={mb}'])
  try:
   for c in warm:client.ask(sql(c),c['id'])
   out['before']=client.metrics();records=[];start=time.perf_counter()
   for c,request in zip(cases,requests):
    t=time.perf_counter();r=client.send(request);records.append({'id':c['id'],'dim':c['dim'],'filter':c['filter'],'seconds':time.perf_counter()-t,'rows':r['rows'],'metrics':r.get('metrics',{})})
   out['query_wall_s']=time.perf_counter()-start;out['after']=client.metrics();out['groups']=stats(records);out['exact']=verify_queries(prep,records);out['final']=client.close()
  finally:client.abort()
 out['queries']=records;(root/'result.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps({k:out[k] for k in ['window_mb','query_wall_s','groups','exact']}),flush=True)
