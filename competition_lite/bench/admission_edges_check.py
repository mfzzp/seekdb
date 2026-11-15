#!/usr/bin/env python3
"""Independent boundary checks for the published SQL/JSONL contract, not hidden cases."""
import argparse
import json
import math
from pathlib import Path
import random
import sys

from public_portable_check import Session, ROOT, strict_json


class Wire(Session):
    def send(self, sql=None, mode='exact', ident=None, ascii=False, op='execute'):
        self.n += 1
        ident = self.n if ident is None else ident
        req = {'id': ident, 'op': op}
        if sql is not None:
            req.update(sql=sql, search_mode=mode)
        self.p.stdin.write((json.dumps(req, ensure_ascii=ascii, allow_nan=False)+'\n').encode())
        self.p.stdin.flush()
        r = self.read()
        assert type(r.get('id')) is type(ident) and r['id'] == ident, (req, r)
        assert type(r.get('ok')) is bool, r
        return r

    def ok(self, sql, **kwargs):
        r = self.send(sql, **kwargs)
        assert r['ok'], r
        return r

    def error(self, sql):
        r = self.send(sql)
        assert r['ok'] is False and r.get('error', {}).get('code'), r


def unknown_table(s):
    s.error('SELECT id FROM absent;')
    s.ok('CREATE TABLE docs (id BIGINT PRIMARY KEY, n BIGINT);')
    s.error("INSERT INTO docs VALUES (1,'not-a-number');")
    s.ok('INSERT INTO docs VALUES (2,3);')
    assert s.ok('SELECT id,n FROM docs;')['rows'] == [[2,3]]


def short_sql(s):
    for sql in ['INSERT', 'INSERT INTO', 'UPDATE', 'DELETE', 'CREATE VECTOR INDEX', 'SELECT']:
        s.error(sql)
    s.ok('CREATE TABLE docs (id BIGINT PRIMARY KEY, v VECTOR(2));')
    for sql in ['CREATE VECTOR INDEX ix ON docs (', 'UPDATE docs SET v=',
                'SELECT id FROM docs LIMIT', 'SELECT id FROM docs ORDER BY']:
        s.error(sql)


def json_strings(s):
    s.ok('CREATE TABLE docs (id BIGINT PRIMARY KEY, title VARCHAR(128));')
    value='中文😀\b\f\x01'
    s.ok("INSERT INTO docs VALUES (1,'"+value+"');", ascii=True, ident='请求😀')
    assert s.ok('SELECT title FROM docs;')['rows'] == [[value]]
    s.shutdown();s.start()
    assert s.ok('SELECT title FROM docs;')['rows'] == [[value]]


def scalar_order(s):
    s.ok('CREATE TABLE docs (id BIGINT PRIMARY KEY,n BIGINT,title VARCHAR(32));')
    s.ok("INSERT INTO docs VALUES (1,9007199254740993,'z'),(2,9007199254740992,'a'),(3,-4,'m');")
    assert s.ok('SELECT id FROM docs ORDER BY n;')['rows'] == [[3],[2],[1]]
    assert s.ok('SELECT id FROM docs ORDER BY title;')['rows'] == [[2],[3],[1]]
    assert s.ok('SELECT id FROM docs ORDER BY title DESC;')['rows'] == [[1],[3],[2]]


def scalar_not_equal(s):
    s.ok('CREATE TABLE docs (id BIGINT PRIMARY KEY,n BIGINT,title VARCHAR(32));')
    s.ok("INSERT INTO docs VALUES (1,1,'a'),(2,2,'b');")
    assert s.ok('SELECT id FROM docs WHERE n!=1 ORDER BY id;')['rows'] == [[2]]
    assert s.ok("SELECT id FROM docs WHERE title!='a' ORDER BY id;")['rows'] == [[2]]


def scalar_validation(s):
    s.error('CREATE TABLE bad (id BIGINT PRIMARY KEY,v VECTOR(0));')
    s.ok('CREATE TABLE docs (id BIGINT PRIMARY KEY,n BIGINT,title VARCHAR(2));')
    for row in ["(1,1.5,'a')", "(1,9223372036854775808,'a')", "(1,1,'abc')"]:
        s.error('INSERT INTO docs VALUES '+row+';')
    s.ok("INSERT INTO docs VALUES (1,001,'中文');")
    assert s.ok('SELECT id FROM docs WHERE n=1;')['rows']==[[1]]
    assert s.ok('SELECT id FROM docs WHERE n=+001;')['rows']==[[1]]
    s.error('SELECT id FROM docs WHERE n=1.5;')
    s.error("UPDATE docs SET title='太长了' WHERE id=1;")
    assert s.ok('SELECT n,title FROM docs;')['rows']==[[1,'中文']]


def invalid_vectors(s):
    s.ok('CREATE TABLE docs (id BIGINT PRIMARY KEY,v VECTOR(2));')
    s.ok("INSERT INTO docs VALUES (1,'[1,2]');")
    for text in ['[NaN,0]', '[Infinity,0]', '[1e39,0]', '[1]', '[1 2]', '[,1,2]',
                 '[1,,2]', '[1,2,]', '[1e99999999999999999999999,0]']:
        s.error(f"INSERT INTO docs VALUES (2,'{text}');")
    assert s.ok('SELECT id,v FROM docs;')['rows'] == [[1,[1,2]]]


def vector_precision(s):
    dim=128
    s.ok(f'CREATE TABLE docs (id BIGINT PRIMARY KEY,v VECTOR({dim}));')
    a=[0.0]*dim;a[0]=10000.0;a[16]=1.0
    b=a[:];b[16]=0.0
    s.ok(f"INSERT INTO docs VALUES (1,'{a}'),(2,'{b}');")
    r=s.ok(f"SELECT id,L2_DISTANCE(v,'{[0.0]*dim}') AS d FROM docs ORDER BY d LIMIT 2;")
    assert [x[0] for x in r['rows']]==[2,1],r
    assert math.isclose(r['rows'][1][1],math.sqrt(100000001),rel_tol=1e-12),r


def vector_finite_range(s):
    dim=128
    s.ok(f'CREATE TABLE docs (id BIGINT PRIMARY KEY,v VECTOR({dim}));')
    s.ok(f"INSERT INTO docs VALUES (1,'{[1e20]*dim}'),(2,'{[0.0]*dim}');")
    r=s.ok(f"SELECT id,L2_DISTANCE(v,'{[-1e20]*dim}') AS d FROM docs ORDER BY d LIMIT 2;")
    assert [x[0] for x in r['rows']]==[2,1] and all(math.isfinite(x[1]) for x in r['rows']),r


def projection_order(s):
    s.ok('CREATE TABLE docs (id BIGINT PRIMARY KEY,v VECTOR(2),title VARCHAR(8));')
    s.ok("INSERT INTO docs VALUES (1,'[3,4]','x');")
    r=s.ok("SELECT L2_DISTANCE(v,'[0,0]') AS d,id,title FROM docs ORDER BY d LIMIT 1;")
    assert r['columns']==['d','id','title'] and r['rows']==[[5,1,'x']],r


def ddl_rollback(s):
    s.ok('BEGIN;');s.ok('CREATE TABLE docs (id BIGINT PRIMARY KEY,v VECTOR(2));')
    s.ok('CREATE VECTOR INDEX ix ON docs(v);');s.ok('ROLLBACK;')
    s.ok('CREATE TABLE docs (id BIGINT PRIMARY KEY,v VECTOR(2));')
    s.ok('CREATE VECTOR INDEX ix ON docs(v);')
    s.ok("INSERT INTO docs VALUES (1,'[3,4]');")
    s.shutdown();s.start()
    assert s.ok('SELECT id,v FROM docs;')['rows']==[[1,[3,4]]]


def crash_recovery(s):
    s.ok('CREATE TABLE docs (id BIGINT PRIMARY KEY,v VECTOR(2),n BIGINT);')
    s.ok("INSERT INTO docs VALUES (1,'[1,2]',1),(2,'[3,4]',2);")
    s.ok('BEGIN;');s.ok("UPDATE docs SET v='[5,6]',n=3 WHERE id>=1;")
    s.ok('COMMIT;');s.ok('BEGIN;');s.ok("UPDATE docs SET v='[7,8]' WHERE id=1;")
    s.ok("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM docs ORDER BY d LIMIT 2;")
    s.p.kill();s.p.wait(timeout=10);s.p.stdin.close();s.p.stdout.close();s.p=None
    s.start()
    assert s.ok('SELECT id,v,n FROM docs ORDER BY id;')['rows']==[[1,[5,6],3],[2,[5,6],3]]


def ann_recall(s):
    dim=32;n=512;rng=random.Random(20261115)
    s.ok(f'CREATE TABLE docs (id BIGINT PRIMARY KEY,tag VARCHAR(8),v VECTOR({dim}));')
    data=[[rng.uniform(-1,1) for _ in range(dim)] for _ in range(n)]
    for start in range(0,n,64):
        rows=','.join(f"({i},'g{i%2}','{data[i]}')" for i in range(start,min(n,start+64)))
        s.ok('INSERT INTO docs VALUES '+rows+';')
    s.ok('CREATE VECTOR INDEX ix ON docs(v);')
    for i in [17,106,211,350]:
        query=[rng.uniform(-1,1) for _ in range(dim)]
        for where in [''," WHERE tag='g1'"]:
            sql=f"SELECT id,L2_DISTANCE(v,'{query}') AS d FROM docs{where} ORDER BY d LIMIT 10;"
            exact=s.ok(sql)['rows'];ann=s.ok(sql,mode='ann')['rows']
            recall=len({r[0] for r in exact}&{r[0] for r in ann})/10
            assert recall>=.95,(i,where,recall,exact,ann)


CASES=[unknown_table,short_sql,json_strings,scalar_order,scalar_not_equal,scalar_validation,invalid_vectors,
       vector_precision,vector_finite_range,projection_order,ddl_rollback,crash_recovery,ann_recall]


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',required=True);ap.add_argument('--root',type=Path,required=True)
    ap.add_argument('--case',action='append')
    a=ap.parse_args();a.root.mkdir(parents=True,exist_ok=False)
    config=strict_json((ROOT/'protocol_check/config.json').read_text());config['request_timeout']=30
    results=[]
    for case in CASES:
        if a.case and case.__name__ not in a.case:continue
        with (a.root/(case.__name__+'.stderr.log')).open('wb') as log:
            s=Wire(str(Path(a.binary).resolve()),(a.root/case.__name__).resolve().as_posix(),log,config)
            try:
                s.start();case(s);s.shutdown();r={'case':case.__name__,'passed':True}
            except Exception as e:
                r={'case':case.__name__,'passed':False,'error':repr(e)[:2000],
                   'exit_code':s.p.poll() if s.p else None}
            finally:s.close()
        results.append(r);print(json.dumps(r,ensure_ascii=False),flush=True)
        (a.root/'result.json').write_text(json.dumps(results,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    return 0 if all(r['passed'] for r in results) else 1


if __name__=='__main__':sys.exit(main())
