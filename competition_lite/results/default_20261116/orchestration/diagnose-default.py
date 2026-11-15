"""Separate diagnostic runs using existing instrumented, hash-checked sources."""
from pathlib import Path
import argparse,hashlib,json,os,random,statistics,struct,sys,time
b=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000');root=b/'default-10000-20261116'
assert json.loads((root/'complete.json').read_text())['passed']
release=b/'readpath-20261115/release';old=b/'mixed-100k-300k-20261115'
sys.path.insert(0,str(release/'source/competition_lite/bench'))
import compare_run as cr
from mixed_stage_run import StageProbe,FUNCTIONS,COUNTERS
os.environ['PATH']='/home/zzp/.local/bin:'+os.environ['PATH']
os.environ['DOCKER_HOST']='unix:///run/user/1002/docker-competition.sock'
prepared=b/'iteration-20261115-ce21bb9/prepared10k';oracle=prepared.parent/'oracle10k.jsonl'
meta=json.loads((prepared/'prepared.json').read_text());cases=json.loads((prepared/'cases.json').read_text())
diagnostics=[('current-9a911a1',release/'bin/diagnostic',release/'diagnostic-source/instrumentation.json'),
 ('scored-f885c28',old/'bin/stages-scored-f885',old/'instrumented/scored-f885/instrumentation.json'),
 ('cpp-449bba1',old/'bin/stages-cpp-449',old/'instrumented/cpp-449/instrumentation.json'),
 ('previous-24d4262',old/'bin/stages-new-24',old/'instrumented/new-24/instrumentation.json')]
complete=[]
for label,binary,instrumentation in diagnostics:
 d=root/'diagnostics'/label;d.mkdir(parents=True,exist_ok=False)
 assert binary.exists() and instrumentation.exists()
 (d/'instrumentation.json').write_bytes(instrumentation.read_bytes())
 probe=StageProbe(d/'data',True)
 try:
  with (d/'stderr.log').open('wb') as err:
   client=cr.Client(str(binary),d/'data',err,'lite:linear-20261115',engine_env=['LITE_PROFILE=1','LITE_SYNC_PROBE=/data/sync-probe.bin'])
   probe.phase('schema');client.ask(meta['schema_sql'])
   probe.phase('insert128');before=client.metrics()
   for line in (prepared/'insert.jsonl').read_bytes().splitlines(keepends=True):client.send(line)
   after=client.metrics();probe.phase('index');client.ask('CREATE VECTOR INDEX idx ON docs(embedding);')
   probe.phase('warmup');rng=random.Random(928313)
   for i in range(18):
    v=struct.unpack('<128f',struct.pack('<128f',*[rng.gauss(0,1) for _ in range(128)]))
    case={'vector':v,'filter':'category' if i%2 else 'none','category':'cat%d'%(i%20)}
    client.ask(cr.query_sql(case),100000+i)
   probe.phase('query');queries=[]
   for i,case in enumerate(cases):
    bf=probe.snapshot_functions();bc=probe.snapshot_counters();start=time.perf_counter()
    out=client.ask(cr.query_sql(case),200000+2*i);elapsed=time.perf_counter()-start
    af=probe.snapshot_functions();ac=probe.snapshot_counters();metrics=out.get('metrics',{})
    metrics['function_ms']={n:(af[j*2+1]-bf[j*2+1])/1e6 for j,n in enumerate(FUNCTIONS)}
    metrics['function_counts']={n:af[j*2]-bf[j*2] for j,n in enumerate(FUNCTIONS)}
    metrics['lsm_counters']={n:ac[j]-bc[j] for j,n in enumerate(COUNTERS)}
    queries.append({'id':case['id'],'mode':'exact','filter':case['filter'],'seconds':elapsed,'rows':out['rows'],'metrics':metrics})
   probe.phase('shutdown');client.close()
  result={'label':label,'diagnostic':True,'binary_sha256':cr.sha(binary),'prepared':str(prepared),'insert_sha256':meta['insert_sha256'],'query_cases_sha256':meta['query_cases_sha256'],'queries':queries,'phases':probe.result(),'resources':{'write_start':before,'write_end':after},'caveat':'Separate instrumented run, nested functions inclusive; insert128 is the complete 10000-row write phase.'}
  (d/'result.json').write_text(json.dumps(result,indent=2)+'\n')
  cr.verify(argparse.Namespace(result=d/'result.json',oracle=oracle,out=d/'verified.json'))
  checked=json.loads((d/'verified.json').read_text());assert checked['verification']['exact']['passed']
  complete.append(label)
 finally:
  for c in list(cr.LIVE_CLIENTS):c.abort()
  probe.close()
 (root/'diagnostics-progress.json').write_text(json.dumps(complete)+'\n')
(root/'diagnostics-complete.json').write_text(json.dumps({'passed':True,'variants':complete})+'\n')
