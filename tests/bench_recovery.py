import sys, tempfile, time, json, statistics
sys.path.insert(0, sys.argv[1])
from minivector_store import Store
with tempfile.TemporaryDirectory() as root:
    db=Store(root)
    db.mutate([('table',('t',[('id','bigint',None),('g','bigint',None)],0))])
    db.mutate(('put',('t',i,[i,i%50])) for i in range(150000))
    db.mutate(('delete',('t',i)) for i in range(0,150000,10))
    db.close()
    times=[]
    for _ in range(7):
        start=time.perf_counter()
        db=Store(root)
        times.append(time.perf_counter()-start)
        assert len(db.tables['t'].rows)==135000
        db.close()
    print(json.dumps({'source':sys.argv[1], 'seconds':times, 'median':statistics.median(times)}))
