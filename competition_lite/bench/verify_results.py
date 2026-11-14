#!/usr/bin/env python3
import argparse,json

def main():
 ap=argparse.ArgumentParser(); ap.add_argument('--queries',required=True); ap.add_argument('--oracle',required=True); a=ap.parse_args(); truth={json.loads(x)['id']:json.loads(x)['ids'] for x in open(a.oracle)}; data=json.load(open(a.queries)); ex=[x for x in data if x['mode']=='exact']; an=[x for x in data if x['mode']=='ann'];
 def score(xs):
  good=0; recall=0.0
  for x in xs:
   t=truth.get(x['id'],[]); got=x['ids']; good+=got==t; recall+=sum(y in t for y in got)/max(1,len(t))
  return good/len(xs),recall/max(1,len(xs))
 e=score(ex); n=score(an); out={'exact_cases':len(ex),'exact_match':e[0],'ann_recall':n[1],'ann_cases':len(an),'pass':e[0]==1.0 and n[1]>=0.8}; print(json.dumps(out,indent=2)); raise SystemExit(0 if out['pass'] else 1)
if __name__=='__main__': main()
