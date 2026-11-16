from pathlib import Path
import json
base=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/distance-screen-20261116')
out={'server_root':str(base),'baseline_commit':'05212db56e98bfee694fe6c2ff13ed65d8101bff','primary_rounds_per_development_variant':1}
for stage in ['dev1','dev2','dev3','dev4']:
 b=base/stage;data={}
 names=['progress.json','complete.json','paired/summary.json']
 if stage=='dev4':names+=['gate/gate.json','acceptance-progress.json','acceptance.json','screen-write-diagnostic.json','diag/minivectordb_lite.json',*[f'paired/{n}-r1.json' for n in ['baseline','screen','screen-off']]]
 for name in names:
  if (b/name).exists():data[name]=json.loads((b/name).read_text())
 for name in ['screen-kernel.log','screen-paths.log','screen-kernel-v2.log','screen-paths-v2.log','fallback-avx2.log','fallback-no-matrix.log','fallback-cold.log','fallback-row-codes.log','asan-screen-kernel.log','asan-screen-paths.log']:
  if (b/name).exists():data[name]=(b/name).read_text()
 for name in ['baseline','screen']:
  p=b/('profile-'+name)/'result.json'
  if p.exists():
   r=json.loads(p.read_text());data['profile-'+name]={'summary':r['summary'],'warmup':r['warmup'],'queries':len(r['queries'])}
 out[stage]=data
print(json.dumps(out,ensure_ascii=False,indent=2))
