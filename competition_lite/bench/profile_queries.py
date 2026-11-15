#!/usr/bin/env python3
"""Diagnostic-only stage timings over prepared requests; never used as final timing."""
import argparse,json,os,statistics,time
from pathlib import Path
from protocol_adapter import Session
from compare_run import query_sql

ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--binary',required=True);ap.add_argument('--prepared',type=Path,required=True)
ap.add_argument('--root',type=Path,required=True);a=ap.parse_args()
a.root.mkdir(parents=True,exist_ok=False)
meta=json.loads((a.prepared/'prepared.json').read_text())
cases=json.loads((a.prepared/'cases.json').read_text())
os.environ['LITE_PROFILE']='1'
s=Session(a.binary,str(a.root/'db'));records=[]
try:
 s.ask(meta['schema_sql'])
 for line in (a.prepared/'insert.jsonl').open():s.ask(json.loads(line)['sql'])
 s.ask('CREATE VECTOR INDEX idx ON docs(embedding);')
 for c in cases[:18]:s.ask(query_sql(c))
 for c in cases:
  sql=query_sql(c);start=time.perf_counter();r=s.ask(sql)
  records.append({'filter':c['filter'],'request_ms':(time.perf_counter()-start)*1000,**r['metrics']})
finally:s.close()
summary={}
for kind in sorted({r['filter'] for r in records}):
 group=[r for r in records if r['filter']==kind]
 keys=group[0]['profile_ms'].keys()
 summary[kind]={k:statistics.median(r['profile_ms'][k] for r in group) for k in keys}
 summary[kind].update(request_ms=statistics.median(r['request_ms'] for r in group),candidates=statistics.median(r['filtered_candidates'] for r in group))
(a.root/'result.json').write_text(json.dumps({'summary':summary,'queries':records},indent=2)+'\n')
print(json.dumps(summary),flush=True)
