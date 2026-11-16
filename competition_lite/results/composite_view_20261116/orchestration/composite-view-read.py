from pathlib import Path
import json
b=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/composite-20261116/view1')
out={'server_root':str(b),'baseline_commit':'830fbba6bba3e0856e8bdeae964715a040395c5f'}
names=['complete.json','progress.json','paired/summary.json','final-paired/summary.json','acceptance.json','acceptance-progress.json','gate/gate.json','view-write-diagnostic.json']
names += [f'final-paired/{v}-r{r}.json' for v in ['baseline','view'] for r in [1,2,3]]
names += ['io-paired/summary.json']+[f'io-paired/{v}-r{r}.json' for v in ['baseline','view'] for r in [1,2,3]]
for name in names:
 if (b/name).exists():out[name]=json.loads((b/name).read_text())
for name in ['matrix-view.log','matrix-view-v2.log','screen-kernel.log','screen-paths.log','fallback-avx2.log','fallback-no-matrix.log','fallback-cold.log','fallback-row-codes.log','asan-screen-paths.log','asan-matrix-view.log']:
 if (b/name).exists():out[name]=(b/name).read_text()
for label in ['baseline','view']:
 p=b/('profile-'+label)/'result.json'
 if p.exists():
  r=json.loads(p.read_text());out['profile-'+label]={'summary':r['summary'],'warmup':r['warmup'],'queries':len(r['queries'])}
print(json.dumps(out,ensure_ascii=False,indent=2))
