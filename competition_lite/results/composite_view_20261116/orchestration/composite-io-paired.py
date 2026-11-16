from pathlib import Path
import json,os,subprocess,time
b=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/composite-20261116/view1')
env=dict(os.environ,PATH='/home/zzp/.local/bin:'+os.environ['PATH'],DOCKER_HOST='unix:///run/user/1002/docker-competition.sock')
prep=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/iteration-20261115-ce21bb9/prepared10k')
old=Path('/mnt/SSD1/zzp/seekdb-round2-lite2000/distance-screen-20261116/dev4/bin/docker-runtime')
with (b/'io-paired.log').open('w') as log:
 subprocess.run(['python3',str(b/'source/competition_lite/bench/paired_compare.py'),'--prepared',str(prep),'--oracle',str(prep.parent/'oracle10k.jsonl'),'--root',str(b/'io-paired'),'--image','lite:composite-view-20261116','--variant','baseline='+str(old),'--variant','view='+str(b/'bin/docker-runtime'),'--rounds','3'],env=env,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=600)
r=json.loads((b/'io-paired/summary.json').read_text())
for run in r['runs']:
 x=json.loads(Path(run['report']).read_text());rs=x['resources'];dev=rs['after_shutdown']['device']
 assert rs['after_shutdown']['io'] is not None and dev in rs['after_shutdown']['io'],rs['after_shutdown']['io']
 print(json.dumps({'label':run['variant'],'round':run['round'],'write':x['insert_wall_s'],'query':x['query_wall_s'],'peak':rs['after_shutdown']['memory_peak_bytes'],'io':rs['after_shutdown']['io']}),flush=True)
