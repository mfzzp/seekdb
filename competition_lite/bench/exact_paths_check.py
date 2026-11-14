#!/usr/bin/env python3
"""Oracle checks for scan/block, cache invalidation, ties, limits and restart."""
import argparse
import math
import os
from pathlib import Path
from protocol_adapter import Session


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',required=True);ap.add_argument('--root',type=Path,required=True)
    a=ap.parse_args();a.root.mkdir(parents=True,exist_ok=False)
    checks=0
    for mode in ['scan','block','auto']:
        os.environ['LITE_EXACT_PATH']=mode;os.environ['LITE_PROFILE']='1'
        os.environ['LITE_BLOCK_ORDER']='bound'
        for matrix in [False,True]:
            os.environ['LITE_MATRIX_MB']='64' if matrix else '0'
            root=(a.root/f'{mode}-{matrix}').resolve().as_posix()
            s=Session(str(Path(a.binary).resolve()),root)
            dim=128;n=2304
            s.ask(f'CREATE TABLE docs (id BIGINT PRIMARY KEY,tag VARCHAR(32),v VECTOR({dim}),w VECTOR(3));')
            model={}
            for begin in range(0,n,64):
                values=[]
                for i in range(begin,begin+64):
                    # Physically clustered blocks, with ties across block boundaries.
                    v=[float(i//64)]*dim;w=[float(i%11),float(i%3),0.0]
                    tag='odd' if i%2 else 'even';ident=n-i
                    model[ident]=[tag,v,w]
                    values.append(f"({ident},'{tag}','{v}','{w}')")
                s.ask('INSERT INTO docs VALUES '+','.join(values)+';')
            def verify():
                nonlocal checks
                for col,query,ix in [('v',[0.0]*dim,1),('w',[1.0,2.0,0.0],2),('v',[35.0]*dim,1)]:
                    for where,pred in [('',lambda i,r:True),(" WHERE tag='odd'",lambda i,r:r[0]=='odd'),
                                       (' WHERE id>=2000 AND id<2100',lambda i,r:2000<=i<2100),
                                       (" WHERE tag='absent'",lambda i,r:False)]:
                        truth=sorted((math.fsum((x-y)**2 for x,y in zip(r[ix],query)),i) for i,r in model.items() if pred(i,r))
                        for limit in [0,1,10,n+5]:
                            result=s.ask(f"SELECT id,L2_DISTANCE({col},'{query}') AS d FROM docs{where} ORDER BY d LIMIT {limit};")
                            expected=[[i,math.sqrt(d)] for d,i in truth[:limit]]
                            assert result['rows']==expected,(mode,matrix,col,where,limit,len(result['rows']),len(expected),[(x,y) for x,y in zip(result['rows'],expected) if x!=y][:3], [r for r in result['rows'] if r[0]==9999],s.ask('SELECT id,v,w FROM docs WHERE id=9999;')['rows'])
                            mt=result['metrics'];checks+=1
                            if mode!='block':
                                assert mt['blocks_considered']==0 and mt['profile_ms']['block_build']==0 and mt['profile_ms']['bounds']==0
                            if mode=='block' and col=='v' and not where and 0<limit<=10:
                                assert mt['blocks_pruned']>0,mt
                desc=s.ask("SELECT id,L2_DISTANCE(w,'[1,2,0]') AS d FROM docs ORDER BY d DESC LIMIT 10;")
                truth=sorted(((sum((x-y)**2 for x,y in zip(r[2],[1,2,0])),i) for i,r in model.items()),key=lambda x:(-x[0],x[1]))[:10]
                assert desc['rows']==[[i,math.sqrt(d)] for d,i in truth];checks+=1
            verify()
            s.ask('BEGIN;');original={k:[r[0],r[1][:],r[2][:]] for k,r in model.items()}
            s.ask(f"UPDATE docs SET v='{[100.0]*dim}',tag='odd' WHERE id=2304;");model[2304][0]='odd';model[2304][1]=[100.0]*dim
            verify();s.ask('ROLLBACK;');model=original;verify()
            s.ask("UPDATE docs SET w='[0,0,0]',tag='even' WHERE id=1;");model[1][0]='even';model[1][2]=[0.,0.,0.]
            s.ask('DELETE FROM docs WHERE id=2;');del model[2]
            s.ask(f"INSERT INTO docs VALUES (9999,'odd','{[0.0]*dim}','[0,0,0]');");model[9999]=['odd',[0.0]*dim,[0.,0.,0.]]
            verify();s.close();s=Session(str(Path(a.binary).resolve()),root);verify();s.close()
            print(f'PASS {mode=} {matrix=} {checks=}',flush=True)
    print(f'PASS total oracle queries={checks}',flush=True)


if __name__=='__main__':main()
