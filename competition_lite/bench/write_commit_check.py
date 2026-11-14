#!/usr/bin/env python3
"""Exercise commit syscall faults, restart, CRC and transaction statement undo."""
import argparse
import json
from pathlib import Path
import subprocess

ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--binary',required=True);ap.add_argument('--root',type=Path,required=True)
a=ap.parse_args();a.root.mkdir(parents=True,exist_ok=False)
binary=str(Path(a.binary).resolve());results=[]
for case in ['crc','ok','short','eintr','sync_eintr','partial_vector','partial_wal','vector_sync','wal_sync','lsm_failure','crash_vector','crash_wal','statement_undo']:
    data=(a.root/case).resolve().as_posix()
    r=subprocess.run([binary,data,case],capture_output=True,text=True)
    assert r.returncode=={'crash_vector':91,'crash_wal':92}.get(case,0),(case,r.returncode,r.stdout,r.stderr)
    output=r.stdout
    if case!='crc':
        r=subprocess.run([binary,data,'inspect'],capture_output=True,text=True)
        assert r.returncode==0,(case,r.returncode,r.stdout,r.stderr)
        expected=1 if case in ['partial_vector','partial_wal','vector_sync','crash_vector'] else 3
        assert f'rows={expected}' in r.stdout,(case,r.stdout)
        output+=r.stdout
    print(case,output.strip(),flush=True);results.append({'case':case,'passed':True,'output':output})
(a.root/'result.json').write_text(json.dumps({'passed':True,'cases':results},indent=2)+'\n')
