from pathlib import Path
import json
b=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/small-parse-20261116/dev2')
out={'server_root':str(b),'primary_rounds':1}
for name in ['complete.json','gate/gate.json','progress.json','paired/summary.json',*[f'paired/{n}-r1.json' for n in ['baseline-hot','fast-parse','parse-off','original9']],*[f'write-profile-{i}/result.json' for i in ['0','1']],'diag/minivectordb_lite.json']:
 out[name]=json.loads((b/name).read_text())
for name in ['parser-check.log','extra-oracle.log']:out[name]=(b/name).read_text()
for label in ['baseline-hot','fast-parse']:
 r=json.loads((b/('profile-'+label)/'result.json').read_text());out['profile-'+label]={'summary':r['summary'],'warmup':r['warmup'],'queries':len(r['queries'])}
print(json.dumps(out,ensure_ascii=False,indent=2))
