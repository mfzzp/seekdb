import sys, tempfile, time, json, random, struct, hashlib
sys.path.insert(0,sys.argv[1])
from minivectordb import Database
rng=random.Random(24081)
with tempfile.TemporaryDirectory() as root:
    db=Database(root)
    db.execute('CREATE TABLE t (id BIGINT PRIMARY KEY, g BIGINT, v VECTOR(128))','exact')
    db.mutate(('put',('t',i,[i,i%20,struct.pack('<128f',*[rng.random() for _ in range(128)])])) for i in range(12000))
    queries={
      'primary_payload_topk': ['SELECT * FROM t ORDER BY id DESC LIMIT 10']*100,
      'primary_range_distance': ["SELECT id,L2_DISTANCE(v,'"+str([0]*128)+"') AS d FROM t WHERE id>=6000 ORDER BY id LIMIT 10"]*100,
      'scalar_distance_projection': ["SELECT id,L2_DISTANCE(v,'"+str([0]*128)+"') AS d FROM t ORDER BY g,id LIMIT 10"]*60,
      'scalar_filter': ['SELECT id,g FROM t WHERE g=7 ORDER BY g,id LIMIT 10']*40,
      'unfiltered_scan': ['SELECT id,g FROM t']*15,
    }
    results={}
    for name,sqls in queries.items():
        db.execute(sqls[0],'exact')
        start=time.perf_counter()
        output=[db.execute(sql,'exact') for sql in sqls]
        results[name]={'seconds':time.perf_counter()-start,'digest':hashlib.sha256(json.dumps(output).encode()).hexdigest()}
    print(json.dumps({'source':sys.argv[1],'results':results}),flush=True)
    db.close()
