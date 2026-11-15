from pathlib import Path
import hashlib,json,time
b=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/readpath-20261115/release')
p=b/'source/competition_lite/bench/mixed_compare.py';old=p.read_bytes();new=Path('/tmp/zzp-readpath-mixed-verify-v2.py').read_bytes()
assert b.as_posix() in p.resolve().as_posix()
assert b'AND id<={min(begin+128,n)}' not in old and b'AND id<={min(begin+128,n)}' in new
(b/'verification-harness-v1.py').write_bytes(old)
(b/'verification-harness-v2.py').write_bytes(new)
p.write_bytes(new)
r={'time_unix':time.time(),'v1_sha256':hashlib.sha256(old).hexdigest(),'v2_sha256':hashlib.sha256(new).hexdigest(),'change':'Only post-timing full restore queries add a primary-key upper bound; every row, scalar and vector byte is still checked. Progress updates now cover both dimensions. No changes to insert, warmup, timed queries, binary, inputs, truth, limits or primary measurement. Existing running Python process keeps v1; subsequent invocations use v2.'}
(b/'verification-harness-revision.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r))
