#!/usr/bin/env python3
"""Run unchanged public SQL cases/assertions over a portable threaded pipe transport.

The original protocol_check transport uses POSIX selectors/killpg. This adapter
supports Windows local development; it does not replace Docker admission checks.
"""
import argparse
import json
from pathlib import Path
import queue
import subprocess
import sys
import threading

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'protocol_check'))
from run import check_case
from _transport import strict_json


class Session:
    def __init__(self,binary,data,log,config):
        self.binary=binary;self.data=data;self.log=log;self.config=config;self.n=0;self.p=None
    def start(self):
        self.p=subprocess.Popen([self.binary,'--data-dir',self.data],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=self.log)
        self.queue=queue.Queue()
        def reader(stream,q):
            while True:
                line=stream.readline(self.config['max_response_bytes']+1)
                q.put(line)
                if not line: break
        threading.Thread(target=reader,args=(self.p.stdout,self.queue),daemon=True).start()
        assert self.read()=={'type':'ready'}
    def read(self):
        line=self.queue.get(timeout=self.config['request_timeout'])
        assert line.endswith(b'\n') and len(line)<=self.config['max_response_bytes'],line[:240]
        return strict_json(line.decode('utf-8'))
    def request(self,sql=None,mode='exact',op='execute'):
        self.n+=1;req={'id':self.n,'op':op}
        if sql is not None:req.update(sql=sql,search_mode=mode)
        self.p.stdin.write((json.dumps(req,ensure_ascii=False)+'\n').encode());self.p.stdin.flush()
        result=self.read();assert result.get('id')==self.n and type(result.get('ok')) is bool
        return result
    def shutdown(self):
        assert self.request(op='shutdown')['ok']
        self.p.stdin.close();assert self.p.wait(timeout=60)==0
        assert self.queue.get(timeout=10)==b''
        self.p.stdout.close();self.p=None
    def close(self):
        if self.p and self.p.poll() is None:self.p.kill();self.p.wait(timeout=10)


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',required=True);ap.add_argument('--root',type=Path,required=True)
    a=ap.parse_args();a.root.mkdir(parents=True,exist_ok=False)
    config=strict_json((ROOT/'protocol_check/config.json').read_text())
    cases=strict_json((ROOT/'mysqltest/manifest.json').read_text())['cases'];results=[]
    for case in cases:
        with (a.root/(case['id']+'.stderr.log')).open('wb') as log:
            s=Session(str(Path(a.binary).resolve()),(a.root/case['id']).resolve().as_posix(),log,config)
            try:
                s.start();check_case(s,case,ROOT/'mysqltest',config);s.shutdown()
                results.append({'id':case['id'],'passed':True});print('PASS',case['id'],flush=True)
            finally:s.close()
    (a.root/'result.json').write_text(json.dumps({'passed':True,'transport':'portable threaded local pipes; original public SQL and assertions','cases':results},indent=2)+'\n')


if __name__=='__main__':main()
