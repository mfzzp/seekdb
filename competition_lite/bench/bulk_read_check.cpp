#define main lite_runtime_main
#include "../src/minivectordb_lite.cpp"
#undef main
static void require(bool ok,const char*why){if(!ok)throw std::runtime_error(why);}
int main(int argc,char**argv){
 try{
  if(argc!=2)return 2;setenv("LITE_ROW_CACHE_MB","0",1);setenv("LITE_READ_CACHE_MB","0",1);setenv("LITE_MEMTABLE_BYTES","32768",1);setenv("LITE_SST_BYTES","65536",1);
  lite::Engine e(argv[1]);e.exec("CREATE TABLE t (id BIGINT PRIMARY KEY,tag VARCHAR(32),v VECTOR(2));");
  for(int batch=0;batch<30;++batch){std::string sql="INSERT INTO t VALUES ";for(int j=0;j<100;++j){int i=batch*100+j;if(j)sql+=',';sql+='('+std::to_string(i)+",'"+(i%2?"odd":"even")+"','["+std::to_string(i%13)+",2]')";}e.exec(sql);}
  e.flush_sst();e.exec("UPDATE t SET tag='even',v='[9,2]' WHERE id<50;");e.flush_sst();e.exec("DELETE FROM t WHERE id>=100 AND id<200;");e.exec("BEGIN;");e.exec("UPDATE t SET v='[0,0]' WHERE id=1;");e.exec("DELETE FROM t WHERE id=2;");
  std::vector<std::string> keys;for(int i=0;i<300;i+=2)keys.push_back(e.skey(e.tabs.at("t"),i));keys.push_back(e.skey(e.tabs.at("t"),1));keys.push_back(e.skey(e.tabs.at("t"),1));
  auto before=*e.store->read_cache->stats;auto cursor=e.rows_get(keys);size_t count=0;
  while(cursor->valid){auto row=e.decode_row(e.tabs.at("t"),cursor->row.value);require(row.id!=2&&(row.id<100||row.id>=200),"deleted version returned");if(row.id==1)require(e.read_vec(row.voff[2],2)==std::vector<float>({0,0}),"private vector missing");++count;cursor->next();}
  require(count==100,"missing keys or duplicate keys in merged batch");
  require(e.store->read_cache->stats->readers-before.readers<=e.store->files.size()+e.tx->runs.size(),"batch initialized a reader for every candidate");
  auto verify=[&](){
   for(auto filter:{"tag='even'","tag='odd'","tag='even' AND id>=200"}){
    std::string sql="SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t WHERE "+std::string(filter)+" ORDER BY d LIMIT 40;";
    setenv("LITE_LSM_READ","scan",1);auto truth=lite::dump(e.exec(sql).o.at("rows"));
    for(auto mode:{"batch","auto"}){setenv("LITE_LSM_READ",mode,1);require(lite::dump(e.exec(sql).o.at("rows"))==truth,"batch/scan result mismatch");}
   }
  };
  verify();e.rollback();verify();e.compact();verify();e.gc();verify();e.checkpoint();
  require(e.store->read_cache->used==0,"disabled cache retained allocations");
  std::cout<<"PASS ordered batches, duplicate/missing keys, tombstones, private overlays, rollback, compaction, GC, cache off\n";
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
