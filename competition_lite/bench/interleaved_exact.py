#!/usr/bin/env python3
"""One alternating-request comparison to limit drift in short query intervals."""
import argparse,json,random,struct,time
from pathlib import Path
from compare_run import Client,LIVE_CLIENTS,query_sql,wire,sha,verify
ap=argparse.ArgumentParser(description=__doc__)
for name in ['prepared','oracle','root']:ap.add_argument('--'+name,type=Path,required=True)
ap.add_argument('--image',required=True);ap.add_argument('--variant',action='append',required=True)
a=ap.parse_args();a.root.mkdir(parents=True,exist_ok=False)
meta=json.loads((a.prepared/'prepared.json').read_text());cases=json.loads((a.prepared/'cases.json').read_text())
variants=[v.split('=',1) for v in a.variant];assert len(variants)==2
clients={};logs=[];queries={label:[] for label,_ in variants};stages={}
try:
 for label,binary in variants:
  d=a.root/label;d.mkdir();log=(d/'stderr.log').open('wb');logs.append(log)
  c=Client(binary,d/'data',log,a.image);clients[label]=c;c.ask(meta['schema_sql'])
  start=time.perf_counter()
  for line in (a.prepared/'insert.jsonl').read_bytes().splitlines(keepends=True):c.send(line)
  stages[label]={'write_s':time.perf_counter()-start}
  start=time.perf_counter();c.ask('CREATE VECTOR INDEX idx ON docs(embedding);');rng=random.Random(928313)
  for i in range(18):
   v=struct.unpack('<%df'%meta['dim'],struct.pack('<%df'%meta['dim'],*[rng.gauss(0,1) for _ in range(meta['dim'])]))
   case={'vector':v,'filter':'category' if i%2 else 'none','category':'cat%d'%(i%20)};c.ask(query_sql(case),100000+i)
  stages[label]['prepare_s']=time.perf_counter()-start
 # Inputs alternate unfiltered/filtered; swap every pair to balance both groups.
 for i,case in enumerate(cases):
  request=wire(query_sql(case),200000+2*i)
  for label,_ in variants[::1 if (i//2)%2==0 else -1]:
   start=time.perf_counter();out=clients[label].send(request);elapsed=time.perf_counter()-start
   queries[label].append({'id':case['id'],'mode':'exact','filter':case['filter'],'seconds':elapsed,'rows':out['rows'],'metrics':out.get('metrics',{})})
 for label,binary in variants:
  resources=clients[label].close();r={'label':label,'binary_sha256':sha(binary),'prepared':str(a.prepared),'insert_sha256':meta['insert_sha256'],'query_cases_sha256':meta['query_cases_sha256'],'queries':queries[label],'stages':stages[label],'resources':resources,'note':'One query pass; request-order alternation, no concurrent work. SQL generation is outside request timing. Use standard isolated runs for whole-workflow/resource comparisons.'}
  (a.root/label/'result.json').write_text(json.dumps(r,indent=2)+'\n')
  verify(argparse.Namespace(result=a.root/label/'result.json',oracle=a.oracle,out=a.root/label/'verified.json'))
  checked=json.loads((a.root/label/'verified.json').read_text());assert checked['verification']['exact']['passed']
finally:
 for c in list(LIVE_CLIENTS):c.abort()
 for log in logs:log.close()
