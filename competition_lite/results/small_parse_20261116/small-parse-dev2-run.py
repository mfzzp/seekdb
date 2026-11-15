from pathlib import Path
import hashlib,json,os,subprocess,tarfile,time
b=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/small-parse-20261116/dev2');b.mkdir(parents=True,exist_ok=False)
src=b/'source';src.mkdir();(b/'bin').mkdir()
with tarfile.open('/tmp/zzp-small-parse-dev2.tar.gz') as t:t.extractall(src,filter='data')
env=dict(os.environ,PATH='/home/zzp/.local/bin:'+os.environ['PATH'],DOCKER_HOST='unix:///run/user/1002/docker-competition.sock')
bench=src/'competition_lite/bench';steps=[];image='lite:small-parse-dev2-20261116';common='lite:small-hot-dev1-20261116'
def run(name,cmd,timeout=1200):
 start=time.monotonic()
 with (b/(name+'.log')).open('w') as f:p=subprocess.run(list(map(str,cmd)),env=env,cwd=src,stdout=f,stderr=subprocess.STDOUT,timeout=timeout)
 entry={'name':name,'code':p.returncode,'seconds':time.monotonic()-start};steps.append(entry);(b/'progress.json').write_text(json.dumps(steps,indent=2)+'\n');print(json.dumps(entry),flush=True)
 if p.returncode:print((b/(name+'.log')).read_text()[-5000:],flush=True);raise RuntimeError(name)
docker=['docker','run','--rm','--network=none','--cpus=2','--memory=2g','--memory-swap=2g','--mount',f'type=bind,src={b},dst=/work']
run('build-parser-check',docker+['--entrypoint','g++',common,'-O3','-DNDEBUG','-std=c++17','/work/source/competition_lite/bench/vector_parse_check.cpp','-o','/work/bin/parser-check'])
run('parser-check',[b/'bin/parser-check'])
run('gate',['python3',bench/'docker_gate.py','--source',src,'--root',b/'gate','--image',image],1800)
c=subprocess.check_output(['docker','create',image],env=env,text=True).strip()
try:subprocess.run(['docker','cp',c+':/app/minivectordb',str(b/'bin/runtime')],env=env,check=True)
finally:subprocess.run(['docker','rm',c],env=env,check=True,stdout=subprocess.DEVNULL)
baseline=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/small-hot-20261116/dev1/bin/runtime');original=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/readpath-20261115/release/bin/runtime')
prep=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/iteration-20261115-ce21bb9/prepared10k')
run('paired',['python3',bench/'paired_compare.py','--prepared',prep,'--oracle',prep.parent/'oracle10k.jsonl','--root',b/'paired','--image',common,'--variant','baseline-hot='+str(baseline),'--variant','fast-parse='+str(b/'bin/runtime'),'--variant','parse-off='+str(b/'bin/runtime'),'--variant-env','parse-off:LITE_FAST_PARSE=0','--variant','original9='+str(original),'--rounds','1'])
run('extra-oracle',['python3',bench/'hot_postings_oracle.py','--binary',b/'bin/runtime','--root',b/'extra-oracle'])
for label,binary in [('baseline-hot',baseline),('fast-parse',b/'bin/runtime')]:
 run('profile-'+label,['python3',bench/'profile_queries.py','--binary',binary,'--prepared',prep,'--root',b/('profile-'+label)])
run('diagnostic-source',['python3',bench/'build_diagnostic.py','--source',src/'competition_lite/src/minivectordb_lite.cpp','--out',b/'diag/minivectordb_lite.cpp'])
run('diagnostic-build',docker+['--entrypoint','g++',common,'-O3','-DNDEBUG','-std=c++17','/work/diag/minivectordb_lite.cpp','-o','/work/bin/diagnostic'])
for mode in ['0','1']:
 run('write-profile-'+mode,['python3',bench/'profile_commits.py','--binary',b/'bin/diagnostic','--prepared',prep,'--root',b/('write-profile-'+mode),'--image',common,'--engine-env','LITE_FAST_PARSE='+mode])
identity={'runtime_sha256':hashlib.sha256((b/'bin/runtime').read_bytes()).hexdigest(),'sources':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (src/'competition_lite/src').iterdir() if p.is_file()},'passed':True}
(b/'complete.json').write_text(json.dumps(identity,indent=2)+'\n');print(json.dumps(identity),flush=True)
