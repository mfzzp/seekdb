#!/usr/bin/env python3
"""Publication errors/crashes checked by independent restart processes."""
import argparse
import json
from pathlib import Path
import subprocess

ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--binary',required=True);ap.add_argument('--root',type=Path,required=True)
a=ap.parse_args();a.root.mkdir(parents=True,exist_ok=False)
cases=[]
for op in ['flush','compact','checkpoint','gc']:
    faults=['ok','manifest_write','manifest_sync','manifest_rename','dir_before','dir_after','crash_before','crash_after']
    if op in ['flush','compact']:faults+=['sst_write','sst_sync']
    if op=='checkpoint':faults+=['wal_rotation_sync']
    if op=='gc':faults+=['gc_vector_sync']
    for fault in faults:cases.append((op,fault))
cases += [('compact','pins'),('checkpoint','lock'),('checkpoint','named_primary_cache')]
results=[]
for op,fault in cases:
    data=str((a.root/(op+'-'+fault)).resolve())
    p=subprocess.run([a.binary,data,fault,op],capture_output=True,text=True,timeout=120)
    assert p.returncode=={'crash_before':91,'crash_after':92}.get(fault,0),(op,fault,p.returncode,p.stdout,p.stderr)
    p=subprocess.run([a.binary,data,'inspect',op],capture_output=True,text=True,timeout=120)
    assert p.returncode==0,(op,fault,p.returncode,p.stdout,p.stderr)
    results.append({'operation':op,'fault':fault,'passed':True})
    print(json.dumps(results[-1]),flush=True)
data=str((a.root/'partial-apply').resolve())
for mode in ['partial_apply','replay_crash','replay_crash','inspect_large','inspect_large']:
    p=subprocess.run([a.binary,data,mode,'flush'],capture_output=True,text=True,timeout=120)
    assert p.returncode==(0 if mode=='inspect_large' else 92),(mode,p.returncode,p.stdout,p.stderr)
results.append({'operation':'large_transaction','fault':'crash_during_apply_and_two_replays','passed':True})
(a.root/'result.json').write_text(json.dumps({'passed':True,'cases':results},indent=2)+'\n')
