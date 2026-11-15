// Link with --wrap=write --wrap=fsync --wrap=rename. Faults affect real files;
// every case is checked by a separate restarted process.
#include <cerrno>
#include <cstdlib>
#include <string>
#include <unistd.h>
static std::string fault;
static bool published=false,injected=false;
static int skip_publications=0;
static bool skip_directory_sync=false;
static std::string fdpath(int fd){char p[4096];std::string n="/proc/self/fd/"+std::to_string(fd);ssize_t z=readlink(n.c_str(),p,sizeof(p));return z<0?"":std::string(p,(size_t)z);}
static bool ends(const std::string&s,const std::string&v){return s.size()>=v.size()&&s.compare(s.size()-v.size(),v.size(),v)==0;}
static bool fail(const std::string&point){if(fault!=point)return false;injected=true;errno=EIO;return true;}
extern "C" ssize_t __real_write(int,const void*,size_t);
extern "C" int __real_fsync(int);
extern "C" int __real_rename(const char*,const char*);
extern "C" ssize_t __wrap_write(int fd,const void*p,size_t n){
  if(!fault.empty()&&!skip_publications){auto s=fdpath(fd);if((ends(s,".sst")&&fail("sst_write"))||(ends(s,".manifest")&&fail("manifest_write")))return -1;}
  return __real_write(fd,p,n);
}
extern "C" int __wrap_fsync(int fd){
  if(skip_directory_sync){skip_directory_sync=false;return __real_fsync(fd);}
  if(!fault.empty()&&!skip_publications){auto s=fdpath(fd);
    if((ends(s,".sst")&&fail("sst_sync"))||(ends(s,".manifest")&&fail("manifest_sync"))||(ends(s,".wal")&&fail("wal_rotation_sync"))||(ends(s,".vectors")&&fail("gc_vector_sync")))return -1;
    if(!ends(s,".sst")&&!ends(s,".manifest")&&!ends(s,".wal")&&!ends(s,".vectors")&&(fail(published?"dir_after":"dir_before")))return -1;
  }
  return __real_fsync(fd);
}
extern "C" int __wrap_rename(const char*a,const char*b){
  if(ends(b,"LSM-MANIFEST")){
    if(skip_publications){--skip_publications;skip_directory_sync=true;return __real_rename(a,b);}
    if(fail("manifest_rename"))return -1;
    if(fault=="crash_before")std::_Exit(91);
    int rc=__real_rename(a,b);if(!rc)published=true;
    if(!rc&&fault=="crash_after")std::_Exit(92);return rc;
  }
  return __real_rename(a,b);
}
#define main lite_runtime_main
#include "../src/minivectordb_lite.cpp"
#undef main
static void check(bool ok,const char*why){if(!ok)throw std::runtime_error(why);}
static void verify(lite::Engine&e){
  auto r=e.exec("SELECT id,tag,v,tail FROM t ORDER BY id;").o.at("rows").a;
  check(r.size()==2,"row count changed across publication");
  check(r[0].a[0].i==1&&r[0].a[1].s=="new"&&r[0].a[2].a[0].d==9&&r[0].a[3].s=="last-field","updated row lost");
  check(r[1].a[0].i==2&&r[1].a[1].s=="reinsert"&&r[1].a[2].a[0].d==7&&r[1].a[3].s=="tail","delete/reinsert version lost");
}
int main(int argc,char**argv){
  if(argc!=4)return 2;
  try{std::string mode=argv[2],operation=argv[3];setenv("LITE_ROW_CACHE_MB","0",1);
    if(mode=="replay_crash"){setenv("LITE_MEMTABLE_BYTES","8192",1);fault="crash_after";}
    lite::Engine e(argv[1]);
    if(mode=="inspect_large"){
      auto rows=e.exec("SELECT id,tag,v,tail FROM t ORDER BY id;").o.at("rows").a;
      check(rows.size()==2002,"partial apply/replay lost committed records");
      for(size_t i=2;i<rows.size();++i){auto&r=rows[i].a;check(r[0].i==(long long)i+1&&r[1].s=="large"&&r[2].a[0].d==1&&r[3].s=="tail","partial recovery row mismatch");}
      std::cout<<"PASS full transaction after partial apply and replay crashes\n";return 0;
    }
    if(mode=="inspect"){verify(e);std::cout<<"PASS recovered data\n";return 0;}
    e.exec("CREATE TABLE t (id BIGINT PRIMARY KEY,tag VARCHAR(64),v VECTOR(2),tail VARCHAR(64));");
    e.exec("INSERT INTO t VALUES (1,'old','[1,2]','first'),(2,'remove','[2,3]','last');");e.checkpoint();
    e.exec("BEGIN;");e.exec("UPDATE t SET tag='intermediate',v='[5,6]',tail='middle' WHERE id=1;");
    e.exec("UPDATE t SET tag='new',v='[9,10]',tail='last-field' WHERE id=1;");
    e.exec("DELETE FROM t WHERE id=2;");e.exec("INSERT INTO t VALUES (2,'reinsert','[7,8]','tail');");
    e.exec("COMMIT;");verify(e);
    if(mode=="named_primary_cache"){
      setenv("LITE_ROW_CACHE_MB","32",1);
      e.exec("CREATE TABLE named (pk BIGINT PRIMARY KEY,n BIGINT);");
      e.exec("INSERT INTO named VALUES (4,10),(2,20),(9,30);");
      auto r=e.exec("SELECT pk FROM named WHERE pk=2;");
      check(r.o.at("rows").a.size()==1&&r.o.at("metrics").o.at("candidate_count").i==1,"named primary index not used");
      verify(e);for(auto&x:e.sindex)check(x.first.find("named.")!=0,"evicted table retained scalar postings");
      e.checkpoint();std::cout<<"PASS named primary key and bounded cache eviction\n";return 0;
    }
    if(mode=="partial_apply"){
      e.flush_sst();e.store->limit=8192;std::string sql="INSERT INTO t VALUES ";
      for(int i=3;i<=2002;++i){if(i>3)sql+=',';sql+='('+std::to_string(i)+",'large','[1,2]','tail')";}
      fault="crash_after";e.exec(sql);throw std::runtime_error("partial apply did not reach flush");
    }
    if(mode=="pins"){
      e.flush_sst();auto old=e.store->files;auto cursor=e.store->scan();e.compact();
      for(auto&f:old)check(lite::lsm::exists(f->path),"SST retired while referenced");
      size_t n=0;while(cursor->valid){++n;cursor->next();}check(n>2,"pinned iterator lost data");
      std::vector<std::string>paths;for(auto&f:old)paths.push_back(f->path);old.clear();cursor.reset();
      for(auto&p:paths)check(!lite::lsm::exists(p),"unreferenced SST not reclaimed");
      verify(e);e.checkpoint();std::cout<<"PASS version pins and streaming cursor\n";return 0;
    }
    if(mode=="lock"){
      bool rejected=false;try{lite::Engine second(argv[1]);}catch(const lite::Err&){rejected=true;}check(rejected,"two writable engines opened the same directory");e.checkpoint();std::cout<<"PASS writer lock\n";return 0;
    }
    // Prepare durable rows so rotation/GC tests fail in their own publication,
    // rather than the prerequisite MemTable flush.
    if(operation!="flush")e.flush_sst();
    if(operation=="gc"){
      // gc() starts with a checkpoint. Arm only after that checkpoint for tests
      // of the vector rewrite by using a dedicated already-checkpointed helper.
      e.checkpoint();
    }
    fault=mode;published=false;skip_publications=operation=="gc"?1:0;bool rejected=false;
    try{if(operation=="flush")e.flush_sst();else if(operation=="compact")e.compact();else if(operation=="checkpoint")e.checkpoint();else if(operation=="gc")e.gc();else throw std::runtime_error("operation");}
    catch(const lite::Err&){rejected=true;}
    fault.clear();check(rejected==injected,"fault point was not reached or unexpected failure");
    if(mode!="ok")check(injected,"fault injection not reached");
    if(rejected)check(e.failed_io||e.store->failed,"publication failure did not freeze engine");else verify(e);
    std::cout<<"PASS "<<operation<<" "<<mode<<'\n';return 0;
  }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
