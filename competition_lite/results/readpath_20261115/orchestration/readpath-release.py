from pathlib import Path
import hashlib,json,os,statistics,subprocess,tarfile,time
base=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/readpath-20261115/release');assert not (base/'progress.json').exists();base.mkdir(parents=True,exist_ok=True);src=base/'source';src.mkdir(exist_ok=True);(base/'bin').mkdir(exist_ok=True)
(base/'preflight-retry.json').write_text(json.dumps({'first_attempt':'source hash mismatch from Windows git archive CRLF conversion, before build or test','fix':'git -c core.autocrlf=false archive; bytes verified against frozen Git blobs'})+'\n')
with tarfile.open('/tmp/zzp-readpath-release-git.tar.gz') as t:t.extractall(src,filter='data')
meta=json.loads(Path('/tmp/zzp-readpath-git-inputs.json').read_text());baseline=json.loads(Path('/tmp/zzp-readpath-baseline-summary.json').read_text());(base/'git-inputs.json').write_text(json.dumps(meta,indent=2)+'\n')
for name,digest in meta['sources'].items():assert hashlib.sha256((src/'competition_lite/src'/name).read_bytes()).hexdigest()==digest
env=dict(os.environ,PATH='/home/zzp/.local/bin:'+os.environ['PATH'],DOCKER_HOST='unix:///run/user/1002/docker-competition.sock');steps=[];bench=src/'competition_lite/bench';image='lite:readpath-20261115';common='lite:linear-20261115'
def run(name,args,timeout=1200):
 start=time.monotonic()
 with (base/(name+'.log')).open('w') as log:p=subprocess.run(list(map(str,args)),cwd=src,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=timeout)
 item={'name':name,'code':p.returncode,'seconds':time.monotonic()-start};steps.append(item);(base/'progress.json').write_text(json.dumps(steps,indent=2)+'\n');print(json.dumps(item),flush=True)
 if p.returncode:print((base/(name+'.log')).read_text()[-6000:],flush=True);raise RuntimeError(name)
# The development run must finish before any official local timing starts.
dev=base.parent/'phase3b/mixed100k-r1/result.json';deadline=time.monotonic()+600
while not dev.exists():
 if time.monotonic()>deadline:raise TimeoutError('development run still incomplete')
 time.sleep(2)
assert json.loads(dev.read_text())['passed']
run('gate',['python3',bench/'docker_gate.py','--source',src,'--root',base/'gate','--image',image],1800)
container=subprocess.check_output(['docker','create',image],env=env,text=True).strip()
try:subprocess.run(['docker','cp',container+':/app/minivectordb',str(base/'bin/runtime')],env=env,check=True)
finally:subprocess.run(['docker','rm',container],env=env,check=True,stdout=subprocess.DEVNULL)
meta['binary_sha256']=hashlib.sha256((base/'bin/runtime').read_bytes()).hexdigest();meta['image_id']=json.loads(subprocess.check_output(['docker','image','inspect',image],env=env))[0]['Id'];(base/'runtime-identity.json').write_text(json.dumps(meta,indent=2)+'\n')
docker=['docker','run','--rm','--network=none','--cpus=2','--memory=2g','--memory-swap=2g','--mount',f'type=bind,src={base},dst=/work','--entrypoint','g++',common]
for name in ['read_cache','bulk_read','query_view','vector_map']:
 run('build-'+name,docker+['-O3','-DNDEBUG','-std=c++17','-Wall','-Wextra','/work/source/competition_lite/bench/'+name+'_check.cpp','-o','/work/bin/'+name])
 run('check-'+name,[base/'bin'/name,base/('check-'+name)])
old=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/iteration-20261115-ce21bb9')
old_current=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/mixed-100k-300k-20261115/bin/new-24')
run('small-paired',['python3',bench/'paired_compare.py','--prepared',old/'prepared10k','--oracle',old/'oracle10k.jsonl','--root',base/'small-paired','--image',common,'--variant','baseline24='+str(old_current),'--variant','readpath='+str(base/'bin/runtime'),'--rounds','3'])
small=json.loads((base/'small-paired/summary.json').read_text())['summary_medians'];(base/'small-decision.json').write_text(json.dumps({'query_time_ratio':small['readpath']['query_wall_s']/small['baseline24']['query_wall_s'],'memory_ratio':small['readpath']['peak_bytes']/small['baseline24']['peak_bytes'],'write_time_ratio':small['readpath']['insert_wall_s']/small['baseline24']['insert_wall_s']},indent=2)+'\n')
if small['readpath']['query_wall_s']>small['baseline24']['query_wall_s']*1.05:raise RuntimeError('small workload query regression; stop before stress promotion')
for repeat in range(1,4):
 name=f'100000-readpath-r{repeat}'
 run(name,['python3',bench/'mixed_compare.py','run','--binary',base/'bin/runtime','--image',common,'--prepared','/mnt/SSD1/zzp/seekdb-round2-lite2000/mixed-100k-300k-20261115/input-100000','--root',base/'runs'/name,'--label',name],1800)
runs=[json.loads(p.read_text()) for p in sorted((base/'runs').glob('100000-*/result.json'))];assert len(runs)==3 and all(r['passed'] and r['restart_pass'] and r['verified_rows']==100000 for r in runs)
medians={k:statistics.median(r[k] for r in runs) for k in ['insert_wall_s','query_wall_s','primary_stage_sum_s']};medians['peak_bytes']=statistics.median(r['resources']['after_shutdown']['memory_peak_bytes'] for r in runs)
checks={}
for label in ['scored-f885','cpp-449']:
 old=baseline['primary']['100000/'+label];checks[label]={k:medians[k]<old[k]['median'] for k in ['insert_wall_s','query_wall_s','primary_stage_sum_s']};checks[label]['memory']=medians['peak_bytes']<old['container_peak_bytes']['median']
passed=all(all(c.values()) for c in checks.values());result={'passed':passed,'medians':medians,'checks':checks,'comparison':'frozen historical three-round baselines from 312f7e0, byte-identical inputs and same machine/compiler/limits','next':'30w allowed only if passed'};(base/'100k-decision.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result),flush=True)
if not passed:raise RuntimeError('100k did not beat both frozen old versions; 300k blocked')
