#define main lite_runtime_main
#include "../src/minivectordb_lite.cpp"
#undef main
static void require(bool ok,const char*why){if(!ok)throw std::runtime_error(why);}
int main(int argc,char**argv){
 try{
  if(argc!=2)return 2;setenv("LITE_ROW_CACHE_MB","0",1);setenv("LITE_QUERY_VIEW_MB","1",1);setenv("LITE_PROFILE","1",1);setenv("LITE_MEMTABLE_BYTES","32768",1);setenv("LITE_SST_BYTES","65536",1);
  lite::Engine e(argv[1]);
  for(auto name:{"a","b"}){e.exec("CREATE TABLE "+std::string(name)+" (pk BIGINT PRIMARY KEY,tag VARCHAR(32),content VARCHAR(64),v VECTOR(2),w VECTOR(3));");
   for(int batch=0;batch<15;++batch){std::string sql="INSERT INTO "+std::string(name)+" VALUES ";for(int j=0;j<100;++j){int i=batch*100+j-700;if(j)sql+=',';sql+='('+std::to_string(i)+",'"+(i%2?"odd":"even")+"','"+(i%3?"keep":"skip")+"','["+std::to_string(i%13)+",2]','[1,2,3]')";}e.exec(sql);}}
  e.flush_sst();size_t checked=0;
  auto verify=[&](){for(auto name:{"a","b"})for(auto col:{"v","w"})for(auto predicate:{""," WHERE tag='even' AND pk>=-100"," WHERE tag='odd' AND content!='skip'"," WHERE pk<5 AND pk>=-10"})for(auto direction:{"ASC","DESC"}){
   std::string sql="SELECT pk,content,"+std::string(col)+",L2_DISTANCE("+col+",'"+(std::string(col)=="v"?"[0,0]":"[0,0,0]")+"') AS d FROM "+name+predicate+" ORDER BY d "+direction+" LIMIT 10;";
   setenv("LITE_QUERY_VIEW_MB","1",1);auto fast=e.exec(sql);setenv("LITE_QUERY_VIEW_MB","0",1);auto slow=e.exec(sql);
   require(lite::dump(fast.o.at("rows"))==lite::dump(slow.o.at("rows")),"compact view result mismatch");require(e.query_view_bytes()==0,"disabled compact cache retained rows");++checked;
  }setenv("LITE_QUERY_VIEW_MB","1",1);e.exec("SELECT pk,L2_DISTANCE(v,'[0,0]') AS d FROM a ORDER BY d LIMIT 10;");};
  verify();
  auto warm="SELECT pk,L2_DISTANCE(v,'[0,0]') AS d FROM a WHERE tag='even' ORDER BY d LIMIT 10;";
  auto metric=e.exec(warm).o.at("metrics");require(metric.o.at("lsm_access").s=="compact"&&metric.o.at("full_row_decodes").i==0,"compact path decoded full rows");
  e.exec("BEGIN;");e.exec("UPDATE a SET tag='even',v='[0,0]',content='changed' WHERE pk=-699;");e.exec("DELETE FROM a WHERE pk=-700;");verify();e.rollback();verify();
  e.exec(warm);try{e.exec("INSERT INTO a VALUES (9000,'even','x','[0,0]','[1,2,3]'),(0,'odd','duplicate','[0,0]','[1,2,3]');");throw std::runtime_error("duplicate insert accepted");}catch(const lite::Err&x){require(x.code=="duplicate_key","unexpected insert error");}verify();
  e.exec("UPDATE a SET v='[0,0]' WHERE pk=1;");e.exec("DELETE FROM b WHERE pk>=700;");verify();e.compact();verify();e.gc();verify();e.checkpoint();
  // Force a descriptor view beyond 1 MiB without allocating another row table.
  auto&q=e.tabs.at("a");auto count=q.count;q.count=70000;require(e.query_view(q,3)==nullptr,"oversized descriptor view admitted");q.count=count;
  require(e.query_view_bytes()<=(1u<<20),"descriptor budget exceeded");
  std::cout<<"PASS "<<checked<<" compact/stream comparisons, projection, residual filter, signed keys, DESC ties, two vector columns/tables, invalidation, budget\n";
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
