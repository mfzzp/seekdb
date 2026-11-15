from pathlib import Path
import hashlib,json,os,statistics,subprocess,time
base=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/readpath-20261115/release')
src=base/'source';bench=src/'competition_lite/bench';image='lite:readpath-20261115';common='lite:linear-20261115'
env=dict(os.environ,PATH='/home/zzp/.local/bin:'+os.environ['PATH'],DOCKER_HOST='unix:///run/user/1002/docker-competition.sock')
deadline=time.monotonic()+1200
while not (base/'100k-decision.json').exists():
 if time.monotonic()>deadline:raise TimeoutError('10w decision not complete; no 30w started')
 time.sleep(3)
decision=json.loads((base/'100k-decision.json').read_text());assert decision['passed'],decision
assert not (base/'finish-progress.json').exists()
meta=json.loads((base/'runtime-identity.json').read_text());assert hashlib.sha256((base/'bin/runtime').read_bytes()).hexdigest()==meta['binary_sha256']
steps=[]
def run(name,args,timeout=1200):
 start=time.monotonic()
 with (base/(name+'.log')).open('w') as log:p=subprocess.run(list(map(str,args)),cwd=src,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=timeout)
 item={'name':name,'code':p.returncode,'seconds':time.monotonic()-start};steps.append(item);(base/'finish-progress.json').write_text(json.dumps(steps,indent=2)+'\n');print(json.dumps(item),flush=True)
 if p.returncode:print((base/(name+'.log')).read_text()[-6000:],flush=True);raise RuntimeError(name)
oldroot=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/mixed-100k-300k-20261115')
for repeat in range(1,4):
 name=f'300000-readpath-r{repeat}'
 run(name,['python3',bench/'mixed_compare.py','run','--binary',base/'bin/runtime','--image',common,'--prepared',oldroot/'input-300000','--root',base/'runs'/name,'--label',name],2400)
runs=[json.loads(p.read_text()) for p in sorted((base/'runs').glob('300000-*/result.json'))];assert len(runs)==3 and all(r['passed'] and r['restart_pass'] and r['verified_rows']==300000 for r in runs)
baseline=json.loads(Path('/tmp/zzp-readpath-baseline-summary.json').read_text())
med={k:statistics.median(r[k] for r in runs) for k in ['insert_wall_s','query_wall_s','primary_stage_sum_s']};med['peak_bytes']=statistics.median(r['resources']['after_shutdown']['memory_peak_bytes'] for r in runs)
checks={}
for label in ['scored-f885','cpp-449']:
 old=baseline['primary']['300000/'+label];checks[label]={k:med[k]<old[k]['median'] for k in ['insert_wall_s','query_wall_s','primary_stage_sum_s']};checks[label]['memory']=med['peak_bytes']<old['container_peak_bytes']['median']
passed=all(all(c.values()) for c in checks.values());(base/'300k-decision.json').write_text(json.dumps({'passed':passed,'medians':med,'checks':checks},indent=2)+'\n')
if not passed:raise RuntimeError('30w did not beat both frozen old versions; retain evidence')
docker=['docker','run','--rm','--network=none','--cpus=2','--memory=2g','--memory-swap=2g','--mount',f'type=bind,src={base},dst=/work']
run('diagnostic-generate',['python3',bench/'mixed_instrument.py','--source',src/'competition_lite/src/minivectordb_lite.cpp','--out',base/'diagnostic-source'])
run('diagnostic-build',docker+['--entrypoint','g++',common,'-O3','-DNDEBUG','-std=c++17','/work/diagnostic-source/minivectordb_lite.cpp','/work/diagnostic-source/probe.cpp','-Wl,--wrap=fsync','-Wl,--wrap=fdatasync','-o','/work/bin/diagnostic'])
configs=[('default',[]),('batch',['LITE_QUERY_VIEW_MB=0','LITE_LSM_READ=batch']),('scan',['LITE_QUERY_VIEW_MB=0','LITE_LSM_READ=scan']),('cache0',['LITE_QUERY_VIEW_MB=0','LITE_LSM_READ=batch','LITE_READ_CACHE_MB=0']),('cache1',['LITE_QUERY_VIEW_MB=0','LITE_LSM_READ=batch','LITE_READ_CACHE_MB=1']),('singlemap',['LITE_MMAP_KEEP_MB=0'])]
for rows in [100000,300000]:
 for label,settings in configs if rows==100000 else configs[:3]:
  name=f'diag-{rows}-{label}';args=['python3',bench/'mixed_stage_run.py','run','--binary',base/'bin/diagnostic','--image',common,'--prepared',oldroot/f'input-{rows}','--root',base/'diagnostics'/name,'--label',name,'--diagnostic','--engine-env','LITE_ROW_CACHE_MB=0']
  for setting in settings:args+=['--engine-env',setting]
  run(name,args,1800)
run('capacity',['python3',bench/'lsm_capacity_check.py','--image',image,'--root',base/'capacity'],1800)
old=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/iteration-20261115-ce21bb9');lsm=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/lsm-20261115-final')
run('migration-state',['python3',bench/'lsm_model_check.py','--binary',base/'bin/runtime','--legacy',old/'bin/final-git','--root',base/'migration-state'])
run('migration-lsm3',['python3',bench/'lsm_model_check.py','--binary',base/'bin/runtime','--legacy',lsm/'bin/lsm-release','--root',base/'migration-lsm3'])
flags=['-O1','-g','-std=c++17','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie']
for name in ['read_cache','bulk_read','query_view','vector_map']:
 run('asan-build-'+name,docker+['--entrypoint','g++',common,*flags,'/work/source/competition_lite/bench/'+name+'_check.cpp','-o','/work/bin/asan-'+name])
 run('asan-check-'+name,docker+['-e','ASAN_OPTIONS=detect_leaks=1:abort_on_error=1','-e','UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1','--entrypoint','/work/bin/asan-'+name,common,'/work/asan-check-'+name])
run('asan-build-runtime',docker+['--entrypoint','g++',common,*flags,'/work/source/competition_lite/src/minivectordb_lite.cpp','-o','/work/bin/asan-runtime'])
for name,script in [('model','lsm_model_check.py'),('exact','exact_paths_check.py')]:
 run('asan-'+name,docker+['-e','ASAN_OPTIONS=detect_leaks=1:abort_on_error=1','-e','UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1','--entrypoint','python3',common,'/work/source/competition_lite/bench/'+script,'--binary','/work/bin/asan-runtime','--root','/work/asan-'+name])
(base/'complete.json').write_text(json.dumps({'passed':True,'binary_sha256':meta['binary_sha256'],'source_sha256':meta['sources'],'same_frozen_binary_for_10w_and_30w':True,'official_submission_used':False},indent=2)+'\n')
