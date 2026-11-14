#!/usr/bin/env python3
"""Prepare byte-identical JSONL once, run frozen binaries serially, verify truth."""
import argparse
import hashlib
import json
import math
import mmap
import os
from pathlib import Path
import random
import select
import statistics
import struct
import subprocess
import tempfile
import time
import uuid
from run_bench import filt
from write_probe import snapshot
LIVE_CLIENTS = set()

FILTERS = ['none', 'category', 'tenant', 'category_tenant', 'price_range',
           'active_day', 'missing_category', 'id_small', 'title_exact']


def esc(value):
    return "'" + str(value).replace("'", "''") + "'"


def row_sql(row, vector):
    if 'content' in row:
        return '(' + ','.join([str(row['id']), esc(row['category']), esc(row['content']), esc(vector)]) + ')'
    return '(' + ','.join([str(row['id']), esc(row['category']), str(row['tenant_id']),
                           str(row['price_cents']), str(row['created_day']), str(row['active']),
                           esc(row['title']), esc(vector)]) + ')'


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(4 << 20), b''):
            h.update(block)
    return h.hexdigest()


def wire(sql, ident, mode='exact'):
    return json.dumps({'id': ident, 'op': 'execute', 'sql': sql,
                       'search_mode': mode}, separators=(',', ':'), ensure_ascii=False).encode() + b'\n'


def literal(values):
    return '[' + ','.join(format(x, '.9g') for x in values) + ']'


def query_sql(case):
    condition = (" WHERE category=" + esc(case['category'])) if case['filter'] == 'category' and 'category' in case else filt(case)
    return ('SELECT id,L2_DISTANCE(embedding,' + esc(literal(case['vector'])) +
            ') AS distance FROM docs' + condition + ' ORDER BY distance LIMIT 10;')


def prepare(args):
    start = time.perf_counter()
    data, root = Path(args.data).resolve(), Path(args.root).resolve()
    root.mkdir(parents=True, exist_ok=False)
    manifest = json.loads((data / 'manifest.json').read_text())
    public = manifest['schema_columns'] == 4
    n, d = min(args.rows or manifest['rows'], manifest['rows']), manifest['dimension']
    rows = [json.loads(line) for line in (data / 'rows.jsonl').open()][:n]
    assert [r['id'] for r in rows] == list(range(1, n + 1))
    order = [x[0] for x in struct.iter_unpack('<I', (data / 'load_order.u32').read_bytes()) if x[0] <= n]
    assert len(set(order)) == n
    request_bytes = max_request = requests = 0
    with (data / 'vectors.f32').open('rb') as source, (root / 'insert.jsonl').open('xb') as out:
        with mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as vectors:
            for base in range(0, n, args.batch):
                values = [row_sql(rows[i - 1], literal(struct.unpack_from('<%df' % d, vectors, (i-1)*d*4)))
                          for i in order[base:base + args.batch]]
                request = wire('INSERT INTO docs VALUES ' + ','.join(values) + ';', 1000 + requests)
                out.write(request); requests += 1
                request_bytes += len(request); max_request = max(max_request, len(request))
                if requests % 100 == 0: print('prepared rows', min(base + args.batch, n), flush=True)
    cases = [json.loads(line) for line in (data / 'query_cases.jsonl').open()][:args.queries]
    with (root / 'queries.f32').open('xb') as qf, (root / 'cases.tsv').open('x') as cf:
        for case in cases:
            packed = struct.pack('<%df' % d, *case['vector'])
            case['vector'] = struct.unpack('<%df' % d, packed)
            mask = (0 if case['filter'] == 'none' else 1 + int(case['category'][3:])) if public else FILTERS.index(case['filter'])
            qf.write(packed); cf.write('%d %d 10\n' % (case['id'], mask))
    (root / 'cases.json').write_text(json.dumps(cases))
    masks = [[] for _ in range(21 if public else len(FILTERS))]
    for r in rows:
        values = ([True] + [r['category'] == 'cat%d' % i for i in range(20)]) if public else [True, r['category'] == 'c01', r['tenant_id'] == 1,
                  r['category'] == 'c01' and r['tenant_id'] == 1,
                  25000 <= r['price_cents'] < 50000,
                  r['active'] == 1 and 100 <= r['created_day'] < 200,
                  r['category'] == 'missing', r['id'] < 100, r['title'] == 'item_00000001']
        for mask, value in zip(masks, values): mask.append(int(value))
    (root / 'masks.u8').write_bytes(b''.join(bytes(mask) for mask in masks))
    result = {'data': str(data), 'rows': n, 'dim': d, 'batch': args.batch,
              'requests': requests, 'request_bytes': request_bytes, 'max_request_bytes': max_request,
              'insert_sha256': sha(root / 'insert.jsonl'), 'query_cases_sha256': sha(root / 'cases.json'),
              'input_manifest': manifest, 'prepare_s': time.perf_counter() - start,
              'modes': ['exact'] if public else ['exact', 'ann'],
              'columns': ['id','category','content'] if public else ['id','category','tenant_id','price_cents','created_day','active','title'],
              'schema_sql': ('CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(32), content VARCHAR(1024), embedding VECTOR(%d));' if public else
                             'CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), tenant_id BIGINT, price_cents BIGINT, created_day BIGINT, active BIGINT, title VARCHAR(64), embedding VECTOR(%d));') % d}
    (root / 'prepared.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result), flush=True)


class Client:
    def __init__(self, binary, data, stderr, image):
        self.data = Path(data); self.data.mkdir(parents=True, exist_ok=True)
        self.name = 'lite-compare-' + uuid.uuid4().hex[:12]
        self.p = None; self.engine_pid = None
        subprocess.run(['docker','run','-d','--name',self.name,'--network=none','--cpus=2',
                        '--memory=2g','--memory-swap=2g','--pids-limit=128',
                        '--mount','type=bind,src=%s,dst=/engine,readonly' % binary,
                        '--mount','type=bind,src=%s,dst=/data' % self.data,
                        '--entrypoint','/bin/sleep',image,'infinity'], check=True, stdout=subprocess.DEVNULL)
        LIVE_CLIENTS.add(self)
        try:
            info = json.loads(subprocess.check_output(['docker','inspect',self.name]))[0]
            self.pid = info['State']['Pid']; self.image = info['Image']
            relative = (Path('/proc')/str(self.pid)/'cgroup').read_text().strip().split('::',1)[1]
            self.cgroup = Path('/sys/fs/cgroup')/relative.lstrip('/')
            dev = self.data.stat().st_dev; self.device = '%d:%d' % (os.major(dev), os.minor(dev))
            self.initial = self.metrics()
            self.p = subprocess.Popen(['docker','exec','-i',self.name,'/engine','--data-dir','/data'],
                                      stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr)
            assert self.read(120) == {'type':'ready'}
            for pid in (self.cgroup/'cgroup.procs').read_text().split():
                if (Path('/proc')/pid/'comm').read_text().strip() == 'engine':
                    self.engine_pid = int(pid)
                    os.sched_setaffinity(self.engine_pid, {0,1})
            if self.engine_pid is None: raise RuntimeError('engine PID not found in its container cgroup')
            assert self.initial['cpu_max']=='200000 100000' and self.initial['memory_max']=='2147483648'
            assert self.initial['swap_max']=='0'
        except BaseException:
            self.abort(); raise

    def metrics(self):
        cg = self.cgroup
        io = {} if (cg/'io.stat').exists() else None
        for line in (cg/'io.stat').read_text().splitlines() if io is not None else []:
            dev, *parts = line.split(); io[dev] = {k:int(v) for k,v in (s.split('=') for s in parts)}
        cpu = dict(line.split() for line in (cg/'cpu.stat').read_text().splitlines())
        proc = snapshot(self.engine_pid) if self.engine_pid and (Path('/proc')/str(self.engine_pid)).exists() else None
        return {'device':self.device, 'io':io, 'io_unavailable_reason':None if io is not None else 'rootless daemon delegated cpu/memory/pids only; io controller unavailable',
                'process_diagnostics_not_official_B':proc, 'memory_peak_bytes':int((cg/'memory.peak').read_text()),
                'memory_current_bytes':int((cg/'memory.current').read_text()), 'cpu_s':int(cpu['usage_usec'])/1e6,
                'memory_events':dict(line.split() for line in (cg/'memory.events').read_text().splitlines()),
                'cpu_max':(cg/'cpu.max').read_text().strip(), 'memory_max':(cg/'memory.max').read_text().strip(),
                'swap_max':(cg/'memory.swap.max').read_text().strip()}

    def drain(self):
        for path in self.data.rglob('*'):
            if path.is_file():
                fd = os.open(path, os.O_RDONLY)
                try: os.fsync(fd)
                finally: os.close(fd)
        fd = os.open(self.data, os.O_RDONLY | os.O_DIRECTORY)
        try: os.fsync(fd)
        finally: os.close(fd)
        return self.metrics()

    def abort(self):
        subprocess.run(['docker','rm','-f',self.name],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        if self.p is not None:
            self.p.wait(timeout=20)
        LIVE_CLIENTS.discard(self)

    def read(self, timeout=60):
        if not select.select([self.p.stdout], [], [], timeout)[0]:
            self.p.kill(); self.p.wait(); raise TimeoutError('protocol response deadline')
        line = self.p.stdout.readline()
        if not line: raise RuntimeError('engine exited; inspect stderr')
        if len(line) > 16777216: raise RuntimeError('response exceeds public 16 MiB limit')
        return json.loads(line)

    def send(self, request):
        self.p.stdin.write(request); self.p.stdin.flush()
        response = self.read()
        # Our encoder always emits id first; do not reparse megabytes of SQL inside the timed path.
        expected_id = json.loads(request[:request.index(b',"op"')] + b'}')['id']
        if response.get('id') != expected_id: raise RuntimeError('response id mismatch')
        if not response.get('ok'): raise RuntimeError(response)
        return response

    def ask(self, sql, ident=1, mode='exact'):
        return self.send(wire(sql, ident, mode))

    def close(self):
        start = time.perf_counter()
        self.send(b'{"id":99999999,"op":"shutdown"}\n')
        self.p.stdin.close()
        self.p.wait(timeout=120)
        if self.p.returncode: raise RuntimeError('shutdown failed')
        if self.p.stdout.read(): raise RuntimeError('extra output after shutdown')
        final = self.drain()
        self.close_s = time.perf_counter()-start
        self.abort()
        return final


def run(args):
    prep, root = Path(args.prepared).resolve(), Path(args.root).resolve()
    meta = json.loads((prep / 'prepared.json').read_text()); cases = json.loads((prep / 'cases.json').read_text())
    root.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix=args.label + '-', dir=root)); binary = Path(args.binary).resolve()
    # Identical OS-cache preparation for the common request file, outside database timing.
    with (prep / 'insert.jsonl').open('rb') as f:
        while f.read(4 << 20): pass
    with (out / 'stderr.log').open('wb') as err:
        t = time.perf_counter(); client = Client(binary, out / 'data', err, args.image)
        startup_s = time.perf_counter() - t
        image_id = client.image; initial = client.initial
        client.ask(meta['schema_sql'])
        before = client.metrics(); timings = []; t = time.perf_counter()
        with (prep / 'insert.jsonl').open('rb') as f:
            for i, request in enumerate(f):
                start = time.perf_counter(); response = client.send(request); timings.append(time.perf_counter()-start)
                assert response.get('affected_rows') == min(meta['batch'], meta['rows']-i*meta['batch'])
                if (i+1) % 50 == 0: print(args.label, 'insert batches', i+1, 'elapsed', round(time.perf_counter()-t, 3), flush=True)
        insert_wall = time.perf_counter() - t
        t = time.perf_counter(); inserted = client.drain(); write_drain_s = time.perf_counter() - t
        t = time.perf_counter(); client.ask('CREATE VECTOR INDEX idx ON docs(embedding);')
        index_s = time.perf_counter() - t
        rng = random.Random(928313); t = time.perf_counter()
        for i in range(18):
            values = struct.unpack('<%df' % meta['dim'], struct.pack('<%df' % meta['dim'], *[rng.gauss(0,1) for _ in range(meta['dim'])]))
            case = {'vector': values, 'filter': ('category' if i%2 else 'none') if meta['dim']==128 else FILTERS[i%9]}
            if meta['dim']==128: case['category']='cat%d'%(i%20)
            sql = query_sql(case)
            for mode in meta['modes']: client.ask(sql, 100000+i, mode)
        warmup_s = time.perf_counter() - t
        query_prepared = client.metrics()
        queries = []
        query_start = time.perf_counter()
        for i, case in enumerate(cases):
            sql = query_sql(case)
            for mode in meta['modes']:
                request = wire(sql, 200000+2*i+(mode=='ann'), mode)
                t = time.perf_counter(); result = client.send(request); elapsed = time.perf_counter()-t
                queries.append({'id': case['id'], 'mode': mode, 'filter': case['filter'],
                                'seconds': elapsed, 'rows': result['rows'], 'metrics': result.get('metrics', {})})
            if (i+1) % 128 == 0: print(args.label, 'query cases', i+1, flush=True)
        query_wall_s = time.perf_counter() - query_start
        t = time.perf_counter(); after = client.drain(); query_drain_s = time.perf_counter()-t
        t = time.perf_counter(); final = client.close()
        checkpoint_s = client.close_s
        t = time.perf_counter(); client = Client(binary, out / 'data', err, args.image); restart_s = time.perf_counter() - t
        t = time.perf_counter(); checked = 0
        with (Path(meta['data'])/'vectors.f32').open('rb') as vf, (Path(meta['data'])/'rows.jsonl').open() as rf:
            for base in range(1,meta['rows']+1,256):
                end=min(base+256,meta['rows']+1)
                found=client.ask('SELECT * FROM docs WHERE id >= %d AND id < %d ORDER BY id LIMIT 256;'%(base,end))['rows']
                assert len(found)==end-base, ('restart count',base,len(found))
                for row in found:
                    expected=json.loads(next(rf))
                    assert row[:-1]==[expected[k] for k in meta['columns']], ('restart scalar',base)
                    assert struct.pack('<%df'%meta['dim'],*row[-1])==vf.read(4*meta['dim']), ('restart vector',base)
                    checked+=1
                if checked%12800==0: print(args.label,'restart verified',checked,flush=True)
        assert not client.ask('SELECT id FROM docs WHERE id > %d ORDER BY id LIMIT 1;'%meta['rows'])['rows']
        verification_s=time.perf_counter()-t
        client.close()
    result={'label':args.label,'run':str(out),'binary':str(binary),'binary_sha256':sha(binary),'prepared':str(prep),
            'insert_sha256':meta['insert_sha256'],'query_cases_sha256':meta['query_cases_sha256'],
            'rows':meta['rows'],'dim':meta['dim'],'batch':meta['batch'],'requests':len(timings),
            'startup_s':startup_s,'insert_request_s':sum(timings),'insert_wall_s':insert_wall,
            'insert_cpu_s':inserted['cpu_s']-before['cpu_s'],'index_s':index_s,'warmup_s':warmup_s,
            'write_drain_s':write_drain_s,'query_wall_s':query_wall_s,'query_drain_s':query_drain_s,
            'checkpoint_s':checkpoint_s,'restart_s':restart_s,'restart_pass':True,'verified_rows':checked,
            'correctness_check_s':verification_s,'image_id':image_id,
            'resources':{'initial':initial,'write_end':inserted,'query_prepared':query_prepared,'query_end':after,'after_shutdown':final},
            'final_file_bytes':sum(p.stat().st_size for p in (out/'data').rglob('*') if p.is_file()),
            'queries':queries}
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k!='queries'}),flush=True)


def verify(args):
    truth={r['id']:r['rows'] for r in map(json.loads,Path(args.oracle).read_text().splitlines())}
    result=json.loads(Path(args.result).read_text()); groups={}
    prep=Path(result['prepared']); meta=json.loads((prep/'prepared.json').read_text())
    query_cases={c['id']:c for c in json.loads((prep/'cases.json').read_text())}
    case_masks={int(line.split()[0]):int(line.split()[1]) for line in (prep/'cases.tsv').read_text().splitlines()}
    masks=(prep/'masks.u8').read_bytes(); n=meta['rows']; dim=meta['dim']
    vf=(Path(meta['data'])/'vectors.f32').open('rb'); vectors=mmap.mmap(vf.fileno(),0,access=mmap.ACCESS_READ)
    for mode in sorted(set(q['mode'] for q in result['queries'])):
        cases=[q for q in result['queries'] if q['mode']==mode]
        errors=[]; recalls=[]; distance_errors=[]; invalid=[]; recalls_by_filter={}
        for q in cases:
            expected=truth[q['id']]; ids=[r[0] for r in q['rows']]; wanted=[r[0] for r in expected]
            if ids!=wanted:errors.append(q['id'])
            if len(ids)!=len(wanted) or len(ids)!=len(set(ids)) or any(not 1<=pk<=n or not masks[case_masks[q['id']]*n+pk-1] for pk in ids):
                invalid.append(q['id']); continue
            if wanted:
                recall=len(set(ids)&set(wanted))/len(wanted);recalls.append(recall)
                recalls_by_filter.setdefault(q['filter'],[]).append(recall)
            query=query_cases[q['id']]['vector']; bad_distance=False
            for pk,dist in q['rows']:
                vector=struct.unpack_from('<%df'%dim,vectors,(pk-1)*dim*4)
                actual=math.sqrt(math.fsum((float(x)-float(y))**2 for x,y in zip(vector,query)))
                if not math.isfinite(dist) or not math.isclose(dist,actual,rel_tol=1e-5,abs_tol=1e-5):bad_distance=True
            if bad_distance:distance_errors.append(q['id'])
        filters={}
        for name in FILTERS:
            samples=[q['seconds']*1000 for q in cases if q['filter']==name]
            if samples:filters[name]={'n':len(samples),'mean_ms':statistics.mean(samples),'p50_ms':statistics.median(samples),'p95_ms':sorted(samples)[min(len(samples)-1,int(len(samples)*.95))]}
            if name in recalls_by_filter:filters[name]['recall']=statistics.mean(recalls_by_filter[name])
        groups[mode]={'cases':len(cases),'mean_ms':statistics.mean(q['seconds'] for q in cases)*1000,
                      'id_order_match':1-len(errors)/len(cases),'recall':statistics.mean(recalls) if recalls else None,
                      'mismatch_ids':errors,'distance_error_ids':distance_errors,'invalid_ids':invalid,'filters':filters,
                      'passed':not distance_errors and not invalid and (not errors if mode=='exact' else all(statistics.mean(v)>=.95 for v in recalls_by_filter.values()))}
    vectors.close();vf.close()
    output={k:v for k,v in result.items() if k!='queries'};output['verification']=groups
    Path(args.out).write_text(json.dumps(output,indent=2)+'\n');print(json.dumps(output),flush=True)


def main():
    ap=argparse.ArgumentParser(description=__doc__);sub=ap.add_subparsers(dest='command',required=True)
    p=sub.add_parser('prepare');p.add_argument('--data',required=True);p.add_argument('--root',required=True)
    p.add_argument('--rows',type=int);p.add_argument('--batch',type=int,default=256);p.add_argument('--queries',type=int,default=1024)
    p=sub.add_parser('run');p.add_argument('--binary',required=True);p.add_argument('--prepared',required=True)
    p.add_argument('--root',required=True);p.add_argument('--label',required=True)
    p.add_argument('--image',default='python:3.12.10-slim-bookworm')
    p=sub.add_parser('verify');p.add_argument('--result',required=True);p.add_argument('--oracle',required=True);p.add_argument('--out',required=True)
    args=ap.parse_args()
    for key in ['root','out']:
        if hasattr(args,key) and '/zzp/' not in str(Path(getattr(args,key)).resolve()):raise ValueError('output must be under zzp')
    try:
        {'prepare':prepare,'run':run,'verify':verify}[args.command](args)
    finally:
        for client in list(LIVE_CLIENTS): client.abort()


if __name__=='__main__':main()
