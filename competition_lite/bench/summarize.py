#!/usr/bin/env python3
import argparse,json,glob,statistics,os

def main():
 ap=argparse.ArgumentParser(); ap.add_argument('--root',required=True); a=ap.parse_args(); xs=[]
 for p in glob.glob(a.root+'/**/summary.json',recursive=True):
  try: xs.append(json.load(open(p)))
  except Exception: pass
 out={'runs':len(xs),'rows':sorted(set(x.get('rows') for x in xs)),'insert_mean_s':statistics.mean(x['insert_seconds'] for x in xs) if xs else None,'exact_mean_s':statistics.mean(x['exact_mean_seconds'] for x in xs) if xs else None,'ann_mean_s':statistics.mean(x['ann_mean_seconds'] for x in xs) if xs else None,'index_mean_s':statistics.mean(x['index_seconds'] for x in xs) if xs else None}
 print(json.dumps(out,indent=2)); json.dump(out,open(os.path.join(a.root,'summary-all.json'),'w'),indent=2)
if __name__=='__main__': main()
