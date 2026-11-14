#!/usr/bin/env python3
"""Run two fixed datasets serially with immutable binaries; keep progress and failures."""
import argparse
import datetime
import json
from pathlib import Path
import subprocess
import sys
import time


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root', type=Path, required=True)
    args = ap.parse_args()
    root = args.root.resolve(); assert '/zzp/' in str(root)
    bench = Path(__file__).resolve().parent
    state = {'started': datetime.datetime.now().astimezone().isoformat(), 'status': 'running', 'phase': 'initializing', 'steps': []}
    summary = {'runs': [], 'scope': 'One local paired run per dataset; public parameters, not official score. io.stat unavailable in this rootless daemon.'}
    def save():
        (root/'driver-status.json').write_text(json.dumps(state, indent=2)+'\n')
        (root/'compare-summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    def run(name, command):
        state['phase']=name;save();print(datetime.datetime.now().isoformat(),name,'START',flush=True)
        start=time.perf_counter()
        with (root/(name+'.log')).open('wb') as log:
            rc=subprocess.run([str(x) for x in command],stdout=log,stderr=subprocess.STDOUT).returncode
        state['steps'].append({'phase':name,'returncode':rc,'seconds':time.perf_counter()-start})
        save();print(name,'DONE' if rc==0 else 'FAILED',flush=True);return rc==0
    datasets=[('public','public-input','public-prepared',128),('stress','input','stress-prepared',1024)]
    for label,data,prep,dim in datasets:
        if not run(label+'-oracle',[root/'exact_oracle',root/data/'vectors.f32',root/prep,100000,dim,root/(label+'-truth.jsonl')]):
            state['status']='failed-oracle';save();return 1
    for label,data,prep,dim in datasets:
        for version,filename in [('old','baseline'),('new','lite')]:
            name=label+'-'+version
            if not run(name,[sys.executable,bench/'compare_run.py','run','--binary',root/'container-binaries'/filename,
                            '--prepared',root/prep,'--root',root/'paired-runs','--label',name]):
                summary['runs'].append({'name':name,'completed':False,'log':str(root/(name+'.log'))});save();continue
            paths=list((root/'paired-runs').glob(name+'-*/result.json'))
            if len(paths)!=1: raise RuntimeError('ambiguous result files: '+name)
            if not run(name+'-verify',[sys.executable,bench/'compare_run.py','verify','--result',paths[0],
                                      '--oracle',root/(label+'-truth.jsonl'),'--out',root/(name+'-summary.json')]):
                summary['runs'].append({'name':name,'completed':False,'log':str(root/(name+'-verify.log'))});save();continue
            result=json.loads((root/(name+'-summary.json')).read_text())
            result['name']=name;result['completed']=True;summary['runs'].append(result);save()
    state['status']='completed';state['phase']='finished';state['finished']=datetime.datetime.now().astimezone().isoformat();save()
    print('COMPARISON_FINISHED',flush=True)
    return 0


if __name__=='__main__':
    raise SystemExit(main())
