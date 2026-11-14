// Diagnostic executable only: link with --wrap=write and --wrap=fdatasync.
#include <cerrno>
#include <cstdlib>
#include <string>
#include <unistd.h>
static std::string fault;
static int vector_fd=-1,wal_fd=-1,vector_syncs=0,wal_syncs=0,writes=0;
static bool injected=false,vector_synced=false,order_ok=true;
extern "C" ssize_t __real_write(int,const void*,size_t);
extern "C" int __real_fdatasync(int);
extern "C" ssize_t __wrap_write(int fd,const void*p,size_t n){
  if(!fault.empty()&&(fd==vector_fd||fd==wal_fd)){
    ++writes;
    if(fd==wal_fd&&!vector_synced)order_ok=false;
    if(fault=="eintr"&&!injected){injected=true;errno=EINTR;return -1;}
    if((fault=="partial_vector"&&fd==vector_fd)||(fault=="partial_wal"&&fd==wal_fd)){
      if(injected){errno=EIO;return -1;}injected=true;return __real_write(fd,p,n/2);
    }
    if(fault=="short")return __real_write(fd,p,std::min<size_t>(n,7));
  }
  return __real_write(fd,p,n);
}
extern "C" int __wrap_fdatasync(int fd){
  if(!fault.empty()){
    if((fault=="vector_sync"&&fd==vector_fd)||(fault=="wal_sync"&&fd==wal_fd)){errno=EIO;return -1;}
    if(fault=="sync_eintr"&&!injected){injected=true;errno=EINTR;return -1;}
  }
  int rc=__real_fdatasync(fd);
  if(!fault.empty()&&rc==0){
    if(fd==vector_fd){++vector_syncs;vector_synced=true;if(fault=="crash_vector")std::_Exit(91);}
    if(fd==wal_fd){++wal_syncs;if(fault=="crash_wal")std::_Exit(92);}
  }
  return rc;
}
#define main lite_runtime_main
#include "../src/minivectordb_lite.cpp"
#undef main
static void require(bool ok,const char*why){if(!ok)throw std::runtime_error(why);}
static uint32_t reference_crc(const char*p,size_t n){uint32_t c=~0u;for(size_t i=0;i<n;++i){c^=(unsigned char)p[i];for(int j=0;j<8;++j)c=(c>>1)^((c&1)?0x82f63b78u:0u);}return ~c;}
static void crc_check(){
  require(lite::crc32c("123456789",9)==0xe3069283u,"CRC known answer");
  std::string data(1048600,'\0');uint32_t x=20261114;
  for(char&c:data){x=x*1664525u+1013904223u;c=(char)(x>>24);}
  for(size_t off=0;off<16;++off)for(size_t len:{0,1,7,8,9,31,64,65,511,4096,1048576}){
    uint32_t want=reference_crc(data.data()+off,len);
    require(lite::crc32c_portable(data.data()+off,len)==want,"portable CRC mismatch");
    require(lite::crc32c(data.data()+off,len)==want,"hardware CRC mismatch");
  }
}
int main(int argc,char**argv){
  if(argc!=3)return 2;
  try{
    std::string mode=argv[2];
    if(mode=="crc"){crc_check();std::cout<<"PASS CRC 176 buffers and known answer\n";return 0;}
    lite::Engine e(argv[1]);
    if(mode=="inspect"){
      auto result=e.exec("SELECT id,tag,v FROM docs ORDER BY id;").o.at("rows");
      require(result.a.size()==1||result.a.size()==3,"partial committed transaction");
      for(size_t i=0;i<result.a.size();++i){auto&r=result.a[i].a;require(r[0].i==(long long)i+1&&r[1].s=="it'works","lost row or literal");
        require(r[2].a[0].d==double(i+1)&&r[2].a[1].d==double(i+2),"vector mismatch");}
      std::cout<<"PASS restart rows="<<result.a.size()<<'\n';return 0;
    }
    e.exec("CREATE TABLE docs (id BIGINT PRIMARY KEY,tag VARCHAR(32),v VECTOR(2));");
    e.exec("INSERT INTO docs VALUES (1,'it''works','[1,2]');");
    if(mode=="statement_undo"){
      e.exec("BEGIN;");e.exec("INSERT INTO docs VALUES (2,'it''works','[2,3]');");
      bool rejected=false;try{e.exec("INSERT INTO docs VALUES (3,'bad','[3,4]'),(4,'bad','[4]');");}catch(const lite::Err&){rejected=true;}
      require(rejected&&e.tabs.at("docs").r.size()==2,"failed statement leaked rows");
      require(e.in,"failed statement ended explicit transaction");
      e.exec("INSERT INTO docs VALUES (3,'it''works','[3,4]');");e.exec("COMMIT;");
      size_t bytes=0;for(auto&r:e.memtable)bytes+=r.key.size()+r.value.size()+32;
      require(e.memtable_bytes==bytes,"MemTable accounting mismatch");
      require(e.wal_bytes==e.fsize(e.wal),"WAL byte accounting mismatch");
      e.flush_sst();require(e.memtable_bytes==0,"MemTable counter not reset");
      e.checkpoint();require(e.wal_bytes==0&&e.fsize(e.wal)==0,"WAL counter not reset");
      std::cout<<"PASS statement undo and MemTable bytes\n";return 0;
    }
    vector_fd=e.vfd;wal_fd=e.wfd;
    if(mode=="lsm_failure"){setenv("LITE_MEMTABLE_BYTES","1",1);mkdir((e.dir+"/MANIFEST.tmp").c_str(),0755);}
    fault=mode;bool failed=false;
    try{e.exec("INSERT INTO docs VALUES (2,'it''works','[2,3]'),(3,'it''works','[3,4]');");}catch(const lite::Err&){failed=true;}
    bool expect_failure=mode=="partial_vector"||mode=="partial_wal"||mode=="vector_sync"||mode=="wal_sync"||mode=="lsm_failure";
    require(failed==expect_failure,"unexpected commit outcome");
    require(order_ok,"WAL published before vector sync");
    if(failed){
      require(e.failed_io,"failed engine not frozen");
      for(auto sql:{"SELECT id FROM docs;","ROLLBACK;","COMMIT;"}){bool rejected=false;try{e.exec(sql);}catch(const lite::Err&){rejected=true;}require(rejected,"request accepted after I/O failure");}
      bool rejected=false;try{e.checkpoint();}catch(const lite::Err&){rejected=true;}require(rejected,"checkpoint accepted after I/O failure");
    }else require(vector_syncs==1&&wal_syncs==1,"commit did not perform two successful syncs");
    fault.clear();std::cout<<"PASS "<<mode<<" vector_syncs="<<vector_syncs<<" wal_syncs="<<wal_syncs<<" writes="<<writes<<'\n';
  }catch(const std::exception&ex){std::cerr<<ex.what()<<'\n';return 1;}
}
