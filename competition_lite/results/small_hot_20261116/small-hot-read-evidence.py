from pathlib import Path
import json
b=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/small-hot-20261116/dev1')
out={'server_root':str(b),'primary_rounds':1,'anomaly_followup':'Separate request interleaving to limit drift; first harness correlated ordering with filter, superseded by balanced grouping.'}
names=['complete.json','gate/gate.json','progress.json','extra.json','paired/summary.json','paired/baseline9-r1.json','paired/hot-r1.json','paired/hot-off-r1.json','interleaved/baseline9/verified.json','interleaved/hot/verified.json','balanced/baseline9/verified.json','balanced/hot/verified.json']
for name in names:out[name]=json.loads((b/name).read_text())
for name in ['hot-check.log','extra-oracle.log']:out[name]=(b/name).read_text()
for label in ['baseline9','hot']:
 r=json.loads((b/('profile-'+label)/'result.json').read_text());qs=r['queries'];out['profile-'+label]={'summary':r['summary'],'warmup':r['warmup'],'queries':len(qs),'hot_posting_builds_after_warmup':sum(q.get('hot_postings_builds',0) for q in qs),'hot_posting_bytes_max':max(q.get('hot_postings_bytes',0) for q in qs),'hot_posting_uses':sum(q.get('hot_postings',False) for q in qs)}
print(json.dumps(out,ensure_ascii=False,indent=2))
