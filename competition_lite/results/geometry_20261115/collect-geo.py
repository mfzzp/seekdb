from pathlib import Path
import difflib,gzip,hashlib,json,statistics,tarfile
base=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/perf-20261115/geo-dev01')
assert json.loads((base/'complete.json').read_text())['all_required_checks_passed']
out=base/'evidence';out.mkdir(exist_ok=False);raw={}
def write(rel,data):
 p=out/rel;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data)
for p in base.glob('*.log'):raw[p.name]=hashlib.sha256(p.read_bytes()).hexdigest();write(p.name+'.gz',gzip.compress(p.read_bytes(),mtime=0))
for name in ['experiments.json','complete.json']:write(name,(base/name).read_bytes())
write('geo-input.json',(base/'generated/geo-input.json').read_bytes())
ref=(base.parent/'linear-release/source/competition_lite/src/minivectordb_lite.cpp').read_text();runtime=(base/'generated/minivectordb_lite.cpp').read_text()
diff=''.join(difflib.unified_diff(ref.splitlines(True),runtime.splitlines(True),fromfile='5d91391/minivectordb_lite.cpp',tofile='geo-experiment/minivectordb_lite.cpp')).encode();raw['runtime.diff']=hashlib.sha256(diff).hexdigest();write('runtime.diff.gz',gzip.compress(diff,mtime=0))
summary={}
for kind in ['uniform','cluster128','cluster3','cluster3-retry']:
 directory=base/('paired-'+kind)
 for p in directory.glob('*.json'):write(Path('paired-'+kind)/p.name,p.read_bytes())
 for p in directory.glob('*.log'):raw['paired-'+kind+'/'+p.name]=hashlib.sha256(p.read_bytes()).hexdigest();write(Path('paired-'+kind)/(p.name+'.gz'),gzip.compress(p.read_bytes(),mtime=0))
 rows=[]
 for p in (directory/'raw').glob('*/result.json'):
  r=json.loads(p.read_text());rows.extend(json.dumps({'run':r['label'],**q},separators=(',',':'))+'\n' for q in r['queries'])
 if rows:write('paired-'+kind+'-queries.jsonl.gz',gzip.compress(''.join(rows).encode(),mtime=0))
 if (directory/'summary.json').exists():
  summary[kind]={}
  for mode,s in json.loads((directory/'summary.json').read_text())['summary_medians'].items():
   runs=[json.loads(p.read_text()) for p in directory.glob(mode+'-r[123].json')];assert len(runs)==3
   assert all(r['restart_pass'] and r['verification']['exact']['passed'] for r in runs)
   summary[kind][mode]={'mean':s['exact_mean_ms'],'groups':s['filters_mean_ms'],'warmup':s['warmup_s'],'workflow':statistics.median(sum(r[k] for k in ['insert_wall_s','index_s','warmup_s','query_wall_s','checkpoint_s']) for r in runs),'peak':s['peak_bytes']}
profiles={}
for p in base.glob('profile-*/result.json'):
 r=json.loads(p.read_text());rows=r.pop('queries');write(Path(p.parent.name)/'result.json',(json.dumps(r,indent=2)+'\n').encode());write(Path(p.parent.name)/'queries.jsonl.gz',gzip.compress(''.join(json.dumps(q,separators=(',',':'))+'\n' for q in rows).encode(),mtime=0))
 profiles[p.parent.name]={'summary':{k:{name:v[name] for name in ['block_build','bounds','block_sort','prepare','vector_distance','full_distance_evaluations_mean','blocks_pruned_mean']} for k,v in r['summary'].items()},'first_build_ms':r['warmup'][0]['profile_ms']['block_build']}
for name in ['cluster3','cluster128']:
 write(Path(name)/'prepared.json',(base/name/'prepared/prepared.json').read_bytes())
 write(Path(name)/'oracle.jsonl.gz',gzip.compress((base/name/'oracle.jsonl').read_bytes(),mtime=0))
 for file in ['data/vectors.f32','data/rows.jsonl','data/load_order.u32','prepared/insert.jsonl','prepared/cases.json']:
  p=base/name/file;write(Path(name)/(file.replace('/','-')+'.sha256'),(hashlib.sha256(p.read_bytes()).hexdigest()+'\n').encode())
names=['geo_experiment.py','geo_bound_check.cpp','geo_paths_check.py','prepare_geometry.py','compare_run.py','profile_queries.py','exact_paths_check.py','exact_kernel_paths_check.py','paired_compare.py','exact_oracle.cpp']
write('bench-source-sha256.json',(json.dumps({name:hashlib.sha256((base/'source/competition_lite/bench'/name).read_bytes()).hexdigest() for name in names},indent=2)+'\n').encode())
write('summary.json',(json.dumps({'medians':summary,'profiles':profiles},indent=2)+'\n').encode())
write('raw-log-sha256.json',(json.dumps(raw,indent=2)+'\n').encode())
write('SHA256.json',(json.dumps({p.relative_to(out).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in out.rglob('*') if p.is_file()},indent=2)+'\n').encode())
with tarfile.open(base/'evidence.tar.gz','w:gz') as t:t.add(out,arcname='geometry_20261115')
print(json.dumps({'archive':str(base/'evidence.tar.gz'),'bytes':(base/'evidence.tar.gz').stat().st_size,'medians':summary,'profiles':profiles}))
