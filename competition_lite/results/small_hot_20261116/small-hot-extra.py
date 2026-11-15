from pathlib import Path
import json,os,shutil,subprocess,time
b=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/small-hot-20261116/dev1');assert json.loads((b/'complete.json').read_text())['passed']
bench=b/'source/competition_lite/bench'
for name in ['interleaved_exact.py','hot_postings_oracle.py']:shutil.copyfile('/tmp/zzp-'+name,bench/name)
env=dict(os.environ,PATH='/home/zzp/.local/bin:'+os.environ['PATH'],DOCKER_HOST='unix:///run/user/1002/docker-competition.sock');steps=[]
def run(name,cmd):
 start=time.monotonic()
 with (b/(name+'.log')).open('w') as f:p=subprocess.run(list(map(str,cmd)),env=env,stdout=f,stderr=subprocess.STDOUT,timeout=600)
 r={'name':name,'code':p.returncode,'seconds':time.monotonic()-start};steps.append(r);print(json.dumps(r),flush=True);(b/'extra.json').write_text(json.dumps(steps,indent=2)+'\n')
 if p.returncode:print((b/(name+'.log')).read_text()[-3000:]);raise RuntimeError(name)
prep=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/iteration-20261115-ce21bb9/prepared10k')
run('interleaved',['python3',bench/'interleaved_exact.py','--prepared',prep,'--oracle',prep.parent/'oracle10k.jsonl','--root',b/'interleaved','--image','lite:small-hot-dev1-20261116','--variant','baseline9=/mnt/SSD1/zzp/seekdb-round2-lite2000/readpath-20261115/release/bin/runtime','--variant','hot='+str(b/'bin/runtime')])
for label in ['baseline9','hot']:
 r=json.loads((b/'interleaved'/label/'verified.json').read_text());print(json.dumps({'label':label,'exact':r['verification']['exact']}),flush=True)
run('extra-oracle',['python3',bench/'hot_postings_oracle.py','--binary',b/'bin/runtime','--root',b/'extra-oracle'])
