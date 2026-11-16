#define main lite_runtime_main
#include "../src/minivectordb_lite.cpp"
#undef main
#include <random>
namespace lite {
  static std::string legacy_jstr(const std::string&s,size_t&p){
    if(p>=s.size()||s[p++]!='"')throw Err("invalid_request","JSON string");
    std::string r;
    auto hex4=[&](){unsigned v=0;for(int i=0;i<4;++i){if(p==s.size())throw Err("invalid_request","unicode escape");
      unsigned char c=s[p++];int d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;
      if(d<0)throw Err("invalid_request","unicode escape");v=(v<<4)|d;}return v;};
    while(p<s.size()){
      unsigned char c=s[p++];if(c=='"')return r;
      if(c<32)throw Err("invalid_request","JSON control character");
      if(c!='\\'){r+=(char)c;continue;}
      if(p==s.size())break;char e=s[p++];
      if(e=='"'||e=='\\'||e=='/')r+=e;
      else if(e=='n')r+='\n';else if(e=='r')r+='\r';else if(e=='t')r+='\t';else if(e=='b')r+='\b';else if(e=='f')r+='\f';
      else if(e=='u'){
        unsigned cp=hex4();
        if(cp>=0xd800&&cp<=0xdbff){if(p+2>s.size()||s[p]!='\\'||s[p+1]!='u')throw Err("invalid_request","unicode surrogate");p+=2;
          unsigned lo=hex4();if(lo<0xdc00||lo>0xdfff)throw Err("invalid_request","unicode surrogate");cp=0x10000+((cp-0xd800)<<10)+(lo-0xdc00);}
        else if(cp>=0xdc00&&cp<=0xdfff)throw Err("invalid_request","unicode surrogate");
        if(cp<0x80)r+=(char)cp;
        else if(cp<0x800){r+=(char)(0xc0|(cp>>6));r+=(char)(0x80|(cp&63));}
        else if(cp<0x10000){r+=(char)(0xe0|(cp>>12));r+=(char)(0x80|((cp>>6)&63));r+=(char)(0x80|(cp&63));}
        else{r+=(char)(0xf0|(cp>>18));r+=(char)(0x80|((cp>>12)&63));r+=(char)(0x80|((cp>>6)&63));r+=(char)(0x80|(cp&63));}
      }else throw Err("invalid_request","JSON escape");
    }
    throw Err("invalid_request","unterminated JSON string");
  }
}
struct Result {std::string value,code,message;size_t pos;};
static Result parse(const std::string&s,size_t p,bool legacy){
  Result r;try{r.value=legacy?lite::legacy_jstr(s,p):lite::jstr(s,p);}catch(const lite::Err&e){r.code=e.code;r.message=e.what();}r.pos=p;return r;
}
static void require(bool ok,const char*m){if(!ok)throw std::runtime_error(m);}
int main(){try{
  size_t checked=0;std::mt19937_64 rng(903728);
  auto test=[&](const std::string&s,size_t p=0){auto a=parse(s,p,true),b=parse(s,p,false);require(a.pos==b.pos&&a.value==b.value&&a.code==b.code&&a.message==b.message,"JSON scan changed output/error/position");++checked;};
  for(int offset=0;offset<49;++offset)for(int byte=0;byte<256;++byte){
    std::string s="\""+std::string(offset,'x')+char(byte)+std::string(40,'y')+"\"suffix";test(s);test("prefix"+s,6);
  }
  for(int i=0;i<12000;++i){std::string s="\"";for(size_t n=rng()%400;n;--n)s+=char(rng());if(i&1)s+='"';test(s);}
  for(int n=0;n<129;++n)for(const std::string&e:{"\\u0000","\\u007f","\\u0080","\\u07ff","\\u0800","\\uffff","\\ud800\\udc00","\\udbff\\udfff","\\uD83D\\uDE00","\\ud800","\\udc00","\\ud800\\u0041","\\u12xz","\\u123","\\q","\\","\\n","\\/","\\\"","\\\\"})test("\""+std::string(n,'a')+e+"tail\"");
  for(int i=0;i<6000;++i){std::string raw;for(size_t n=rng()%500;n;--n)raw+=char(rng());std::string s;lite::dump_into(lite::js(raw),s);test(s);require(parse(s,0,false).value==raw,"JSON roundtrip");}
  for(auto s:{"","x","\"","\"abc","\"abc\\","\"\\u","\"\\ud800\\u"})test(s);
  // Check allocations of every tail length under ASan; raw bytes >=128 retain legacy treatment.
  for(int n=0;n<1024;++n){test("\""+std::string(n,'x')+"\"");test("\""+std::string(n,'x'));}
  setenv("LITE_JSON_SCAN","0",1);test("\"control\\ud83d\\ude00\"");unsetenv("LITE_JSON_SCAN");
  std::cout<<"PASS JSON input cases="<<checked<<" identical bytes/errors/positions; controls, escapes, surrogates, tails, roundtrip\n";
  volatile size_t sink=0;
  for(size_t bytes:{size_t(1500),size_t(200000)}){std::string s="\""+std::string(bytes,'1')+"\"";int loops=bytes<2000?20000:200;
    for(bool legacy:{true,false}){auto t=std::chrono::steady_clock::now();for(int i=0;i<loops;++i){size_t p=0;auto out=legacy?lite::legacy_jstr(s,p):lite::jstr(s,p);sink+=out.size();}
      std::cout<<"bytes="<<bytes<<" legacy="<<legacy<<" loops="<<loops<<" ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count()<<'\n';}
  }
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
