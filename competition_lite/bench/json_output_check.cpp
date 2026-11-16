#define main lite_runtime_main
#include "../src/minivectordb_lite.cpp"
#undef main
#include <random>
static void require(bool x,const char*s){if(!x)throw std::runtime_error(s);}
int main(){try{
  std::mt19937_64 rng(721309);size_t checked=0,different_text=0;
  for(int i=0;i<100000;++i){uint64_t bits=rng();double d;std::memcpy(&d,&bits,8);if(!std::isfinite(d))continue;
    auto j=lite::jd(d);std::string fast;lite::dump_into(j,fast);auto old=lite::dump_legacy(j);
    char*e=nullptr;double actual=std::strtod(fast.c_str(),&e);require(e==fast.c_str()+fast.size(),"invalid numeric JSON");require(!std::memcmp(&d,&actual,8),"double roundtrip changed");
    require(fast.find_first_of(".eE")!=std::string::npos,"double encoded as integer");different_text+=fast!=old;++checked;
  }
  for(double d:{0.,-0.,1.,-1.,std::numeric_limits<double>::denorm_min(),std::numeric_limits<double>::max()}){std::string s;lite::dump_into(lite::jd(d),s);double x=std::strtod(s.c_str(),nullptr);require(!std::memcmp(&d,&x,8),"boundary roundtrip");}
  lite::J root=lite::jo(),rows=lite::ja();std::string chars;for(int i=0;i<256;++i)chars+=(char)i;
  root.o["id"]=lite::ji(INT64_MIN);root.o["ok"]=lite::jb(true);root.o["escaped\"key"]=lite::js(chars);
  for(int i=0;i<100;++i){auto row=lite::ja();row.a={lite::ji(i),lite::jd(i*.125),lite::js(chars),lite::J{}};rows.a.push_back(row);}root.o["rows"]=rows;
  std::string fast;lite::dump_into(root,fast);require(fast==lite::dump_legacy(root),"nested JSON/escaping changed");
  for(double d:{INFINITY,-INFINITY,NAN}){try{std::string s;lite::dump_into(lite::jd(d),s);throw std::runtime_error("nonfinite accepted");}catch(const lite::Err&e){require(e.code=="invalid_value","wrong nonfinite error");}}
  std::cout<<"PASS JSON finite doubles="<<checked<<" alternate spellings="<<different_text<<" exact roundtrip, types, nested values, escapes, invalid values\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
