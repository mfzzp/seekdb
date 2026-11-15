#!/usr/bin/env python3
"""Check that manifest, SST/index, WAL corruption and missing files fail closed."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
from protocol_adapter import Session

ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--binary',required=True);ap.add_argument('--root',type=Path,required=True)
a=ap.parse_args();a.root.mkdir(parents=True,exist_ok=False)
base=a.root/'baseline';s=Session(a.binary,str(base))
s.ask('CREATE TABLE t (id BIGINT PRIMARY KEY,v VECTOR(2),tail VARCHAR(64));')
s.ask('INSERT INTO t VALUES '+','.join(f"({i},'[{i},2]','last-{i}')" for i in range(1000))+';');s.close()
assert subprocess.run([a.binary,'--data-dir',str(base),'--check'],capture_output=True).returncode==0
cases=[]
for case in ['manifest_checksum','sst_block_checksum','sst_index_checksum','sst_missing','sst_truncated','vectors_truncated','wal_missing']:
    dst=a.root/case;shutil.copytree(base,dst)
    if case.startswith('manifest'):file=dst/'LSM-MANIFEST';off=12
    elif case.startswith('sst'):file=next(dst.glob('*.sst'));off=20 if 'block' in case else -20
    elif case.startswith('vectors'):file=dst/'vectors.f32';off=0
    else:file=next(dst.glob('*.wal'));off=0
    if case.endswith('missing'):file.unlink()
    elif case.endswith('truncated'):
        with file.open('r+b') as f:f.truncate(1)
    else:
        with file.open('r+b') as f:
            if off<0:off=file.stat().st_size+off
            f.seek(off);x=f.read(1);f.seek(off);f.write(bytes([x[0]^128]))
    p=subprocess.run([a.binary,'--data-dir',str(dst),'--check'],capture_output=True,text=True)
    assert p.returncode!=0,(case,p.stdout,p.stderr)
    cases.append({'case':case,'passed':True,'stderr':p.stderr.strip()})
# A checksum mismatch in a committed WAL is different from a torn final frame.
dst=a.root/'wal_checksum';s=Session(a.binary,str(dst))
s.ask('CREATE TABLE t (id BIGINT PRIMARY KEY,v VECTOR(2));');s.ask("INSERT INTO t VALUES (1,'[1,2]');")
s.p.kill();s.p.wait(timeout=30);file=next(dst.glob('*.wal'))
with file.open('r+b') as f:f.seek(15);x=f.read(1);f.seek(15);f.write(bytes([x[0]^1]))
p=subprocess.run([a.binary,'--data-dir',str(dst),'--check'],capture_output=True,text=True)
assert p.returncode!=0,(p.stdout,p.stderr);cases.append({'case':'wal_checksum','passed':True,'stderr':p.stderr.strip()})
report={'passed':True,'cases':cases};(a.root/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report),flush=True)
