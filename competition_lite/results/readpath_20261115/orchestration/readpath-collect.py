from pathlib import Path
import hashlib,json,tarfile
base=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/readpath-20261115')
assert json.loads((base/'release/complete.json').read_text())['passed']
selected=[]
for phase in ['phase3','phase3b','release']:
 root=base/phase
 for p in sorted(root.rglob('*')):
  if not p.is_file():continue
  rel=p.relative_to(root);parts=rel.parts
  if any(x in parts for x in ['data','bin','__pycache__']):continue
  if 'source' in parts:
   if not ('src' in parts or 'bench' in parts or p.name in ['Dockerfile','CMakeLists.txt']):continue
   if p.suffix not in ['.cpp','.hpp','.py'] and p.name!='Dockerfile':continue
  elif 'diagnostic-source' in parts:
   if p.suffix not in ['.cpp','.hpp','.json']:continue
  elif p.suffix not in ['.json','.log'] and p.name not in ['verification-harness-v1.py','verification-harness-v2.py']:continue
  selected.append(p)
manifest={p.relative_to(base).as_posix():{'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'bytes':p.stat().st_size} for p in selected}
(base/'release-evidence-SHA256.json').write_text(json.dumps(manifest,indent=2)+'\n')
with tarfile.open(base/'release-evidence.tar.gz','w:gz') as t:
 for p in selected:t.add(p,arcname=p.relative_to(base).as_posix())
 t.add(base/'release-evidence-SHA256.json',arcname='release-evidence-SHA256.json')
print(json.dumps({'files':len(selected),'uncompressed_bytes':sum(v['bytes'] for v in manifest.values()),'archive_bytes':(base/'release-evidence.tar.gz').stat().st_size}))
