from pathlib import Path
import hashlib,json,os,subprocess,time
b=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/composite-20261116/json1');src=b/'source';bench=src/'competition_lite/bench';assert json.loads((b/'complete.json').read_text())['passed']
env=dict(os.environ,PATH='/home/zzp/.local/bin:'+os.environ['PATH'],DOCKER_HOST='unix:///run/user/1002/docker-competition.sock');steps=[];image='lite:json-scan-20261116'
def run(name,cmd,timeout=600):
 start=time.monotonic()
 with (b/(name+'.log')).open('w') as log:p=subprocess.run(list(map(str,cmd)),env=env,cwd=src,stdout=log,stderr=subprocess.STDOUT,timeout=timeout)
 r={'name':name,'code':p.returncode,'seconds':time.monotonic()-start};steps.append(r);(b/'acceptance-progress.json').write_text(json.dumps(steps,indent=2)+'\n');print(json.dumps(r),flush=True)
 if p.returncode:print((b/(name+'.log')).read_text()[-4000:],flush=True);raise RuntimeError(name)
run('gate',['python3',bench/'docker_gate.py','--source',src,'--root',b/'gate','--image',image],1800)
c=subprocess.check_output(['docker','create',image],env=env,text=True).strip()
try:subprocess.run(['docker','cp',c+':/app/minivectordb',str(b/'bin/docker-runtime')],env=env,check=True)
finally:subprocess.run(['docker','rm',c],env=env,stdout=subprocess.DEVNULL,check=True)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
assert sha(b/'bin/docker-runtime')==sha(b/'bin/runtime'),'tested binary differs from Docker build'
d=['docker','run','--rm','--network=none','--cpus=2','--memory=2g','--memory-swap=2g','--mount',f'type=bind,src={b},dst=/work']
flags=['-O1','-g','-std=c++17','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie']
run('asan-build',d+['--entrypoint','g++',image,*flags,'/work/source/competition_lite/bench/json_input_check.cpp','-o','/work/bin/json-sanitized'],600)
run('asan-json',d+['-e','ASAN_OPTIONS=detect_leaks=1:abort_on_error=1','-e','UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1','--entrypoint','/work/bin/json-sanitized',image])
run('diagnostic-source',['python3',bench/'build_diagnostic.py','--source',src/'competition_lite/src/minivectordb_lite.cpp','--out',b/'diag/minivectordb_lite.cpp'])
run('diagnostic-build',d+['--entrypoint','g++',image,'-O3','-DNDEBUG','-std=c++17','/work/diag/minivectordb_lite.cpp','-o','/work/bin/diagnostic'])
out={'passed':True,'docker_runtime_sha256':sha(b/'bin/docker-runtime'),'runtime_sources':{p.name:sha(p) for p in (src/'competition_lite/src').iterdir() if p.is_file()}}
(b/'acceptance.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(out),flush=True)
prep=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/iteration-20261115-ce21bb9/prepared10k');old=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/distance-screen-20261116/dev4/bin/docker-runtime')
run('final-paired',['python3',bench/'paired_compare.py','--prepared',prep,'--oracle',prep.parent/'oracle10k.jsonl','--root',b/'final-paired','--image',image,'--variant','baseline='+str(old),'--variant','view='+str(b.parent/'view1/bin/docker-runtime'),'--variant','json='+str(b/'bin/docker-runtime'),'--rounds','3'])
