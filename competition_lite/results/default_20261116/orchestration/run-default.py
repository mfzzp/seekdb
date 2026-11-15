"""Repeat the frozen default workload; no production source or database changes."""
from pathlib import Path
import hashlib,json,os,shutil,subprocess,time

b=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000')
root=b/'default-10000-20261116'
root.mkdir(exist_ok=False)
release=b/'readpath-20261115/release'
old=b/'mixed-100k-300k-20261115'
prepared=b/'iteration-20261115-ce21bb9/prepared10k'
oracle=prepared.parent/'oracle10k.jsonl'
bench=release/'source/competition_lite/bench'
env=dict(os.environ,PATH='/home/zzp/.local/bin:'+os.environ['PATH'],DOCKER_HOST='unix:///run/user/1002/docker-competition.sock')
variants={
 'current-9a911a1':(release/'bin/runtime','9a911a1ea7bfe8754047ce8313483b30728e9b63','86d8a27867fd8583e9d5f8e9bdcb6169e9d18a809fc5bf551f943f494a0cc1e2'),
 'scored-f885c28':(old/'bin/scored-f885','f885c285606e9c0b62806d92f29b96977e2badf8','4485f38cd7869e62b1cde92330972b3deddbf996005cd941a6907144673e39b5'),
 'cpp-449bba1':(old/'bin/cpp-449','449bba12112755a1a3061a422b383587a5bb468a','84d2ccf317d65dc66f149a9b42a48697214024f8d53fa59f24294d2a9d1e3ae3'),
 'previous-24d4262':(old/'bin/new-24','24d426257dfbe4f88387cb547dd0987c2aa4ffc5','c72cf54105b2b6963a51263c5db193056780f29bc447a6c8cc40af6b2cf2d428'),
}
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
for label,(binary,commit,digest) in variants.items():assert sha(binary)==digest,label
meta=json.loads((prepared/'prepared.json').read_text())
cases=json.loads((prepared/'cases.json').read_text())
assert (meta['rows'],meta['dim'],meta['batch'],meta['requests'])==(10000,128,100,100)
assert meta['modes']==['exact'] and len(cases)==1000
assert {f:sum(c['filter']==f for c in cases) for f in ['none','category']}=={'none':500,'category':500}
assert sha(prepared/'insert.jsonl')==meta['insert_sha256']
assert sha(prepared/'cases.json')==meta['query_cases_sha256']
identity={'variants':{label:{'commit':c,'binary':str(p),'sha256':h} for label,(p,c,h) in variants.items()},
 'prepared':meta,'oracle_sha256':sha(oracle),'rounds':5,'filters':{'none':500,'category':500},
 'limits':{'cpus':2,'engine_affinity':[0,1],'memory_bytes':2<<30,'swap':0},
 'common_image':json.loads(subprocess.check_output(['docker','image','inspect','lite:linear-20261115'],env=env))[0]['Id'],
 'bench_sha256':{name:sha(bench/name) for name in ['paired_compare.py','compare_run.py','exact_oracle.cpp','write_probe.py']},
 'input_hashes':{p.name:sha(p) for p in prepared.iterdir() if p.is_file()},
 'started_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),
 'method':'Fresh database/container per run, rotating variant order, fixed inputs and 18 distinct warmups; independent oracle and full restart validation per run; timings are uninstrumented.'}
(root/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
(root/'source').mkdir()
for name in identity['bench_sha256']:shutil.copyfile(bench/name,root/'source'/name)
shutil.copyfile(prepared/'prepared.json',root/'prepared.json')
shutil.copyfile(oracle,root/'oracle10k.jsonl')
cmd=['python3',str(bench/'paired_compare.py'),'--prepared',str(prepared),'--oracle',str(oracle),'--root',str(root/'paired'),'--image','lite:linear-20261115','--rounds','5']
for label,(binary,_,_) in variants.items():cmd+=['--variant',label+'='+str(binary)]
with (root/'paired.log').open('w') as log:
 p=subprocess.run(cmd,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=1800)
(root/'complete.json').write_text(json.dumps({'passed':p.returncode==0,'returncode':p.returncode,'finished_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())})+'\n')
print((root/'paired.log').read_text()[-1500:],flush=True)
assert p.returncode==0
