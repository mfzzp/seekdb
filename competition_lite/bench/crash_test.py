#!/usr/bin/env python3
import argparse,json,os,signal,subprocess,time,random,shutil

def ask(p,n,sql):
 n+=1; p.stdin.write(json.dumps({'id':n,'op':'execute','sql':sql})+'\n'); p.stdin.flush(); return n,json.loads(p.stdout.readline())
def run(a,root,rounds):
 os.makedirs(root,exist_ok=True); out=[]
 for r in range(rounds):
  d=os.path.join(root,'r%02d'%r); shutil.rmtree(d,ignore_errors=True)
  p=subprocess.Popen([a.binary,'--data-dir',d],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True,bufsize=1); p.stdout.readline(); n=0
  n,x=ask(p,n,'CREATE TABLE t (id BIGINT PRIMARY KEY, v BIGINT, embedding VECTOR(4));')
  n,x=ask(p,n,'INSERT INTO t VALUES (1,10,\'[0,0,0,0]\');')
  n,x=ask(p,n,'BEGIN;'); n,x=ask(p,n,'INSERT INTO t VALUES (2,20,\'[1,1,1,1]\');')
  os.kill(p.pid,signal.SIGKILL); p.wait()
  q=subprocess.Popen([a.binary,'--data-dir',d],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True,bufsize=1); q.stdout.readline(); n,y=ask(q,0,'SELECT id FROM t ORDER BY id LIMIT 10;'); q.stdin.write(json.dumps({'id':99,'op':'shutdown'})+'\n'); q.stdin.flush(); q.stdout.readline(); q.wait()
  ids=[z[0] for z in y.get('rows',[])]; out.append({'round':r,'committed_present':1 in ids,'uncommitted_absent':2 not in ids,'ids':ids})
 json.dump(out,open(os.path.join(root,'crash.json'),'w'),indent=2); print(json.dumps({'rounds':rounds,'pass':sum(x['committed_present'] and x['uncommitted_absent'] for x in out)}))
def main():
 ap=argparse.ArgumentParser(); ap.add_argument('--binary',required=True); ap.add_argument('--root',required=True); ap.add_argument('--rounds',type=int,default=20); a=ap.parse_args(); run(a,a.root,a.rounds)
if __name__=='__main__': main()
