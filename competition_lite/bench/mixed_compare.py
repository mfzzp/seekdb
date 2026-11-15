#!/usr/bin/env python3
"""Three frozen entrypoints, two tables in one process, independent exact truth.

All resource limits and affinity reuse the existing common container client.
Failures retain phase, stderr and cgroup evidence and never count as speedups.
"""
import argparse,hashlib,json,math,mmap,os,random,select,statistics,struct,subprocess,time,traceback
from pathlib import Path
from compare_run import Client,LIVE_CLIENTS,literal,sha,wire

DIMS=[128,1024]
PHASES=['startup','schema','insert128','insert1024','index','warmup','query','shutdown','restart','other']
KINDS=['vector','wal_journal','sst','manifest','directory','other']

class MixedClient(Client):
 def __init__(self,*args,**kwargs):self.rx=bytearray();super().__init__(*args,**kwargs);os.set_blocking(self.p.stdin.fileno(),False)
 def read(self,timeout=60):
  end=time.monotonic()+timeout
  while b'\n' not in self.rx:
   left=end-time.monotonic()
   if left<=0 or not select.select([self.p.stdout],[],[],left)[0]:raise TimeoutError('complete response deadline')
   block=os.read(self.p.stdout.fileno(),65536)
   if not block:raise RuntimeError('engine exited before complete response')
   self.rx.extend(block)
   if len(self.rx)>16777216:raise RuntimeError('response exceeds public 16 MiB limit')
  line,_,rest=self.rx.partition(b'\n');self.rx=bytearray(rest);return json.loads(line)
 def send(self,request):
  end=time.monotonic()+60;view=memoryview(request)
  while view:
   left=end-time.monotonic()
   if left<=0 or not select.select([],[self.p.stdin],[],left)[1]:raise TimeoutError('request write deadline')
   try:n=os.write(self.p.stdin.fileno(),view)
   except BlockingIOError:continue
   view=view[n:]
  response=self.read(max(.001,end-time.monotonic()));expected=json.loads(request[:request.index(b',"op"')]+b'}')['id']
  if response.get('id')!=expected:raise RuntimeError(('response id mismatch',expected,response))
  if not response.get('ok'):raise RuntimeError(response)
  return response
 def close(self):
  start=time.perf_counter();self.send(b'{"id":99999999,"op":"shutdown"}\n');self.p.stdin.close();self.p.wait(timeout=120)
  if self.p.returncode:raise RuntimeError(('shutdown exit',self.p.returncode))
  if self.rx or self.p.stdout.read():raise RuntimeError('extra output after shutdown')
  self.close_s=time.perf_counter()-start;final=self.metrics();self.abort();return final

def sql(case):
 where='' if case['filter']=='none' else " WHERE category='cat%d'"%case['category']
 return "SELECT id,L2_DISTANCE(embedding,'%s') AS distance FROM docs%d%s ORDER BY distance LIMIT 10;"%(literal(case['vector']),case['dim'],where)

def prepare(a):
 root=a.root;assert a.rows%200==0 and a.rows>0
 if (root/'prepared.json').exists():raise FileExistsError(root/'prepared.json')
 root.mkdir(parents=True,exist_ok=True);n=a.rows//2
 for d in DIMS:assert (root/f'vectors{d}.f32').stat().st_size==n*d*4
 rng={d:random.Random(928313+d) for d in DIMS};warm_rng=random.Random(733311)
 cases=[];warm=[]
 for i in range(a.queries):
  d=DIMS[i%2];v=struct.unpack(f'<{d}f',struct.pack(f'<{d}f',*[rng[d].gauss(0,.5) for _ in range(d)]))
  cases.append({'id':i,'dim':d,'vector':v,'filter':'none' if i%4<2 else 'category','category':(i//4)%20})
 for i in range(18):
  d=DIMS[i%2];v=struct.unpack(f'<{d}f',struct.pack(f'<{d}f',*[warm_rng.gauss(0,.5) for _ in range(d)]))
  warm.append({'id':100000+i,'dim':d,'vector':v,'filter':'none' if i%4<2 else 'category','category':i%20})
 (root/'cases.json').write_text(json.dumps(cases)+'\n');(root/'warmup.json').write_text(json.dumps(warm)+'\n')
 with (root/'queries.jsonl').open('wb') as f:
  for c in cases:f.write(wire(sql(c),200000+c['id']))
 for d in DIMS:
  p=root/str(d);p.mkdir(exist_ok=False)
  with (p/'queries.f32').open('wb') as qf,(p/'cases.tsv').open('w') as cf:
   for c in cases:
    if c['dim']!=d:continue
    qf.write(struct.pack(f'<{d}f',*c['vector']));cf.write(f'{c["id"]} {0 if c["filter"]=="none" else c["category"]+1} 10\n')
  (p/'masks.u8').write_bytes(bytes([1])*n+b''.join(bytes(int(i%20==cat) for i in range(1,n+1)) for cat in range(20)))
 files=['insert.jsonl','queries.jsonl','cases.json','warmup.json',*[f'vectors{d}.f32' for d in DIMS]]
 meta={'rows':a.rows,'rows_per_dimension':n,'dims':DIMS,'batch':100,'commits':a.rows//100,'queries':len(cases),'k':10,'order':'alternate dimension each insert batch and query','distribution':'independent normal(0,0.5), float32; 20 balanced categories; synthetic, not official hidden input','schema':{str(d):f'CREATE TABLE docs{d} (id BIGINT PRIMARY KEY,category VARCHAR(32),content VARCHAR(1024),embedding VECTOR({d}));' for d in DIMS},'sha256':{f:sha(root/f) for f in files}}
 (root/'prepared.json').write_text(json.dumps(meta,indent=2)+'\n');print(json.dumps(meta),flush=True)

class Probe:
 def __init__(self,data,enabled):
  self.path=data/'sync-probe.bin';self.map=None
  if enabled:
   data.mkdir(parents=True,exist_ok=True)
   with self.path.open('wb') as f:f.truncate(4096)
   self.fd=self.path.open('r+b');self.map=mmap.mmap(self.fd.fileno(),4096)
 def phase(self,name):
  if self.map is not None:struct.pack_into('<Q',self.map,0,PHASES.index(name))
 def result(self):
  if self.map is None:return None
  vals=struct.unpack_from('<512Q',self.map);out={}
  for i,phase in enumerate(PHASES):
   stats={}
   for j,kind in enumerate(KINDS):
    for k,fn in enumerate(['fsync','fdatasync']):
     base=8+((i*6+j)*2+k)*3;count,ns,errors=vals[base:base+3]
     if count:stats[kind+'/'+fn]={'count':count,'ms':ns/1e6,'errors':errors}
   out[phase]=stats
  return out
 def close(self):
  if self.map is not None:self.map.close();self.fd.close()

def stats(records):
 groups={}
 for key in sorted({f'{q["dim"]}/{q["filter"]}' for q in records}):
  xs=[q for q in records if f'{q["dim"]}/{q["filter"]}'==key];v=sorted(q['seconds']*1000 for q in xs)
  m={'count':len(v),'total_s':sum(v)/1000,'mean_ms':statistics.mean(v),'p95_ms':v[min(len(v)-1,int(.95*len(v)))],'qps':1000/statistics.mean(v)}
  for name in ['filtered_candidates','full_distance_evaluations','distance_dimensions','blocks_pruned']:
   values=[q['metrics'][name] for q in xs if name in q['metrics']]
   if len(values)==len(xs):m[name+'_mean']=statistics.mean(values)
  paths={str(q['metrics'].get('storage_path','unreported')) for q in xs};m['storage_paths']=sorted(paths)
  stages=[q['metrics'].get('profile_ms',{}) for q in xs]
  if all(stages):m['profile_ms_median']={k:statistics.median(v[k] for v in stages) for k in set.intersection(*[set(x) for x in stages])}
  groups[key]=m
 return groups

def verify_queries(prep,queries):
 truth={}
 for d in DIMS:
  for line in (prep/str(d)/'oracle.jsonl').open():r=json.loads(line);truth[r['id']]=r['rows']
 cases={c['id']:c for c in json.loads((prep/'cases.json').read_text())};maps={};files=[]
 try:
  for d in DIMS:
   f=(prep/f'vectors{d}.f32').open('rb');files.append(f);maps[d]=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)
  for q in queries:
   c=cases[q['id']];expected=truth[q['id']];got=q['rows'];assert [r[0] for r in got]==[r[0] for r in expected],('exact IDs/order',q['id'],got,expected)
   for pk,dist in got:
    assert c['filter']=='none' or pk%20==c['category'],('filter',q['id'],pk)
    v=struct.unpack_from(f'<{c["dim"]}f',maps[c['dim']],(pk-1)*c['dim']*4)
    actual=math.sqrt(math.fsum((float(x)-float(y))**2 for x,y in zip(v,c['vector'])))
    assert math.isfinite(dist) and math.isclose(actual,dist,rel_tol=1e-5,abs_tol=1e-5),('distance',q['id'],pk,dist,actual)
  return {'passed':True,'queries':len(queries),'independent_topk_ids_order':True,'returned_distances_python_fsum':True}
 finally:
  for m in maps.values():m.close()
  for f in files:f.close()

def run(a):
 a.root.mkdir(parents=True,exist_ok=False);prep=a.prepared;meta=json.loads((prep/'prepared.json').read_text());cases=json.loads((prep/'cases.json').read_text());requests=(prep/'queries.jsonl').read_bytes().splitlines(keepends=True)
 # Warm the common request stream equally outside the database's timing/cgroup.
 with (prep/'insert.jsonl').open('rb') as f:
  while f.read(4<<20):pass
 out={'label':a.label,'diagnostic':a.diagnostic,'binary_sha256':sha(a.binary),'prepared_sha256':meta['sha256'],'rows':meta['rows'],'batch':100,'limits':{'cpus':2,'engine_affinity':[0,1],'memory_bytes':2<<30,'swap':0},'passed':False,'resources':{},'queries':[],'phase':'startup','restore_verifier':'bounded-primary-ranges-v2'}
 client=None;probe=Probe(a.root/'data',a.diagnostic);env=['LITE_PROFILE=1','LITE_SYNC_PROBE=/data/sync-probe.bin'] if a.diagnostic else [];env+=getattr(a,'engine_env',[])
 out['engine_env']=env
 def save():
  (a.root/'progress.json').write_text(json.dumps({k:v for k,v in out.items() if k not in ['queries','warmup','grouped_queries']},indent=2)+'\n')
 with (a.root/'stderr.log').open('wb') as err:
  try:
   start=time.perf_counter();client=MixedClient(str(a.binary.resolve()),a.root/'data',err,a.image,engine_env=env);out['startup_s']=time.perf_counter()-start;out['image_id']=client.image
   out['phase']='schema';probe.phase('schema')
   for d in DIMS:client.ask(meta['schema'][str(d)],d)
   out['resources']['write_start']=client.metrics();out['phase']='write';out['write_by_dim_s']={str(d):0.0 for d in DIMS};save();start=time.perf_counter()
   with (prep/'insert.jsonl').open('rb') as f:
    for i,request in enumerate(f):
     d=DIMS[i%2];probe.phase('insert'+str(d));t=time.perf_counter();response=client.send(request);out['write_by_dim_s'][str(d)]+=time.perf_counter()-t
     assert response.get('affected_rows')==100,('affected rows',i,response);out['commits_done']=i+1
     if (i+1)%100==0:save();print(a.label,'commits',i+1,flush=True)
   out['insert_wall_s']=time.perf_counter()-start;out['resources']['write_end']=client.metrics();out['phase']='index';probe.phase('index');start=time.perf_counter()
   for d in DIMS:client.ask(f'CREATE VECTOR INDEX idx{d} ON docs{d}(embedding);',d)
   out['index_s']=time.perf_counter()-start;out['phase']='warmup';probe.phase('warmup');out['warmup']=[];start=time.perf_counter()
   for c in json.loads((prep/'warmup.json').read_text()):
    t=time.perf_counter();r=client.ask(sql(c),c['id']);out['warmup'].append({'dim':c['dim'],'filter':c['filter'],'seconds':time.perf_counter()-t,'metrics':r.get('metrics',{})})
   out['warmup_s']=time.perf_counter()-start;out['resources']['query_start']=client.metrics();out['phase']='query';probe.phase('query');start=time.perf_counter();save()
   for c,request in list(zip(cases,requests))[:32 if a.diagnostic else len(cases)]:
    t=time.perf_counter();r=client.send(request);out['queries'].append({'id':c['id'],'dim':c['dim'],'filter':c['filter'],'seconds':time.perf_counter()-t,'rows':r['rows'],'metrics':r.get('metrics',{})});out['queries_done']=len(out['queries'])
    if len(out['queries'])%100==0:save();print(a.label,'queries',len(out['queries']),flush=True)
   out['query_wall_s']=time.perf_counter()-start;out['resources']['query_end']=client.metrics();out['query_groups']=stats(out['queries'])
   if a.diagnostic:
    out['phase']='grouped_diagnostic';out['grouped_queries']=[]
    # Unique queries not used by warmup/interleaved phase: avoids legacy cache hits.
    for c in sorted(cases[32:96],key=lambda c:c['dim']):
     t=time.perf_counter();r=client.ask(sql(c),200000+c['id']);out['grouped_queries'].append({'id':c['id'],'dim':c['dim'],'filter':c['filter'],'seconds':time.perf_counter()-t,'rows':r['rows'],'metrics':r.get('metrics',{})})
    out['grouped_query_groups']=stats(out['grouped_queries'])
   out['phase']='shutdown';probe.phase('shutdown');save();out['resources']['after_shutdown']=client.close();out['checkpoint_s']=client.close_s;client=None
   out['primary_stage_sum_s']=sum(out[k] for k in ['insert_wall_s','index_s','warmup_s','query_wall_s','checkpoint_s']);out['sync_diagnostic']=probe.result();out['phase']='query_verify';save()
   out['exact_check']=verify_queries(prep,out['queries']+out.get('grouped_queries',[]))
   if not a.diagnostic:
    out['phase']='restart';probe.phase('restart');start=time.perf_counter();client=MixedClient(str(a.binary.resolve()),a.root/'data',err,a.image);out['restart_s']=time.perf_counter()-start;out['phase']='full_restore_verify';save();start=time.perf_counter();checked=0;n=meta['rows_per_dimension']
    for d in DIMS:
     with (prep/f'vectors{d}.f32').open('rb') as raw:
      for begin in range(0,n,128):
       rows=client.ask(f'SELECT id,category,content,embedding FROM docs{d} WHERE id>{begin} AND id<={min(begin+128,n)} ORDER BY id LIMIT 128;',400000+begin+d)['rows'];assert len(rows)==min(128,n-begin),('restored length',d,begin,len(rows))
       for i,row in enumerate(rows,begin+1):
        assert row[:3]==[i,f'cat{i%20}',f'doc{d}-{i}'],('restored scalars',d,i)
        assert struct.pack(f'<{d}f',*row[3])==raw.read(d*4),('restored float32',d,i)
        checked+=1
       if (begin//128+1)%100==0:out['verified_rows']=checked;save()
      assert not client.ask(f'SELECT id FROM docs{d} WHERE id>{n} ORDER BY id LIMIT 1;')['rows']
    out['verified_rows']=checked;assert checked==meta['rows'];out['restore_check_s']=time.perf_counter()-start;out['resources']['after_restore']=client.close();client=None;out['restart_pass']=True
   out['final_file_bytes']=sum(p.stat().st_size for p in (a.root/'data').rglob('*') if p.is_file());out['passed']=True;out['phase']='complete'
  except BaseException as exc:
   out['failure']={'type':type(exc).__name__,'message':str(exc),'traceback':traceback.format_exc()};out['sync_diagnostic']=probe.result()
   if client is not None:
    try:out['resources']['at_failure']=client.metrics()
    except Exception as e:out['resource_capture_error']=str(e)
   print(a.label,'FAILED',out['phase'],str(exc),flush=True)
  finally:
   if client is not None:client.abort()
   probe.close();(a.root/'result.json').write_text(json.dumps(out,indent=2)+'\n');save()
 print(json.dumps({k:out[k] for k in ['label','passed','phase','insert_wall_s','query_wall_s','query_groups','primary_stage_sum_s','failure'] if k in out}),flush=True)
 return 0 if out['passed'] else 1

def main():
 p=argparse.ArgumentParser(description=__doc__);sub=p.add_subparsers(dest='command',required=True)
 x=sub.add_parser('prepare');x.add_argument('--root',type=Path,required=True);x.add_argument('--rows',type=int,required=True);x.add_argument('--queries',type=int,default=1000)
 x=sub.add_parser('run');x.add_argument('--prepared',type=Path,required=True);x.add_argument('--root',type=Path,required=True);x.add_argument('--binary',type=Path,required=True);x.add_argument('--image',required=True);x.add_argument('--label',required=True);x.add_argument('--diagnostic',action='store_true');x.add_argument('--engine-env',action='append',default=[])
 a=p.parse_args();return prepare(a) if a.command=='prepare' else run(a)
if __name__=='__main__':raise SystemExit(main())
