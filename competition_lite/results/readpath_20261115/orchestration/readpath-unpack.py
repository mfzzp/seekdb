from pathlib import Path,PurePosixPath
import gzip,hashlib,json,tarfile
archive=Path(r'D:\oceanbase-competition\readpath-release-evidence.tar.gz')
out=Path(r'D:\oceanbase-competition\seekdb-round2-lite2000\exact-fix\competition_lite\results\readpath_20261115')
out.mkdir(parents=True,exist_ok=True)
with tarfile.open(archive) as t:
 for m in t:
  if not m.isfile():continue
  name=PurePosixPath(m.name);assert not name.is_absolute() and '..' not in name.parts,m.name
  raw=t.extractfile(m).read();target=out.joinpath(*name.parts)
  if m.name!='release-evidence-SHA256.json':target=target.with_name(target.name+'.gz');raw=gzip.compress(raw,mtime=0)
  target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(raw)
scripts=['readpath-phase1.py','readpath-phase1-finish.py','readpath-phase2.py','readpath-phase3.py','readpath-phase3b.py','readpath-map-probe.py','readpath-release.py','readpath-finish.py','readpath-verifier-update.py','readpath-collect.py','readpath-unpack.py']
dest=out/'orchestration';dest.mkdir(exist_ok=True);hashes={}
for name in scripts:
 p=Path(r'D:\oceanbase-competition')/name
 if p.exists():
  b=p.read_bytes();(dest/name).write_bytes(b);hashes[name]=hashlib.sha256(b).hexdigest()
(dest/'SHA256.json').write_bytes((json.dumps(hashes,indent=2)+'\n').encode('utf-8'))
(dest/'.gitattributes').write_bytes(b'* -text whitespace=blank-at-eol,blank-at-eof,space-before-tab,cr-at-eol\n')
print(json.dumps({'archive_sha256':hashlib.sha256(archive.read_bytes()).hexdigest(),'records':len(json.loads((out/'release-evidence-SHA256.json').read_bytes())),'orchestration_files':len(hashes)}))
