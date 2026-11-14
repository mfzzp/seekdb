// Build with -Wl,--wrap=rename -Wl,--wrap=fsync. No fault hooks in the runtime.
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
static std::string fault;
static bool published=false;
extern "C" int __real_rename(const char*,const char*);
extern "C" int __real_fsync(int);
extern "C" int __wrap_rename(const char*a,const char*b){
  bool manifest=std::string(b).find("/MANIFEST")!=std::string::npos;
  if(manifest&&fault=="rename"){errno=EIO;return -1;}
  if(manifest&&fault=="crash_before")std::_Exit(91);
  int rc=__real_rename(a,b);
  if(manifest&&rc==0){published=true;if(fault=="crash_after")std::_Exit(92);}
  return rc;
}
extern "C" int __wrap_fsync(int fd){
  struct stat st{};fstat(fd,&st);
  if(fault=="sync_file"&&!S_ISDIR(st.st_mode)){errno=EIO;return -1;}
  if((fault=="sync_dir_before"&&!published||fault=="sync_dir_after"&&published)&&S_ISDIR(st.st_mode)){errno=EIO;return -1;}
  return __real_fsync(fd);
}
#define main lite_runtime_main
#include "../src/minivectordb_lite.cpp"
#undef main
static void require(bool ok,const char*message){if(!ok)throw std::runtime_error(message);}
int main(int argc,char**argv){
  if(argc!=3)return 2;
  try{
    std::string mode=argv[2];lite::Engine e(argv[1]);
    if(mode=="inspect"){
      size_t files=0;
      for(auto&kv:e.levels)for(auto&p:kv.second){
        std::ifstream f(p);require(bool(f),"manifest references missing SST");
        std::string line;require(bool(std::getline(f,line)),"empty test SST");
        lite::SRec rec;require(e.sread(line,rec),"invalid SST");
        require(e.fsize(p+".idx")>0,"missing sidecar");++files;
      }
      require(files==1||files==4,"wrong published view");std::cout<<"PASS inspect files="<<files<<'\n';return 0;
    }
    for(int i=1;i<=4;++i){
      std::string p=e.dir+"/L0-"+std::to_string(i)+".sst";
      std::ofstream f(p);f<<e.sline({"key","value",(uint64_t)i,false})<<'\n';
      e.finish_file(f,p);e.seal_sst(p);e.levels[0].push_back(p);
    }
    e.sst_seq=4;e.write_manifest(e.levels);
    const auto old=e.levels[0];published=false;
    if(mode=="sst_write")mkdir((e.dir+"/L1-5.sst").c_str(),0755);
    if(mode=="manifest_write")mkdir((e.dir+"/MANIFEST.tmp").c_str(),0755);
    fault=mode;bool failed=false;
    try{e.compact();}catch(const lite::Err&){failed=true;}
    fault.clear();
    require(failed==(mode!="ok"),"unexpected publication outcome");
    for(auto&p:old)require((e.fsize(p)>0)==failed,"old file reclaimed before durable publication");
    if(failed&&!published)require(e.levels[0]==old,"old memory view changed on failed publication");
    std::cout<<"PASS "<<mode<<'\n';
  }catch(const std::exception&ex){std::cerr<<ex.what()<<'\n';return 1;}
}
