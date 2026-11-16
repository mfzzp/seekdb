from pathlib import Path
import json,os,subprocess,tarfile,time,hashlib
b=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/composite-20261116/view1');b.mkdir(parents=True,exist_ok=False);src=b/'source';src.mkdir();(b/'bin').mkdir()
with tarfile.open('/tmp/zzp-composite-view-dev1.tar.gz') as t:t.extractall(src,filter='data')
env=dict(os.environ,PATH='/home/zzp/.local/bin:'+os.environ['PATH'],DOCKER_HOST='unix:///run/user/1002/docker-competition.sock')
bench=src/'competition_lite/bench';image='lite:distance-screen-20261116';steps=[]
def run(name,cmd,timeout=600):
 start=time.monotonic()
 with (b/(name+'.log')).open('w') as log:p=subprocess.run(list(map(str,cmd)),env=env,cwd=src,stdout=log,stderr=subprocess.STDOUT,timeout=timeout)
 r={'name':name,'code':p.returncode,'seconds':time.monotonic()-start};steps.append(r);(b/'progress.json').write_text(json.dumps(steps,indent=2)+'\n');print(json.dumps(r),flush=True)
 if p.returncode:print((b/(name+'.log')).read_text()[-3000:],flush=True);raise RuntimeError(name)
d=['docker','run','--rm','--network=none','--cpus=2','--memory=2g','--memory-swap=2g','--mount',f'type=bind,src={b},dst=/work','--entrypoint','g++',image,'-O3','-DNDEBUG','-std=c++17']
run('build',d+['/work/source/competition_lite/src/minivectordb_lite.cpp','-o','/work/bin/runtime'])
run('build-screen',d+['/work/source/competition_lite/bench/screen_kernel_check.cpp','-o','/work/bin/screen-check'])
run('screen-kernel',[b/'bin/screen-check'])
run('matrix-view',['python3',bench/'matrix_view_check.py','--binary',b/'bin/runtime','--root',b/'matrix-view'])
run('screen-paths',['python3',bench/'screen_paths_check.py','--binary',b/'bin/runtime','--root',b/'screen-paths'])
prep=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/iteration-20261115-ce21bb9/prepared10k');old=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/distance-screen-20261116/dev4/bin/docker-runtime')
run('paired',['python3',bench/'paired_compare.py','--prepared',prep,'--oracle',prep.parent/'oracle10k.jsonl','--root',b/'paired','--image',image,'--variant','baseline='+str(old),'--variant','view='+str(b/'bin/runtime'),'--variant','copy='+str(b/'bin/runtime'),'--variant-env','copy:LITE_MATRIX_VIEW=0','--rounds','1'])
for label,binary in [('baseline',old),('view',b/'bin/runtime')]:run('profile-'+label,['python3',bench/'profile_queries.py','--binary',binary,'--prepared',prep,'--root',b/('profile-'+label)])
r={'passed':True,'sources':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (src/'competition_lite/src').iterdir() if p.is_file()},'binary_sha256':hashlib.sha256((b/'bin/runtime').read_bytes()).hexdigest()};(b/'complete.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r))

