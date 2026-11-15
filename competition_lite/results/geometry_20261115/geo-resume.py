from pathlib import Path
import json,os,subprocess,time
root=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/perf-20261115/geo-dev01');src=root/'source';bench=src/'competition_lite/bench'
env=dict(os.environ,PATH='/home/zzp/.local/bin:'+os.environ['PATH'],DOCKER_HOST='unix:///run/user/1002/docker-competition.sock')
image='lite:exact-20261115';steps=json.loads((root/'experiments.json').read_text())
def run(name,args,timeout=900):
 start=time.monotonic()
 with (root/(name+'.log')).open('w') as log:p=subprocess.run(list(map(str,args)),env=env,cwd=src,stdout=log,stderr=subprocess.STDOUT,timeout=timeout)
 item={'name':name,'returncode':p.returncode,'seconds':time.monotonic()-start};steps.append(item);(root/'experiments.json').write_text(json.dumps(steps,indent=2)+'\n');print(json.dumps(item),flush=True)
 if p.returncode:print((root/(name+'.log')).read_text()[-8000:],flush=True)
 assert p.returncode==0,item
docker=['docker','run','--rm','--cpus=2','--cpuset-cpus=0,1','--memory=2g','--memory-swap=2g','--network=none','--mount',f'type=bind,src={root},dst=/work']
prepared=root/'cluster3/prepared';oracle=root/'cluster3/oracle.jsonl'
run('paired-cluster3-retry',['python3',bench/'paired_compare.py','--prepared',prepared,'--oracle',oracle,'--root',root/'paired-cluster3-retry','--image',image,
 '--variant','frozen='+str(root.parent/'exact-release/bin/runtime'),'--variant','scan='+str(root/'runtime'),'--variant','block='+str(root/'runtime'),'--variant','geo='+str(root/'runtime'),
 '--variant-env','scan:LITE_EXACT_PATH=scan','--variant-env','block:LITE_EXACT_PATH=block','--variant-env','geo:LITE_EXACT_PATH=geo','--rounds','3'],1800)
for mode in ['scan','block','geo']:
 name='profile-cluster3-'+mode
 run(name,docker+['--mount',f'type=bind,src={prepared},dst=/prepared,readonly','-e','LITE_EXACT_PATH='+mode,'--entrypoint','python3',image,'-B','/work/source/competition_lite/bench/profile_queries.py','--binary','/work/runtime','--prepared','/prepared','--root','/work/'+name])
flags=['-O1','-g','-std=c++17','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie']
run('sanitizer-build',docker+['--entrypoint','g++',image,*flags,'/work/source/competition_lite/src/minivectordb_lite.cpp','-o','/work/sanitized'])
run('sanitizer-paths',docker+['-e','LITE_GEO_MIN=1','-e','ASAN_OPTIONS=detect_leaks=1:abort_on_error=1','-e','UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1','--entrypoint','python3',image,'-B','/work/source/competition_lite/bench/geo_paths_check.py','--binary','/work/sanitized','--root','/work/sanitizer-paths'])
(root/'complete.json').write_text(json.dumps({'all_required_checks_passed':True,'production_changed':False,'harness_fix':'schema-based warmup; retained failed baseline attempt'},indent=2)+'\n')
