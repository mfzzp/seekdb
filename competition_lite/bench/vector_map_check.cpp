#define main lite_runtime_main
#include "../src/minivectordb_lite.cpp"
#undef main
static void need(bool ok,const char*why){if(!ok)throw std::runtime_error(why);}
int main(int argc,char**argv){
 try{
  if(argc!=2)return 2;setenv("LITE_MMAP_MB","1",1);setenv("LITE_MMAP_KEEP_MB","1",1);
  lite::Engine e(argv[1]);e.exec("CREATE TABLE t (id BIGINT PRIMARY KEY,v VECTOR(129));");
  for(int b=0;b<30;++b){std::string sql="INSERT INTO t VALUES ";for(int j=0;j<100;++j){int id=b*100+j;if(j)sql+=',';sql+='('+std::to_string(id)+",'[";for(int d=0;d<129;++d){if(d)sql+=',';sql+=std::to_string(id);}sql+="]')";}e.exec(sql);}
  auto verify=[&](){for(auto id:{0,2032,2999,1,2031,2998}){lite::Row r;need(e.row_get(e.tabs.at("t"),id,r),"missing row");auto v=e.read_vec(r.voff[1],129);need(v==std::vector<float>(129,float(id)),"retained or crossing mapping returned stale vector");}need(e.mapped_bytes()<=(1u<<20)+129*4,"mapping budget");};
  verify();e.exec("BEGIN;");e.exec("UPDATE t SET v='["+std::string("0,")+[](){std::string s;for(int i=1;i<129;++i){if(i>1)s+=',';s+='0';}return s;}()+"]' WHERE id=1;");
  e.exec("SELECT v FROM t WHERE id=1;");e.rollback();verify();e.gc();verify();e.checkpoint();
  setenv("LITE_MMAP_KEEP_MB","0",1);e.clear_maps();verify();
  std::cout<<"PASS retained mapping budget, non-aligned vectors across windows, growth, rollback, GC, retention off\n";
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
