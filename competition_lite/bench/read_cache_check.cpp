#define main lite_runtime_main
#include "../src/minivectordb_lite.cpp"
#undef main
static void need(bool value,const char*why){if(!value)throw std::runtime_error(why);}
int main(int argc,char**argv){
  try{
    if(argc!=2)return 2;
    setenv("LITE_ROW_CACHE_MB","0",1);setenv("LITE_READ_CACHE_MB","1",1);
    lite::Engine e(argv[1]);
    e.exec("CREATE TABLE t (id BIGINT PRIMARY KEY,tag VARCHAR(32),v VECTOR(2));");
    std::string sql="INSERT INTO t VALUES ";
    for(int i=0;i<1800;++i){if(i)sql+=',';sql+='('+std::to_string(i)+",'same','[1,2]')";}
    e.exec(sql);e.flush_sst();
    auto cache=e.store->read_cache;auto before=*cache->stats;
    for(int i=0;i<250;++i){lite::Row row;need(e.row_get(e.tabs.at("t"),i,row)&&row.id==i,"point lookup");}
    need(cache->stats->readers-before.readers<10,"reader per key");
    need(cache->stats->blocks-before.blocks<10,"block decoded per key");
    need(cache->used<=cache->limit,"retained budget");
    auto old=e.store->files;auto cursor=e.store->scan();e.compact();
    for(auto&f:old)need(lite::lsm::exists(f->path),"active iterator lost old SST");
    while(cursor->valid)cursor->next();cursor.reset();
    std::vector<std::string> paths;for(auto&f:old)paths.push_back(f->path);old.clear();
    for(auto&p:paths)need(!lite::lsm::exists(p),"cache prevented retired file deletion");
    auto query="SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t WHERE tag='same' ORDER BY d LIMIT 10;";
    auto result=e.exec(query).o.at("rows");setenv("LITE_LSM_READ","scan",1);
    need(lite::dump(e.exec(query).o.at("rows"))==lite::dump(result),"scan filter differs");
    unsetenv("LITE_LSM_READ");
    // --check must bypass an already warm block; reloading never skips CRC.
    auto f=e.store->files.front();lite::lsm::DiskCursor warm(f);need(warm.valid,"empty SST");
    int fd=open(f->path.c_str(),O_RDWR);need(fd>=0,"test mutation open");char byte=0;need(pread(fd,&byte,1,20)==1,"test read");byte^=64;need(pwrite(fd,&byte,1,20)==1,"test mutation");close(fd);
    bool rejected=false;try{e.store->verify();}catch(const lite::Err&x){rejected=x.code=="corruption";}
    need(rejected,"warm cache hid corruption from verify");cache->retire(f->identity);
    rejected=false;try{lite::lsm::DiskCursor reload(f);}catch(const lite::Err&x){rejected=x.code=="corruption";}
    need(rejected,"cache miss omitted CRC");
    std::cout<<"PASS reader/block reuse, cache budget, file pins, scan fallback, reload CRC\n";
  }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
