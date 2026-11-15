from pathlib import Path
import json,subprocess,time
b=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/readpath-20261115/phase1');steps=[]
for name,script,binary in [('storage','lsm_storage_check.py','storage'),('exact','exact_paths_check.py','engine')]:
 start=time.monotonic()
 with (b/(name+'-retry.log')).open('w') as log:p=subprocess.run(['python3',str(b/'source/competition_lite/bench'/script),'--binary',str(b/binary),'--root',str(b/('checks-'+name))],stdout=log,stderr=subprocess.STDOUT,timeout=600)
 r={'name':name,'code':p.returncode,'seconds':time.monotonic()-start};steps.append(r);print(json.dumps(r),flush=True)
 if p.returncode:print((b/(name+'-retry.log')).read_text()[-4000:]);raise RuntimeError(name)
(b/'completed.json').write_text(json.dumps({'passed':True,'original_harness_error':'binary and result directory both named storage; changed only result path','retry_steps':steps},indent=2)+'\n')
