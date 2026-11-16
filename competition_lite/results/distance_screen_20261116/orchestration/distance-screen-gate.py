from pathlib import Path
import hashlib,json,os,subprocess,time
b=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/distance-screen-20261116/dev4');src=b/'source';bench=src/'competition_lite/bench';assert json.loads((b/'complete.json').read_text())['passed']
env=dict(os.environ,PATH='/home/zzp/.local/bin:'+os.environ['PATH'],DOCKER_HOST='unix:///run/user/1002/docker-competition.sock');steps=[];image='lite:distance-screen-20261116'
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
assert sha(b/'bin/docker-runtime')==sha(b/'bin/runtime'),'tested binary differs from actual Docker build'
d=['docker','run','--rm','--network=none','--cpus=2','--memory=2g','--memory-swap=2g','--mount',f'type=bind,src={b},dst=/work']
for name,settings in [('avx2',['LITE_SIMD=avx2']),('no-matrix',['LITE_MATRIX_MB=0']),('cold',['LITE_ROW_CACHE_MB=0']),('row-codes',['LITE_SCREEN_DOT=0'])]:
 opts=[arg for item in settings for arg in ['-e',item]]
 run('fallback-'+name,d+opts+['--entrypoint','python3',image,'-B','/work/source/competition_lite/bench/screen_paths_check.py','--binary','/work/bin/docker-runtime','--root','/work/fallback-'+name,'--expect-screen','yes' if name=='row-codes' else 'no'])
flags=['-O1','-g','-std=c++17','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie']
run('asan-build',d+['--entrypoint','g++',image,*flags,'/work/source/competition_lite/src/minivectordb_lite.cpp','-o','/work/bin/sanitized'],600)
san=d+['-e','ASAN_OPTIONS=detect_leaks=1:abort_on_error=1','-e','UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1']
run('asan-screen-paths',san+['--entrypoint','python3',image,'-B','/work/source/competition_lite/bench/screen_paths_check.py','--binary','/work/bin/sanitized','--root','/work/asan-screen-paths'])
run('asan-kernel-build',d+['--entrypoint','g++',image,*flags,'/work/source/competition_lite/bench/screen_kernel_check.cpp','-o','/work/bin/screen-sanitized'],600)
run('asan-screen-kernel',san+['--entrypoint','/work/bin/screen-sanitized',image])
run('diagnostic-source',['python3',bench/'build_diagnostic.py','--source',src/'competition_lite/src/minivectordb_lite.cpp','--out',b/'diag/minivectordb_lite.cpp'])
run('diagnostic-build',d+['--entrypoint','g++',image,'-O3','-DNDEBUG','-std=c++17','/work/diag/minivectordb_lite.cpp','-o','/work/bin/diagnostic'])
out={'passed':True,'docker_runtime_sha256':sha(b/'bin/docker-runtime'),'runtime_sources':{p.name:sha(p) for p in (src/'competition_lite/src').iterdir() if p.is_file()}}
(b/'acceptance.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(out),flush=True)
