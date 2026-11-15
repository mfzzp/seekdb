// Cache lifetime and eligibility checks complement the independent exact oracles.
#define main lite_runtime_main
#include "../src/minivectordb_lite.cpp"
#undef main
static void require(bool ok,const char*why){if(!ok)throw std::runtime_error(why);}
int main(int argc,char**argv){try{
  if(argc!=2)return 2;setenv("LITE_PROFILE","1",1);setenv("LITE_EXACT_PATH","scan",1);
  lite::Engine e(argv[1]);size_t checks=0;
  for(auto table:{"a","b"}){
    e.exec("CREATE TABLE "+std::string(table)+" (pk BIGINT PRIMARY KEY,tag BIGINT,label VARCHAR(64),v VECTOR(3));");
    std::string sql="INSERT INTO "+std::string(table)+" VALUES ";
    for(int i=0;i<257;++i){if(i)sql+=',';int id=(i*137)%257-128;sql+='('+std::to_string(id)+","+std::to_string((id+129)%5)+",'"+std::to_string(id%3)+"','["+std::to_string(id%7)+",0,1]')";}e.exec(sql);
  }
  auto compare=[&](const std::string&sql,bool direct=true){
    setenv("LITE_HOT_POSTINGS_KB","0",1);auto slow=e.exec(sql);
    setenv("LITE_HOT_POSTINGS_KB","1024",1);auto fast=e.exec(sql);auto again=e.exec(sql);
    require(lite::dump(slow.o.at("rows"))==lite::dump(fast.o.at("rows")),"cached candidates differ");
    require(lite::dump(again.o.at("rows"))==lite::dump(fast.o.at("rows")),"reused candidates differ");
    require(fast.o.at("metrics").o.at("hot_postings").b==direct,"wrong eligibility");
    require(again.o.at("metrics").o.at("hot_postings_builds").i==0,"readonly postings rebuilt");++checks;
  };
  auto verify=[&](){for(auto table:{"a","b"})for(auto where:{"tag=+01","label='1'","tag=9","pk=-128","tag=2 AND pk<0"})for(int k:{1,10,300})
    compare("SELECT label,pk,v,L2_DISTANCE(v,'[0,0,0]') AS d FROM "+std::string(table)+" WHERE "+where+" ORDER BY d LIMIT "+std::to_string(k)+";",std::string(where).find("AND")==std::string::npos);};
  verify();auto warm="SELECT pk,L2_DISTANCE(v,'[0,0,0]') AS d FROM a WHERE tag=1 ORDER BY d LIMIT 10;";
  e.exec(warm);e.exec("BEGIN;");e.exec("UPDATE a SET tag=1,label='changed',v='[0,0,0]' WHERE pk=0;");e.exec("DELETE FROM b WHERE pk<0;");verify();e.rollback();verify();
  e.exec(warm);try{e.exec("INSERT INTO a VALUES (999,1,'new','[0,0,0]'),(0,1,'duplicate','[0,0,0]');");throw std::runtime_error("duplicate accepted");}catch(const lite::Err&x){require(x.code=="duplicate_key","wrong insert error");}verify();
  e.exec("UPDATE a SET v='[1,1,1]',tag=1 WHERE pk=-128;");e.exec("DELETE FROM a WHERE pk=127;");verify();e.compact();e.gc();verify();
  setenv("LITE_HOT_POSTINGS_KB","1",1);auto small=e.exec(warm);require(!small.o.at("metrics").o.at("hot_postings").b,"oversized postings retained");
  auto again=e.exec(warm);require(again.o.at("metrics").o.at("hot_postings_builds").i==0,"rejected postings repeatedly rebuilt");require(e.hot_posts.slots.empty()&&e.hot_posts.bytes==0,"rejected allocation retained");
  compare("SELECT pk,L2_DISTANCE(v,'[0,0,0]') AS d FROM a WHERE tag=1 ORDER BY d LIMIT 0;",false);
  compare("SELECT pk,L2_DISTANCE(v,'[0,0,0]') AS d FROM a WHERE tag=1 ORDER BY d DESC LIMIT 10;",false);
  e.checkpoint();std::cout<<"PASS hot postings comparisons="<<checks<<" private writes, rollback, failure, GC, column/table switch, budget fallback\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
