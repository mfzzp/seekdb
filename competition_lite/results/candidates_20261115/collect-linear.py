from pathlib import Path
import difflib,gzip,hashlib,json,shutil,statistics,tarfile
base=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/perf-20261115/linear-release')
assert json.loads((base/'release-metadata.json').read_text())['required_checks_passed']
out=base/'evidence';out.mkdir(exist_ok=False);raw_hashes={}
reference=(base.parent/'exact-release/source/competition_lite/src/minivectordb_lite.cpp').read_text()
def write(rel,data):
 p=out/rel;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data)
def copy(path,rel):
 data=path.read_bytes()
 if path.suffix=='.log':raw_hashes[str(rel)]=hashlib.sha256(data).hexdigest();write(str(rel)+'.gz',gzip.compress(data,mtime=0))
 else:write(rel,data)
def paired(path,rel):
 if not path.exists():return
 for p in path.glob('*.json'):copy(p,rel/p.name)
 rows=[]
 for p in sorted((path/'raw').glob('*/result.json')):
  r=json.loads(p.read_text());rows.extend(json.dumps({'run':r['label'],**q},separators=(',',':'))+'\n' for q in r['queries'])
 write(str(rel)+'-queries.jsonl.gz',gzip.compress(''.join(rows).encode(),mtime=0))
for directory in [base,*[base.parent/x for x in ['candidate-dev01','candidate-dev02','candidate-dev03','candidate-release']]]:
 prefix=Path(directory.name)
 for p in directory.glob('*.log'):copy(p,prefix/p.name)
 for name in ['experiments.json','release-metadata.json']:
  if (directory/name).exists():copy(directory/name,prefix/name)
 runtime=directory/'source/competition_lite/src/minivectordb_lite.cpp'
 write(prefix/'runtime.diff',''.join(difflib.unified_diff(reference.splitlines(True),runtime.read_text().splitlines(True),fromfile='9884dd3/minivectordb_lite.cpp',tofile=directory.name+'/minivectordb_lite.cpp')).encode())
 write(prefix/'runtime-sha256.json',(json.dumps({p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in runtime.parent.glob('*')},indent=2)+'\n').encode())
 for name in ['paired','selectivity','paired-10k','paired-selectivity']:paired(directory/name,prefix/name)
 for p in [*directory.glob('profile-*/result.json'),*directory.glob('query-profile-*/result.json')]:
  data=json.loads(p.read_text());rows=data.pop('queries',None)
  write(prefix/p.parent.name/'result.json',(json.dumps(data,indent=2)+'\n').encode())
  if rows is not None:write(prefix/p.parent.name/'queries.jsonl.gz',gzip.compress(''.join(json.dumps(r,separators=(',',':'))+'\n' for r in rows).encode(),mtime=0))
 if (directory/'gate').exists():
  for p in (directory/'gate').glob('*.log'):copy(p,prefix/'gate'/p.name)
  for p in (directory/'gate').rglob('*.json'):copy(p,prefix/'gate'/p.relative_to(directory/'gate'))
 for name in ['capacity','migration-state','migration-lsm3','sanitizer-model','sanitizer-wal']:
  p=directory/name/'result.json'
  if p.exists():copy(p,prefix/name/'result.json')
for label in ['current','baseline']:
 copy(base/('diagnostic-'+label)/'diagnostic.json',Path('linear-release')/('diagnostic-'+label+'.json'))
 for mode in ['auto','explicit']:
  p=base/('profile-'+label)/mode/'trace.jsonl';write(Path('linear-release')/('trace-'+label+'-'+mode+'.jsonl.gz'),gzip.compress(p.read_bytes(),mtime=0))
summary=json.loads((base/'paired/summary.json').read_text())['summary_medians'];six={}
for variant in summary:
 runs=[json.loads(p.read_text()) for p in (base/'paired').glob(variant+'-r[12345].json')];assert len(runs)==5
 assert all(r['restart_pass'] and r['verification']['exact']['passed'] for r in runs)
 d={'measurements':summary[variant],'request_qps':1000/statistics.median(r['verification']['exact']['mean_ms'] for r in runs),'query_interval_qps':1000/statistics.median(r['query_wall_s'] for r in runs),'write_rows_per_s':10000/statistics.median(r['insert_wall_s'] for r in runs),
 'measured_stage_sum_s':statistics.median(sum(r[k] for k in ['insert_wall_s','index_s','warmup_s','query_wall_s','checkpoint_s']) for r in runs),'query_cpu_s':statistics.median(r['resources']['query_end']['cpu_s']-r['resources']['query_prepared']['cpu_s'] for r in runs),'insert_cpu_s':statistics.median(r['insert_cpu_s'] for r in runs),'query_end_peak_rss_kib':statistics.median(r['resources']['query_end']['process_diagnostics_not_official_B']['peak_rss_kib'] for r in runs),'storage_io_available':all(r['resources']['query_end']['io'] is not None for r in runs),'process_io_diagnostics':{}}
 for stage,lo,hi in [('write','write_start','write_end'),('query','query_prepared','query_end')]:d['process_io_diagnostics'][stage]={key:statistics.median(r['resources'][hi]['process_diagnostics_not_official_B'][key]-r['resources'][lo]['process_diagnostics_not_official_B'][key] for r in runs) for key in ['read_bytes','write_bytes','rchar','wchar','cancelled_write_bytes']}
 six[variant]=d
for label in ['current','baseline']:six['write_diagnostic_'+label]=json.loads((base/('profile-'+label)/'result.json').read_text())
write('six-metrics.json',(json.dumps(six,indent=2)+'\n').encode())
write('raw-log-sha256.json',(json.dumps(raw_hashes,indent=2)+'\n').encode())
write('SHA256.json',(json.dumps({str(p.relative_to(out)).replace('\\','/'):hashlib.sha256(p.read_bytes()).hexdigest() for p in out.rglob('*') if p.is_file()},indent=2)+'\n').encode())
with tarfile.open(base/'evidence.tar.gz','w:gz') as tar:tar.add(out,arcname='candidates_20261115')
print(json.dumps({'archive':str(base/'evidence.tar.gz'),'bytes':(base/'evidence.tar.gz').stat().st_size,'measured_stage_sum':{k:v['measured_stage_sum_s'] for k,v in six.items() if not k.startswith('write_')}}))
