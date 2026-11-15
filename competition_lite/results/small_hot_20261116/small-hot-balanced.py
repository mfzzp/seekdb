from pathlib import Path
import json,os,shutil,subprocess
b=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/small-hot-20261116/dev1');bench=b/'source/competition_lite/bench'
shutil.copyfile('/tmp/zzp-interleaved_exact.py',bench/'interleaved_exact.py')
prep=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/iteration-20261115-ce21bb9/prepared10k')
env=dict(os.environ,PATH='/home/zzp/.local/bin:'+os.environ['PATH'],DOCKER_HOST='unix:///run/user/1002/docker-competition.sock')
with (b/'balanced.log').open('w') as f:
 p=subprocess.run(['python3',str(bench/'interleaved_exact.py'),'--prepared',str(prep),'--oracle',str(prep.parent/'oracle10k.jsonl'),'--root',str(b/'balanced'),'--image','lite:small-hot-dev1-20261116','--variant','baseline9=/mnt/SSD1/zzp/seekdb-round2-lite2000/readpath-20261115/release/bin/runtime','--variant','hot='+str(b/'bin/runtime')],env=env,stdout=f,stderr=subprocess.STDOUT,timeout=300)
assert p.returncode==0
for label in ['baseline9','hot']:
 r=json.loads((b/'balanced'/label/'verified.json').read_text());print(json.dumps({'label':label,'exact':r['verification']['exact']}))
