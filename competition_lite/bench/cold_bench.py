#!/usr/bin/env python3
"""Measure warm queries and process-cold restart separately; never claims SSD cache was dropped."""
import argparse,json,time
from protocol_adapter import Session

def main():
 ap=argparse.ArgumentParser(); ap.add_argument('--binary',required=True); ap.add_argument('--data-dir',required=True); ap.add_argument('--cases',required=True); ap.add_argument('--limit',type=int,default=32); a=ap.parse_args()
 qs=[json.loads(x) for x in open(a.cases)][:a.limit];
 def sql(q):
  v='['+','.join(format(float(x),'.7g') for x in q['vector'])+']'; return "SELECT id,L2_DISTANCE(embedding,'"+v+"') AS distance FROM docs ORDER BY distance LIMIT 10;"
 s=Session(a.binary,a.data_dir); warm=[]
 for q in qs:
  t=time.perf_counter(); s.ask(sql(q),'exact'); warm.append(time.perf_counter()-t)
 t=time.perf_counter(); s.close(); shutdown=time.perf_counter()-t
 t=time.perf_counter(); s=Session(a.binary,a.data_dir); ready=time.perf_counter()-t; cold=[]
 for q in qs:
  t=time.perf_counter(); s.ask(sql(q),'exact'); cold.append(time.perf_counter()-t)
 s.close(); print(json.dumps({'queries':len(qs),'warm_mean_s':sum(warm)/len(warm),'process_cold_ready_s':ready,'process_cold_query_mean_s':sum(cold)/len(cold),'shutdown_s':shutdown,'storage_cold':'not_claimed'},sort_keys=True))
if __name__=='__main__': main()
