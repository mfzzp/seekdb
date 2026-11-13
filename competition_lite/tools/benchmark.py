#!/usr/bin/env python3
import argparse,json,os,subprocess,time
def run(binary,data_dir,n,dim,batch,queries):
 os.system('rm -rf '+data_dir); os.makedirs(data_dir,exist_ok=True)
 v='['+','.join('0.1' for _ in range(dim))+']'
 p=subprocess.Popen([binary,'--data-dir',data_dir],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True,bufsize=1)
 assert json.loads(p.stdout.readline())=={'type':'ready'}
 seq=0
 def ask(sql):
  nonlocal seq
  seq+=1;p.stdin.write(json.dumps({'id':seq,'op':'execute','sql':sql,'search_mode':'exact'},separators=(',',':'))+'\n');p.stdin.flush();o=json.loads(p.stdout.readline())
  if not o.get('ok',True): raise RuntimeError(o)
  return o
 t0=time.perf_counter();ask('CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), tenant_id BIGINT, price_cents BIGINT, created_day BIGINT, active BIGINT, title VARCHAR(32), embedding VECTOR('+str(dim)+'));')
 for a in range(0,n,batch):
  rows=[f"({i},'c{i%20}',{i%10},{100+i%10000},{19000+i%365},{1 if i%7 else 0},'title{i}','{v}')" for i in range(a+1,min(n,a+batch)+1)]
  ask('INSERT INTO docs VALUES '+','.join(rows)+';')
 t1=time.perf_counter();q='['+','.join('0.1' for _ in range(dim))+']';q0=time.perf_counter()
 for _ in range(queries):ask("SELECT id, L2_DISTANCE(embedding, '"+q+"') AS d FROM docs WHERE category = 'c1' ORDER BY d LIMIT 10;")
 q1=time.perf_counter();p.stdin.write(json.dumps({'id':seq+1,'op':'shutdown'})+'\n');p.stdin.flush();p.stdout.readline();p.stdin.close()
 try:p.wait(timeout=20)
 except subprocess.TimeoutExpired:p.kill()
 return {'rows':n,'dim':dim,'insert_s':t1-t0,'query_s':q1-q0,'requests':seq,'vectors_bytes':os.path.getsize(data_dir+'/vectors.f32') if os.path.exists(data_dir+'/vectors.f32') else 0,'wal_bytes':os.path.getsize(data_dir+'/wal.log') if os.path.exists(data_dir+'/wal.log') else 0}
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--binary',required=True);ap.add_argument('--data-dir',required=True);ap.add_argument('--rows',type=int,default=100000);ap.add_argument('--dim',type=int,default=1024);ap.add_argument('--batch',type=int,default=256);ap.add_argument('--queries',type=int,default=1024);ap.add_argument('--runs',type=int,default=1);a=ap.parse_args()
 for i in range(a.runs):
  d=a.data_dir+'-'+str(i+1);print(json.dumps(run(a.binary,d,a.rows,a.dim,a.batch,a.queries),ensure_ascii=False),flush=True)
if __name__=='__main__':main()
