// Diagnostic only. Fault wrappers exercise the actual storage implementation.
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <unistd.h>
static std::string fault;
static bool armed=false,injected=false,wal_durable=false,order_ok=true;
static int vector_syncs=0,wal_syncs=0,vector_writes=0,wal_writes=0,redo_calls=0;
static std::string pathof(int fd){char p[4096];std::string name="/proc/self/fd/"+std::to_string(fd);ssize_t n=readlink(name.c_str(),p,sizeof(p));return n<0?std::string():std::string(p,n);}
static bool ends(const std::string&s,const std::string&t){return s.size()>=t.size()&&s.compare(s.size()-t.size(),t.size(),t)==0;}
static bool isvector(int fd){auto s=pathof(fd);return ends(s,".f32")||ends(s,".vectors");}
static bool iswal(int fd){return ends(pathof(fd),".wal");}
extern "C" ssize_t __real_write(int,const void*,size_t);
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" int __real_fdatasync(int);
extern "C" ssize_t __wrap_write(int fd,const void*p,size_t n){
  if(armed){std::string kind=isvector(fd)?"vector":iswal(fd)?"wal":"other";if(kind=="vector")++vector_writes;if(kind=="wal")++wal_writes;
    if(fault==kind+"_eintr"&&!injected){injected=true;errno=EINTR;return -1;}
    if(fault==kind+"_short")return __real_write(fd,p,std::min<size_t>(n,7));
    if(fault==kind+"_eio"||fault==kind+"_enospc"){if(!injected){injected=true;return __real_write(fd,p,std::max<size_t>(1,n/2));}errno=ends(fault,"enospc")?ENOSPC:EIO;return -1;}
  }return __real_write(fd,p,n);
}
extern "C" ssize_t __wrap_pwrite(int fd,const void*p,size_t n,off_t off){
  if(armed&&isvector(fd)){
    ++redo_calls;
    if(fault=="redo_eintr"&&!injected){injected=true;errno=EINTR;return -1;}
    if(fault=="redo_short")return __real_pwrite(fd,p,std::min<size_t>(n,3),off);
    if(fault=="redo_eio"||fault=="redo_enospc"){if(!injected){injected=true;return __real_pwrite(fd,p,std::min<size_t>(n,5),off);}errno=ends(fault,"enospc")?ENOSPC:EIO;return -1;}
    if(fault=="redo_crash"){__real_pwrite(fd,p,std::min<size_t>(n,5),off);std::_Exit(94);}
    if(fault=="redo_crash_later"&&redo_calls==3){__real_pwrite(fd,p,std::min<size_t>(n,5),off);std::_Exit(94);}
  }return __real_pwrite(fd,p,n,off);
}
extern "C" int __wrap_fdatasync(int fd){
  bool vec=isvector(fd),wal=iswal(fd);
  if(armed){
    if(wal&&fault=="sync_eintr"&&!injected){injected=true;errno=EINTR;return -1;}
    if((vec&&fault=="vector_sync_eio")||(wal&&fault=="wal_sync_eio")||(vec&&fault=="vector_sync_enospc")||(wal&&fault=="wal_sync_enospc")){injected=true;errno=ends(fault,"enospc")?ENOSPC:EIO;return -1;}
  }
  int rc=__real_fdatasync(fd);
  if(armed&&rc==0){
    if(wal){++wal_syncs;wal_durable=true;if(fault=="after_wal")std::_Exit(92);}
    if(vec){++vector_syncs;if(!wal_durable)order_ok=false;if(fault=="after_vector")std::_Exit(93);}
  }return rc;
}
#define main lite_runtime_main
#include "../src/minivectordb_lite.cpp"
#undef main
static void require(bool ok,const char*why){if(!ok)throw std::runtime_error(why);}
static void verify(lite::Engine&e,bool changed){
  auto rows=e.exec("SELECT id,tag,v,tail FROM t ORDER BY id;").o.at("rows").a;
  require(rows.size()==(changed?2:1),"transaction is incomplete or uncommitted row leaked");
  for(size_t i=0;i<rows.size();++i){auto&a=rows[i].a;long long id=(long long)i+1;require(a[0].i==id,"id mismatch");require(a[1].s==(changed?"new":"old"),"tag mismatch");require(a[3].s=="last-field","last field mismatch");
    for(int d=0;d<4;++d)require(a[2].a[d].d==double(changed?10*id+d+1:d+1),"vector bytes were not restored");
  }
}
static void mutate(lite::Engine&e){
  e.exec("BEGIN;");e.exec("UPDATE t SET tag='new',v='[11,12,13,14]' WHERE id=1;");
  e.exec("INSERT INTO t VALUES (2,'new','[21,22,23,24]','last-field'),(3,'dead','[31,32,33,34]','last-field');");
  e.exec("DELETE FROM t WHERE id=3;");e.exec("COMMIT;");
}
static void metadata(lite::Engine&e){std::cout<<"{\"vector\":\""<<e.store->vector_name<<"\",\"wal\":\""<<e.store->wal_name<<"\",\"end\":"<<e.store->vector_end<<",\"durable_end\":"<<e.store->vector_synced<<",\"vector_syncs\":"<<vector_syncs<<",\"wal_syncs\":"<<wal_syncs<<",\"vector_writes\":"<<vector_writes<<",\"wal_writes\":"<<wal_writes<<"}\n"<<std::flush;}
int main(int argc,char**argv){
  if(argc!=3)return 2;
  try{
    std::string mode=argv[2];setenv("LITE_COMMIT_MODE","wal",1);
    if(mode.compare(0,5,"redo_")==0){fault=mode;armed=true;}
    lite::Engine e(argv[1]);
    if(mode=="chunk_seed"||mode=="chunk_inspect"||mode=="redo_crash_later"){
      const int rows=4096,dim=128;
      if(mode=="chunk_seed"){
        e.exec("CREATE TABLE chunks (id BIGINT PRIMARY KEY,v VECTOR(128),tail VARCHAR(32));");e.checkpoint();
        std::string sql="INSERT INTO chunks VALUES ";
        for(int i=0;i<rows;++i){if(i)sql+=',';sql+='('+std::to_string(i)+",'[";for(int j=0;j<dim;++j){if(j)sql+=',';sql+=std::to_string(i%997);}sql+="]','tail-"+std::to_string(i)+"')";}
        e.exec(sql+";");require(e.store->vector_synced<e.store->vector_end,"fixture unexpectedly flushed its vector tail");
      }else{
        for(int begin=0;begin<rows;begin+=256){auto found=e.exec("SELECT id,v,tail FROM chunks WHERE id>="+std::to_string(begin)+" AND id<"+std::to_string(begin+256)+" ORDER BY id;").o.at("rows").a;require(found.size()==256,"chunk row count");
          for(int i=0;i<256;++i){auto&r=found[i].a;int id=begin+i;require(r[0].i==id&&r[2].s=="tail-"+std::to_string(id),"chunk row metadata");for(auto&x:r[1].a)require(x.d==double(id%997),"chunk vector content");require(r[1].a.size()==dim,"chunk dimensions");}}
      }metadata(e);return 0;
    }
    if(mode=="inspect_new"||mode=="inspect_old"||mode.compare(0,5,"redo_")==0){verify(e,mode!="inspect_old");metadata(e);return 0;}
    e.exec("CREATE TABLE t (id BIGINT PRIMARY KEY,tag VARCHAR(32),v VECTOR(4),tail VARCHAR(64));");
    e.exec("INSERT INTO t VALUES (1,'old','[1,2,3,4]','last-field');");e.checkpoint();
    if(mode=="seed_gc")e.gc();
    bool maintenance=mode.compare(0,11,"checkpoint_")==0||mode.compare(0,6,"flush_")==0;
    if(mode=="auto_sync_eio"||mode=="auto_sync_enospc"||mode=="auto_crash")e.store->limit=1;
    if(maintenance){mutate(e);wal_durable=true;fault=mode.substr(mode.find('_')+1);}
    else if(mode=="auto_sync_eio")fault="vector_sync_eio";
    else if(mode=="auto_sync_enospc")fault="vector_sync_enospc";
    else if(mode=="auto_crash")fault="after_vector";
    else if(mode!="seed"&&mode!="seed_gc"&&mode!="uncommitted"&&mode!="scalar")fault=mode;
    armed=true;bool failed=false;
    try{if(maintenance){if(mode.compare(0,11,"checkpoint_")==0)e.checkpoint();else e.flush_sst();}
      else if(mode=="scalar")e.exec("UPDATE t SET tag='old' WHERE id=1;");else mutate(e);
    }catch(const lite::Err&){failed=true;}
    bool expected_error=ends(fault,"_eio")||ends(fault,"_enospc");require(failed==expected_error,"unexpected error outcome");
    require(order_ok,"vector publication barrier preceded durable WAL");
    if(failed){require(e.failed_io||e.store->failed,"I/O error did not freeze engine");for(auto sql:{"SELECT id FROM t;","ROLLBACK;","COMMIT;"}){bool blocked=false;try{e.exec(sql);}catch(const lite::Err&){blocked=true;}require(blocked,"failed engine still accepts requests");}}
    else if(!maintenance){require(wal_syncs==1&&vector_syncs==0,"ordinary commit did not use only WAL sync");}
    armed=false;fault.clear();
    if(mode=="uncommitted"){e.exec("BEGIN;");e.exec("INSERT INTO t VALUES (999,'never','[99,98,97,96]','last-field');");e.flush_vecbuf();}
    if(!failed&&mode!="uncommitted")verify(e,mode!="scalar");metadata(e);return 0;
  }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
