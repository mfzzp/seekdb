#!/usr/bin/env python3
import json,subprocess,time
class Session:
 def __init__(self,binary,data):
  self.p=subprocess.Popen([binary,'--data-dir',data],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True,bufsize=1); self.n=0
  ready=json.loads(self.p.stdout.readline()); assert ready.get('type')=='ready',ready
 def ask(self,sql,mode='exact'):
  self.n+=1; self.p.stdin.write(json.dumps({'id':self.n,'op':'execute','sql':sql,'search_mode':mode},separators=(',',':'))+'\n'); self.p.stdin.flush(); r=json.loads(self.p.stdout.readline())
  if not r.get('ok',False): raise RuntimeError(r)
  return r
 def close(self):
  self.p.stdin.write(json.dumps({'id':self.n+1,'op':'shutdown'})+'\n'); self.p.stdin.flush(); self.p.stdout.readline(); self.p.wait(timeout=60)
