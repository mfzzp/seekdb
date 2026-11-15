from pathlib import Path
import hashlib,json,os,subprocess,tarfile,time
base=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/readpath-20261115/phase3');base.mkdir(parents=True,exist_ok=False)
with tarfile.open('/tmp/zzp-readpath-phase3.tar.gz') as t:t.extractall(base,filter='data')
src=base/'source';env=dict(os.environ,PATH='/home/zzp/.local/bin:'+os.environ['PATH'],DOCKER_HOST='unix:///run/user/1002/docker-competition.sock');steps=[]
def run(name,args,timeout=600):
 start=time.monotonic()
 with (base/(name+'.log')).open('w') as log:p=subprocess.run(list(map(str,args)),cwd=src,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=timeout)
 step={'name':name,'code':p.returncode,'seconds':time.monotonic()-start};steps.append(step);(base/'progress.json').write_text(json.dumps(steps,indent=2)+'\n');print(json.dumps(step),flush=True)
 if p.returncode:print((base/(name+'.log')).read_text()[-5000:],flush=True);raise RuntimeError(name)
image='lite:linear-20261115';run('lines',['python3','competition_lite/tools/check_lines.py'])
docker=['docker','run','--rm','--network=none','--cpus=2','--memory=2g','--memory-swap=2g','--mount',f'type=bind,src={base},dst=/work','--entrypoint','g++',image]
for name,file,flags in [('engine','src/minivectordb_lite.cpp',[]),('cache','bench/read_cache_check.cpp',[]),('bulk','bench/bulk_read_check.cpp',[]),('view','bench/query_view_check.cpp',[]),('storage','bench/lsm_storage_check.cpp',['-Wl,--wrap=write','-Wl,--wrap=fsync','-Wl,--wrap=rename'])]:
 run('build-'+name,docker+['-O3','-DNDEBUG','-std=c++17','-Wall','-Wextra','/work/source/competition_lite/'+file,*flags,'-o','/work/'+name])
(base/'source-sha256.json').write_text(json.dumps({p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (src/'competition_lite/src').iterdir()},indent=2)+'\n')
run('cache-check',[base/'cache',base/'cache-data'])
run('bulk-check',[base/'bulk',base/'bulk-data'])
run('view-check',[base/'view',base/'view-data'])
for name,file,binary in [('model','lsm_model_check.py','engine'),('corruption','lsm_corruption_check.py','engine'),('storage','lsm_storage_check.py','storage')]:
 run(name,['python3',src/'competition_lite/bench'/file,'--binary',base/binary,'--root',base/('checks-'+name)])
run('exact',['python3',src/'competition_lite/bench/exact_paths_check.py','--binary',base/'engine','--root',base/'exact'])
(base/'completed.json').write_text(json.dumps({'passed':True,'steps':steps},indent=2)+'\n')

run('mixed100k-r1',['python3',src/'competition_lite/bench/mixed_compare.py','run','--binary',base/'engine','--image',image,'--prepared','/mnt/SSD1/zzp/seekdb-round2-lite2000/mixed-100k-300k-20261115/input-100000','--root',base/'mixed100k-r1','--label','100000-readpath-dev-r1'],1800)
