from pathlib import Path
import json,os,subprocess,tarfile,time,shutil
root=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/perf-20261115/geo-dev01');root.mkdir(parents=True,exist_ok=False)
src=root/'source';src.mkdir()
with tarfile.open('/tmp/zzp-geo-experiment.tar.gz') as tar:tar.extractall(src,filter='data')
env=dict(os.environ,PATH='/home/zzp/.local/bin:'+os.environ['PATH'],DOCKER_HOST='unix:///run/user/1002/docker-competition.sock')
image='lite:exact-20261115';steps=[];bench=src/'competition_lite/bench'
def run(name,args,timeout=900):
 start=time.monotonic()
 with (root/(name+'.log')).open('w') as log:p=subprocess.run(list(map(str,args)),env=env,cwd=src,stdout=log,stderr=subprocess.STDOUT,timeout=timeout)
 item={'name':name,'returncode':p.returncode,'seconds':time.monotonic()-start};steps.append(item);(root/'experiments.json').write_text(json.dumps(steps,indent=2)+'\n');print(json.dumps(item),flush=True)
 if p.returncode:print((root/(name+'.log')).read_text()[-8000:],flush=True)
 assert p.returncode==0,item
# Do not overlap measurements/builds with the other active acceptance run.
release=root.parent/'linear-release'
waiting=time.monotonic()
while not (release/'release-metadata.json').exists():
 time.sleep(10)
 if time.monotonic()-waiting>2400:raise RuntimeError('acceptance not finished')
run('generate',['python3',bench/'geo_experiment.py','--source',src/'competition_lite/src/minivectordb_lite.cpp','--out',root/'generated'])
shutil.copyfile(root/'generated/minivectordb_lite.cpp',src/'competition_lite/src/minivectordb_lite.cpp')
docker=['docker','run','--rm','--cpus=2','--cpuset-cpus=0,1','--memory=2g','--memory-swap=2g','--network=none','--mount',f'type=bind,src={root},dst=/work']
for name,file in [('runtime','src/minivectordb_lite.cpp'),('bounds','bench/geo_bound_check.cpp'),('oracle','bench/exact_oracle.cpp')]:
 run('build-'+name,docker+['--entrypoint','g++',image,'-O3','-DNDEBUG','-std=c++17','-Wall','-Wextra','/work/source/competition_lite/'+file,'-o','/work/'+name])
run('bounds',docker+['--entrypoint','/work/bounds',image])
for minimum in [2048,1]:run('geo-paths-'+str(minimum),docker+['-e','LITE_GEO_MIN='+str(minimum),'--entrypoint','python3',image,'-B','/work/source/competition_lite/bench/geo_paths_check.py','--binary','/work/runtime','--root','/work/geo-paths-'+str(minimum)])
for isa in ['auto','avx2','scalar']:
 run('odd-paths-'+isa,docker+['-e','LITE_SIMD='+isa,'-e','LITE_EXACT_PATH=geo','-e','LITE_GEO_MIN=1','--entrypoint','python3',image,'-B','/work/source/competition_lite/bench/exact_kernel_paths_check.py','--binary','/work/runtime','--root','/work/odd-paths-'+isa])
old=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/iteration-20261115-ce21bb9')
inputs=[('uniform',old/'prepared10k',old/'oracle10k.jsonl')]
for dim in [128,3]:
 data=root/('cluster'+str(dim));run('prepare-'+str(dim),['python3',bench/'prepare_geometry.py','--template',old/'prepared10k','--out',data,'--dim',dim])
 run('oracle-'+str(dim),[root/'oracle',data/'data/vectors.f32',data/'prepared',10000,dim,data/'oracle.jsonl'])
 inputs.append(('cluster'+str(dim),data/'prepared',data/'oracle.jsonl'))
for kind,prepared,oracle in inputs:
 run('paired-'+kind,['python3',bench/'paired_compare.py','--prepared',prepared,'--oracle',oracle,'--root',root/('paired-'+kind),'--image',image,
 '--variant','frozen='+str(root.parent/'exact-release/bin/runtime'),'--variant','scan='+str(root/'runtime'),'--variant','block='+str(root/'runtime'),'--variant','geo='+str(root/'runtime'),
 '--variant-env','scan:LITE_EXACT_PATH=scan','--variant-env','block:LITE_EXACT_PATH=block','--variant-env','geo:LITE_EXACT_PATH=geo','--rounds','3'],1800)
 for mode in ['scan','block','geo']:
  name='profile-'+kind+'-'+mode
  run(name,docker+['--mount',f'type=bind,src={prepared},dst=/prepared,readonly','-e','LITE_EXACT_PATH='+mode,'--entrypoint','python3',image,'-B','/work/source/competition_lite/bench/profile_queries.py','--binary','/work/runtime','--prepared','/prepared','--root','/work/'+name])
flags=['-O1','-g','-std=c++17','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie']
run('sanitizer-build',docker+['--entrypoint','g++',image,*flags,'/work/source/competition_lite/src/minivectordb_lite.cpp','-o','/work/sanitized'])
run('sanitizer-paths',docker+['-e','LITE_GEO_MIN=1','-e','ASAN_OPTIONS=detect_leaks=1:abort_on_error=1','-e','UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1','--entrypoint','python3',image,'-B','/work/source/competition_lite/bench/geo_paths_check.py','--binary','/work/sanitized','--root','/work/sanitizer-paths'])
(root/'complete.json').write_text(json.dumps({'all_passed':True,'production_changed':False},indent=2)+'\n')
