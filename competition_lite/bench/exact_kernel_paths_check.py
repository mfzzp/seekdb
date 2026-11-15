#!/usr/bin/env python3
"""Odd dimensions/group tails, sparse candidates and mmap-window transitions."""
import argparse
import math
import os
from pathlib import Path
import random
from protocol_adapter import Session

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',required=True);ap.add_argument('--root',type=Path,required=True)
    a=ap.parse_args();a.root.mkdir(parents=True,exist_ok=False);checks=0
    os.environ['LITE_MMAP_MB']='1';os.environ['LITE_PROFILE']='1'
    for dim in [33,65,129,256]:
        for matrix in [0,64]:
            os.environ['LITE_MATRIX_MB']=str(matrix)
            data=(a.root/f'd{dim}-m{matrix}').resolve().as_posix()
            s=Session(str(Path(a.binary).resolve()),data);n=2051
            s.ask(f'CREATE TABLE docs (id BIGINT PRIMARY KEY, tag BIGINT, v VECTOR({dim}));')
            order=list(range(1,n+1));random.Random(221).shuffle(order)
            model={i:[float(((i%211)*19+j*31)%257-128)/32 for j in range(dim)] for i in order}
            for begin in range(0,n,100):
                s.ask('INSERT INTO docs VALUES '+','.join(f"({i},{i%3},'{model[i]}')" for i in order[begin:begin+100])+';')
            def check():
                nonlocal checks
                for q in [[0.0]*dim,[1.5]*dim,model[11]]:
                    for where,pred in [('',lambda i:True),(' WHERE tag=1',lambda i:i%3==1),(' WHERE id<1997',lambda i:i<1997)]:
                        truth=sorted((math.fsum((x-y)**2 for x,y in zip(v,q)),i) for i,v in model.items() if pred(i))
                        for limit in [1,10,257]:
                            result=s.ask(f"SELECT id,L2_DISTANCE(v,'{q}') AS d FROM docs{where} ORDER BY d LIMIT {limit};")
                            expected=[[i,math.sqrt(d)] for d,i in truth[:limit]]
                            assert result['rows']==expected,(dim,matrix,where,limit,result['rows'][:2],expected[:2])
                            assert result['metrics']['filtered_candidates']==len(truth)
                            checks+=1
            check();original={i:model[i][:] for i in [1,3,7,11]};s.ask('BEGIN;')
            for i in original:
                model[i]=[float(i)]*dim;s.ask(f"UPDATE docs SET v='{model[i]}' WHERE id={i};")
            check();s.ask('ROLLBACK;');model.update(original);check()
            s.ask('DELETE FROM docs WHERE id=2;');del model[2]
            s.close();s=Session(str(Path(a.binary).resolve()),data);check();s.close()
            print(f'PASS dim={dim} matrix={matrix} checks={checks}',flush=True)
    print(f'PASS total kernel-path queries={checks}',flush=True)

if __name__=='__main__':main()
