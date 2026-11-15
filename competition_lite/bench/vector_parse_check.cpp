// Verify float32 bytes and rejection semantics against the original parser.
#define main lite_runtime_main
#include "../src/minivectordb_lite.cpp"
#undef main
#include <random>
static void require(bool x,const char*s){if(!x)throw std::runtime_error(s);}
struct Parsed {std::string error;std::vector<float> v;};
static Parsed parse(const std::string&s,int dim,bool fast){
  setenv("LITE_FAST_PARSE",fast?"1":"0",1);Parsed p;
  try{lite::Engine::parse_into(s,dim,p.v);}catch(const lite::Err&e){p.error=e.code;}
  return p;
}
int main(){try{
  size_t cases=0,values=0;auto check=[&](const std::string&s,int dim=-1){auto a=parse(s,dim,false),b=parse(s,dim,true);
    require(a.error==b.error,"parser rejection changed");if(a.error.empty()){require(a.v.size()==b.v.size(),"parser dimension changed");require(!std::memcmp(a.v.data(),b.v.data(),a.v.size()*sizeof(float)),"float32 bytes changed");values+=a.v.size();}++cases;};
  for(auto s:{"[]","[0,-0,+0,-0.0,1.,.5,+.5,1e+3,1E-3]","[1e-9999,-1e-9999,1e-324,-1e-324]",
    "[1.0000000596046448,1.0000000596046447,16777217,16777219]","[1.401298464324817e-45,7.006492321624085e-46,-7.006492321624086e-46]",
    "[3.4028234663852886e38]","[3.4028235677973366e38]","[1e9999]","[NaN]","[nan(1)]","[-Inf]","[Infinity]","[0x1p2]",
    "[+-1]","[--1]","[+ 1]","[1e]","[1e-]","[1.2.3]","[1,]","[,1]","[1,,2]","[ ]","[\t-1\n, +2 ]","[1 2]","[1,2]junk"}){
    check(s);check(s,128);check("'"+std::string(s)+"'");}
  std::mt19937_64 rng(701923);
  for(int trial=0;trial<3000;++trial){std::string s="[";
    for(int i=0;i<128;++i){if(i)s+=',';double d=std::ldexp(double(int64_t(rng()%2000001)-1000000),int(rng()%268)-160);char buf[128];
      auto r=std::to_chars(buf,buf+sizeof(buf),d,std::chars_format::general,17);require(r.ec==std::errc{},"number generation failed");
      if(trial%13==0&&d>=0)s+='+';s.append(buf,r.ptr);
    }s+=']';check(s,128);
  }
  for(int exponent=-149;exponent<=127;++exponent){float f=std::ldexp(1.f,exponent),next=std::nextafter(f,INFINITY);double middle=(double(f)+double(next))/2;
    for(double d:{std::nextafter(middle,0.),middle,std::nextafter(middle,INFINITY)}){char buf[128];auto r=std::to_chars(buf,buf+sizeof(buf),d,std::chars_format::general,17);check("["+std::string(buf,r.ptr)+"]",1);}}
  require(parse("[nan]",1,true).error=="invalid_value","NaN admitted");require(parse("[1,2]",1,true).error=="dimension_mismatch","dimension mismatch admitted");
  std::cout<<"PASS vector parse cases="<<cases<<" validated_float32_values="<<values<<" bytes identical, boundaries/underflow/invalid inputs\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
