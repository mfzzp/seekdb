from pathlib import Path
import json,os,subprocess,tarfile,time,hashlib
b=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/composite-20261116/json1');b.mkdir(parents=True,exist_ok=False);src=b/'source';src.mkdir();(b/'bin').mkdir()
with tarfile.open('/tmp/zzp-json-scan-dev1.tar.gz') as t:t.extractall(src,filter='data')
env=dict(os.environ,PATH='/home/zzp/.local/bin:'+os.environ['PATH'],DOCKER_HOST='unix:///run/user/1002/docker-competition.sock');bench=src/'competition_lite/bench';image='lite:composite-view-20261116';steps=[]
def run(name,cmd,timeout=600):
 start=time.monotonic()
 with (b/(name+'.log')).open('w') as log:p=subprocess.run(list(map(str,cmd)),env=env,cwd=src,stdout=log,stderr=subprocess.STDOUT,timeout=timeout)
 r={'name':name,'code':p.returncode,'seconds':time.monotonic()-start};steps.append(r);(b/'progress.json').write_text(json.dumps(steps,indent=2)+'\n');print(json.dumps(r),flush=True)
 if p.returncode:print((b/(name+'.log')).read_text()[-4000:],flush=True);raise RuntimeError(name)
d=['docker','run','--rm','--network=none','--cpus=2','--memory=2g','--memory-swap=2g','--mount',f'type=bind,src={b},dst=/work','--entrypoint','g++',image,'-O3','-DNDEBUG','-std=c++17']
run('build',d+['/work/source/competition_lite/src/minivectordb_lite.cpp','-o','/work/bin/runtime'])
run('build-json',d+['/work/source/competition_lite/bench/json_input_check.cpp','-o','/work/bin/json-check'])
run('json-input',[b/'bin/json-check'])
prep=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/iteration-20261115-ce21bb9/prepared10k');old=b.parent/'view1/bin/docker-runtime'
run('paired',['python3',bench/'paired_compare.py','--prepared',prep,'--oracle',prep.parent/'oracle10k.jsonl','--root',b/'paired','--image',image,'--variant','view='+str(old),'--variant','json='+str(b/'bin/runtime'),'--variant','json-off='+str(b/'bin/runtime'),'--variant-env','json-off:LITE_JSON_SCAN=0','--rounds','1'])
r={'passed':True,'sources':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (src/'competition_lite/src').iterdir() if p.is_file()},'binary_sha256':hashlib.sha256((b/'bin/runtime').read_bytes()).hexdigest()};(b/'complete.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r),flush=True)
print((b/'json-input.log').read_text(),flush=True);print((b/'paired/summary.json').read_text(),flush=True)
