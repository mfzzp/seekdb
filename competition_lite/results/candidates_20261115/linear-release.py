from pathlib import Path
import hashlib
import json
import os
import subprocess
import tarfile
import time

base=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/perf-20261115/linear-release')
base.mkdir(parents=True,exist_ok=False);src=base/'source';src.mkdir();(base/'bin').mkdir()
with tarfile.open('/tmp/zzp-perf-git.tar.gz') as tar:tar.extractall(src,filter='data')
env=dict(os.environ,PATH='/home/zzp/.local/bin:'+os.environ['PATH'],DOCKER_HOST='unix:///run/user/1002/docker-competition.sock')
image='lite:linear-20261115';steps=[]
def run(name,args,timeout=900):
    start=time.monotonic()
    with (base/(name+'.log')).open('w') as log:p=subprocess.run(list(map(str,args)),cwd=src,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=timeout)
    item={'name':name,'returncode':p.returncode,'seconds':time.monotonic()-start};steps.append(item)
    (base/'experiments.json').write_text(json.dumps(steps,indent=2)+'\n');print(json.dumps(item),flush=True)
    if p.returncode:print((base/(name+'.log')).read_text()[-8000:],flush=True)
    assert p.returncode==0,item
bench=src/'competition_lite/bench'
run('gate',['python3',bench/'docker_gate.py','--source',src,'--root',base/'gate','--image',image])
container=subprocess.check_output(['docker','create',image],env=env,text=True).strip()
try:subprocess.run(['docker','cp',container+':/app/minivectordb',str(base/'bin/runtime')],env=env,check=True)
finally:subprocess.run(['docker','rm',container],env=env,check=True,stdout=subprocess.DEVNULL)
old=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/iteration-20261115-ce21bb9')
lsm=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/lsm-20261115-final')
run('paired',['python3',bench/'paired_compare.py','--prepared',old/'prepared10k','--oracle',old/'oracle10k.jsonl','--root',base/'paired','--image',image,
    '--variant','baseline-899='+str(lsm/'bin/lsm-release'),'--variant','old-f885='+str(old/'bin/old-f885c28'),'--variant','exact-9884='+str(base.parent/'exact-release/bin/runtime'),'--variant','exact='+str(base/'bin/runtime'),'--rounds','5'])
run('selectivity',['python3',bench/'paired_compare.py','--prepared',old/'prepared-selectivity','--oracle',old/'oracle-selectivity.jsonl','--root',base/'selectivity','--image',image,
    '--variant','exact-9884='+str(base.parent/'exact-release/bin/runtime'),'--variant','exact='+str(base/'bin/runtime'),'--rounds','5'])
docker=['docker','run','--rm','--cpus=2','--memory=2g','--memory-swap=2g','--network=none','--mount',f'type=bind,src={base},dst=/work']
for label in ['current','baseline']:
    binary=base/'bin/runtime' if label=='current' else base.parent/'exact-release/bin/runtime'
    run('query-profile-'+label,docker+['--mount',f'type=bind,src={old}/prepared10k,dst=/prepared,readonly','--mount',f'type=bind,src={binary},dst=/runtime,readonly','--entrypoint','python3',image,'-B','/work/source/competition_lite/bench/profile_queries.py','--binary','/runtime','--prepared','/prepared','--root','/work/query-profile-'+label])
for label,source in [('current',src/'competition_lite/src/minivectordb_lite.cpp'),('baseline',lsm/'release-source/competition_lite/src/minivectordb_lite.cpp')]:
    run('diagnostic-generate-'+label,['python3',bench/'build_diagnostic.py','--source',source,'--out',base/('diagnostic-'+label)/'diagnostic.cpp'])
    run('diagnostic-build-'+label,docker+['--entrypoint','g++',image,'-O3','-DNDEBUG','-std=c++17','/work/diagnostic-'+label+'/diagnostic.cpp','-o','/work/bin/diagnostic-'+label])
    run('profile-'+label,['python3',bench/'profile_commits.py','--binary',base/('bin/diagnostic-'+label),'--prepared',old/'prepared10k','--root',base/('profile-'+label),'--image',image])
run('capacity',['python3',bench/'lsm_capacity_check.py','--image',image,'--root',base/'capacity'],1200)
run('migration-state',['python3',bench/'lsm_model_check.py','--binary',base/'bin/runtime','--legacy',old/'bin/final-git','--root',base/'migration-state'])
run('migration-lsm3',['python3',bench/'lsm_model_check.py','--binary',base/'bin/runtime','--legacy',lsm/'bin/lsm-release','--root',base/'migration-lsm3'])
flags=['-O1','-g','-std=c++17','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie']
run('sanitizer-build',docker+['--entrypoint','g++',image,*flags,'/work/source/competition_lite/src/minivectordb_lite.cpp','-o','/work/bin/sanitized'])
checks=docker+['--read-only','-e','ASAN_OPTIONS=detect_leaks=1:abort_on_error=1','-e','UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1','--entrypoint','python3',image]
for label,tool in [('model','lsm_model_check.py'),('exact','exact_paths_check.py'),('kernel-paths','exact_kernel_paths_check.py')]:
    run('sanitizer-'+label,checks+['/work/source/competition_lite/bench/'+tool,'--binary','/work/bin/sanitized','--root','/work/sanitizer-'+label])
run('sanitizer-kernel-build',docker+['--entrypoint','g++',image,*flags,'/work/source/competition_lite/bench/l2_kernel_check.cpp','-o','/work/bin/kernel-sanitized'])
run('sanitizer-kernel',docker+['-e','ASAN_OPTIONS=detect_leaks=1:abort_on_error=1','-e','UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1','--entrypoint','/work/bin/kernel-sanitized',image])
run('sanitizer-wal-build',docker+['--entrypoint','g++',image,*flags,'/work/source/competition_lite/bench/wal_vector_check.cpp','-Wl,--wrap=write','-Wl,--wrap=pwrite','-Wl,--wrap=fdatasync','-o','/work/bin/wal-sanitized'])
run('sanitizer-wal',checks+['/work/source/competition_lite/bench/wal_vector_check.py','--binary','/work/bin/wal-sanitized','--root','/work/sanitizer-wal'])
meta={'image_id':json.loads(subprocess.check_output(['docker','image','inspect',image],env=env))[0]['Id'],
      'binary_sha256':hashlib.sha256((base/'bin/runtime').read_bytes()).hexdigest(),
      'runtime_sources':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (src/'competition_lite/src').glob('*')},
      'baseline_commit':'9884dd39428601937bc1b8e58df174631ec54237','required_checks_passed':True}
(base/'release-metadata.json').write_text(json.dumps(meta,indent=2)+'\n');print(json.dumps(meta),flush=True)
