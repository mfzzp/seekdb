from pathlib import Path
import json
b=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/composite-20261116/json1')
out={'server_root':str(b),'baseline_commit':'830fbba6bba3e0856e8bdeae964715a040395c5f','parent_commit':'10bc5751c139bc0b7620a5f7cc97bc3ec9950527'}
names=['complete.json','progress.json','paired/summary.json','final-paired/summary.json','acceptance.json','acceptance-progress.json','gate/gate.json','json-write-diagnostic.json']
names += [f'final-paired/{v}-r{r}.json' for v in ['baseline','view','json'] for r in [1,2,3]]
for name in names:
 if (b/name).exists():out[name]=json.loads((b/name).read_text())
for name in ['json-input.log','asan-json.log']:
 if (b/name).exists():out[name]=(b/name).read_text()
print(json.dumps(out,ensure_ascii=False,indent=2))
