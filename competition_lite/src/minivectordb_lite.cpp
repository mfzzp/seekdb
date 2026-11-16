#include <algorithm>
#include <functional>
#include <memory>
#include <sys/file.h>
#include <cmath>
#include <chrono>
#include <charconv>
#include <limits>
#include <immintrin.h>
#include <cctype>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <list>
#include <iostream>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <unordered_map>
#include <unordered_set>
namespace lite {
  struct Err:std::runtime_error{
    std::string code;
    Err(std::string c,std::string m):runtime_error(m),code(std::move(c)){
    }
  };
  struct J{
    enum K{
      N,B,I,D,S,A,O
    }
    k=N;
    bool b=false;
    long long i=0;
    double d=0;
    std::string s;
    std::vector<J>a;
    std::map<std::string,J>o;
  };
  static J ji(long long x){
    J j;
    j.k=J::I;
    j.i=x;
    return j;
  }
  static J jd(double x){
    J j;
    j.k=J::D;
    j.d=x;
    return j;
  }
  static J js(std::string x){
    J j;
    j.k=J::S;
    j.s=std::move(x);
    return j;
  }
  static J jb(bool x){J j;j.k=J::B;j.b=x;return j;}
  static J ja(){
    J j;
    j.k=J::A;
    return j;
  }
  static J jo(){
    J j;
    j.k=J::O;
    return j;
  }
  static void ws(const std::string&s,size_t&p){while(p<s.size()&&std::isspace((unsigned char)s[p]))++p;}
  static std::string quoted(const std::string&s,size_t&p,char quote,bool json){
    if(p>=s.size()||s[p++]!=quote)throw Err("invalid_request","string");
    std::string r;
    while(p<s.size()){
      size_t end=s.find(quote,p);
      if(end==std::string::npos){
        if(json)throw Err("invalid_request","string");
        end=s.size();
      }
      const char* escape=(const char*)std::memchr(s.data()+p,'\\',end-p);
      if(!escape){
        r.append(s,p,end-p);p=end+(end<s.size());
        if(!json&&p<s.size()&&s[p]==quote){r+=quote;++p;continue;}
        return r;
      }
      size_t at=(size_t)(escape-s.data());r.append(s,p,at-p);p=at+1;
      if(p==s.size()){if(json)throw Err("invalid_request","string");r+='\\';return r;}
      char e=s[p++];
      r+=json?(e=='n'?'\n':e=='r'?'\r':e=='t'?'\t':e):e;
    }
    if(!json)return r;
    throw Err("invalid_request","string");
  }
  static std::string jstr(const std::string&s,size_t&p){
    if(p>=s.size()||s[p++]!='"')throw Err("invalid_request","JSON string");
    std::string r;
    auto hex4=[&](){unsigned v=0;for(int i=0;i<4;++i){if(p==s.size())throw Err("invalid_request","unicode escape");
      unsigned char c=s[p++];int d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;
      if(d<0)throw Err("invalid_request","unicode escape");v=(v<<4)|d;}return v;};
    bool scan=!getenv("LITE_JSON_SCAN")||std::atoi(getenv("LITE_JSON_SCAN"))!=0;
    while(p<s.size()){
      if(scan){size_t b=p;while(s.size()-p>=16){__m128i v=_mm_loadu_si128((const __m128i*)(s.data()+p)),control=_mm_set1_epi8(31);
          unsigned mask=_mm_movemask_epi8(_mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v,_mm_set1_epi8('"')),_mm_cmpeq_epi8(v,_mm_set1_epi8('\\'))),_mm_cmpeq_epi8(_mm_max_epu8(v,control),control)));
          if(mask){p+=__builtin_ctz(mask);break;}p+=16;}r.append(s,b,p-b);if(p==s.size())break;}
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
  static J pv(const std::string&s,size_t&p){
    ws(s,p);
    if(p>=s.size())throw Err("invalid_request","value");
    if(s[p]=='"')return js(jstr(s,p));
    if(s[p]=='{'){
      J x=jo();
      ++p;
      ws(s,p);
      while(p<s.size()&&s[p]!='}'){
        std::string k=jstr(s,p);
        ws(s,p);
        if(p>=s.size()||s[p++]!=':')throw Err("invalid_request","colon");
        x.o[k]=pv(s,p);
        ws(s,p);
        if(p<s.size()&&s[p]==',')++p;
        ws(s,p);
      }
      if(p>=s.size())throw Err("invalid_request","object");
      ++p;
      return x;
    }
    if(s[p]=='['){
      J x=ja();
      ++p;
      ws(s,p);
      while(p<s.size()&&s[p]!=']'){
        x.a.push_back(pv(s,p));
        ws(s,p);
        if(p<s.size()&&s[p]==',')++p;
        ws(s,p);
      }
      if(p>=s.size())throw Err("invalid_request","array");
      ++p;
      return x;
    }
    size_t b=p;
    while(p<s.size()&&!std::isspace((unsigned char)s[p])&&s[p]!=','&&s[p]!='}'&&s[p]!=']')++p;
    std::string n=s.substr(b,p-b);
    if(n=="true")return jb(true);
    if(n=="false")return jb(false);
    if(n=="null")return J{
    };
    J x;
    try{
      if(n.find_first_of(".eE")!=std::string::npos){
        x.k=J::D;
        x.d=std::stod(n);
      }
      else{
        x.k=J::I;
        x.i=std::stoll(n);
      }
    }
    catch(...){
      throw Err("invalid_request","number");
    }
    return x;
  }
  static J parse(const std::string&s){
    size_t p=0;
    J x=pv(s,p);
    ws(s,p);
    if(p!=s.size())throw Err("invalid_request","trailing");
    return x;
  }
  static std::string esc(const std::string&s){
    std::string r="\"";
    for(char c:s){
      if((unsigned char)c<32){const char*h="0123456789abcdef";r+="\\u00";r+=h[((unsigned char)c)>>4];r+=h[c&15];continue;}
      if(c=='"'||c=='\\')r+='\\';
      if(c=='\n')r+="\\n";
      else if(c=='\r')r+="\\r";
      else if(c=='\t')r+="\\t";
      else r+=c;
    }
    return r+"\"";
  }
  static std::string dump_legacy(const J&x){
    if(x.k==J::N)return"null";
    if(x.k==J::B)return x.b?"true":"false";
    if(x.k==J::I)return std::to_string(x.i);
    if(x.k==J::D){
      if(!std::isfinite(x.d))throw Err("invalid_value","nonfinite result");
      std::ostringstream o;
      o<<std::setprecision(17)<<x.d;
      std::string r=o.str();if(r.find_first_of(".eE")==std::string::npos)r+=".0";return r;
    }
    if(x.k==J::S)return esc(x.s);
    if(x.k==J::A){
      std::string r="[";
      for(size_t i=0;i<x.a.size();++i)r+=(i?",":"")+dump_legacy(x.a[i]);
      return r+"]";
    }
    std::string r="{";
    bool f=true;
    for(auto&v:x.o){
      if(!f)r+=',';
      f=false;
      r+=esc(v.first)+":"+dump_legacy(v.second);
    }
    return r+"}";
  }
  static void dump_into(const J&x,std::string&r){
    if(x.k==J::N){r+="null";return;}if(x.k==J::B){r+=x.b?"true":"false";return;}
    if(x.k==J::I){r+=std::to_string(x.i);return;}
    if(x.k==J::D){
      if(!std::isfinite(x.d))throw Err("invalid_value","nonfinite result");
      char b[64];auto end=std::to_chars(b,b+sizeof(b),x.d,std::chars_format::general,17);
      if(end.ec!=std::errc{})throw Err("invalid_value","number encoding");
      r.append(b,end.ptr);if(std::find_if(b,end.ptr,[](char c){return c=='.'||c=='e'||c=='E';})==end.ptr)r+=".0";return;
    }
    if(x.k==J::S){r+=esc(x.s);return;}
    if(x.k==J::A){r+='[';for(size_t i=0;i<x.a.size();++i){if(i)r+=',';dump_into(x.a[i],r);}r+=']';return;}
    r+='{';bool first=true;for(auto&v:x.o){if(!first)r+=',';first=false;r+=esc(v.first);r+=':';dump_into(v.second,r);}r+='}';
  }
  static std::string dump(const J&x){
    const char*e=std::getenv("LITE_JSON_FAST");if(e&&std::atoi(e)==0)return dump_legacy(x);
    std::string r;r.reserve(1024);dump_into(x,r);return r;
  }
  static std::vector<std::string> tok(const std::string&s){
    std::vector<std::string>r;
    for(size_t p=0;p<s.size();){
      while(p<s.size()&&std::isspace((unsigned char)s[p]))++p;
      if(p>=s.size())break;
      if(s[p]=='\''){
        r.push_back("'"+quoted(s,p,'\'',false)+"'");
      }
      else if(std::string("(),;=<>!").find(s[p])!=std::string::npos){
        if(p+1<s.size()&&(((s[p]=='<'||s[p]=='>'||s[p]=='!')&&s[p+1]=='=')||(s[p]=='<'&&s[p+1]=='>'))){
          r.push_back(s.substr(p,2));
          p+=2;
        }
        else r.push_back(std::string(1,s[p++]));
      }
      else{
        size_t b=p;
        while(p<s.size()&&!std::isspace((unsigned char)s[p])&&std::string("(),;=<>!").find(s[p])==std::string::npos)++p;
        r.push_back(s.substr(b,p-b));
      }
    }
    while(!r.empty()&&r.back()==";")r.pop_back();
    return r;
  }
  static std::string up(std::string x){
    for(char&c:x)c=(char)std::toupper((unsigned char)c);
    return x;
  }
  static std::string unq(const std::string&s){
    return s.size()>=2&&s.front()=='\''&&s.back()=='\''?s.substr(1,s.size()-2):s;
  }
  static std::string sqltok(const std::string&s){
    if(s.size()<2||s.front()!='\''||s.back()!='\'')return s;
    std::string r="'";
    for(size_t i=1;i+1<s.size();++i){if(s[i]=='\''||s[i]=='\\')r+=s[i];r+=s[i];}
    return r+"'";
  }
  struct Col{
    std::string n,t;
    int d=0;
  };
  struct Row{
    long long id=0;
    uint64_t ordinal=0;
    std::vector<std::string>v;
    std::vector<off_t>voff;
  };
  struct Table{
    std::string n;
    std::vector<Col>c;
    std::unordered_map<long long,Row>r;
    std::vector<long long>order;
    std::vector<Row*>fast;
    uint64_t count=0,row_bytes=0;
    bool hot=false;
  };
  struct Cond{
    int col=-1;
    std::string op,val;
  };
  struct PQ{
    int d=0,m=0,ks=0,nlist=0;
    std::vector<float>coarse,book;
    std::unordered_map<long long,std::vector<unsigned char>>code;
    std::unordered_map<long long,int>cell;
  };
  struct VBlock{
    size_t begin=0,end=0;
    std::vector<float>lo,hi;
    std::vector<float>center;
    float radius=0;
  };
  struct Matrix{
    int col=0,dim=0;
    std::vector<float>v;
    size_t values=0;off_t mapped_off=-1;
    std::vector<int8_t>codes;std::vector<uint8_t>packed;std::vector<int>norms;double scale=0;
  };
  struct SRec{
    std::string key,value;
    uint64_t seq=0;
    bool tomb=false;
  };
  static uint32_t crc32c_portable(const char*p,size_t n){
    static const auto table=[](){std::vector<uint32_t> t(256);for(uint32_t i=0;i<256;++i){uint32_t c=i;for(int k=0;k<8;++k)c=(c>>1)^((c&1)?0x82f63b78u:0u);t[i]=c;}return t;}();
    uint32_t c=0xffffffffu;
    for(size_t i=0;i<n;++i)c=table[(c^(unsigned char)p[i])&255]^(c>>8);
    return ~c;
  }
  #if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
  __attribute__((target("sse4.2"))) static uint32_t crc32c_hw(const char*p,size_t n){
    uint64_t c=0xffffffffu;while(n>=8){uint64_t x;std::memcpy(&x,p,8);c=_mm_crc32_u64(c,x);p+=8;n-=8;}
    while(n--)c=_mm_crc32_u8((uint32_t)c,(unsigned char)*p++);
    return ~(uint32_t)c;
  }
  #endif
  static uint32_t crc32c(const char*p,size_t n){
    #if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    static const bool hw=__builtin_cpu_supports("sse4.2");if(hw)return crc32c_hw(p,n);
    #endif
    return crc32c_portable(p,n);
  }
  static int envint(const char*n,int d){
    const char*x=std::getenv(n);
    return x&&*x?std::max(1,std::atoi(x)):d;
  }
  static const uint32_t WAL_MAGIC=0x4d564c32u;
  #if defined(__x86_64__) || defined(__i386__)
  #if defined(__GNUC__) || defined(__clang__)
  template<bool Early=false> __attribute__((target("avx2"),always_inline)) static inline double l2_avx2_core(const float*a,const float*b,int d,double lim=0,int*used=nullptr){
    if constexpr(Early)*used=d;
    __m256d s0=_mm256_setzero_pd(),s1=s0,s2=s0,s3=s0;
    int i=0;
    for(;i+16<=d;i+=16){
      __m256d x=_mm256_sub_pd(_mm256_cvtps_pd(_mm_loadu_ps(a+i)),_mm256_cvtps_pd(_mm_loadu_ps(b+i)));
      s0=_mm256_add_pd(s0,_mm256_mul_pd(x,x));
      x=_mm256_sub_pd(_mm256_cvtps_pd(_mm_loadu_ps(a+i+4)),_mm256_cvtps_pd(_mm_loadu_ps(b+i+4)));
      s1=_mm256_add_pd(s1,_mm256_mul_pd(x,x));
      x=_mm256_sub_pd(_mm256_cvtps_pd(_mm_loadu_ps(a+i+8)),_mm256_cvtps_pd(_mm_loadu_ps(b+i+8)));
      s2=_mm256_add_pd(s2,_mm256_mul_pd(x,x));
      x=_mm256_sub_pd(_mm256_cvtps_pd(_mm_loadu_ps(a+i+12)),_mm256_cvtps_pd(_mm_loadu_ps(b+i+12)));
      s3=_mm256_add_pd(s3,_mm256_mul_pd(x,x));
      if constexpr(Early)if((i+16)%64==0&&i+16<d){
        alignas(32) double z[4];_mm256_store_pd(z,_mm256_add_pd(_mm256_add_pd(s0,s1),_mm256_add_pd(s2,s3)));
        double partial=0;for(double value:z)partial+=value;
        if(partial>lim){*used=i+16;return partial;}
      }
    }
    s0=_mm256_add_pd(s0,s1);s2=_mm256_add_pd(s2,s3);s0=_mm256_add_pd(s0,s2);
    for(;i+4<=d;i+=4){
      __m256d x=_mm256_sub_pd(_mm256_cvtps_pd(_mm_loadu_ps(a+i)),_mm256_cvtps_pd(_mm_loadu_ps(b+i)));
      s0=_mm256_add_pd(s0,_mm256_mul_pd(x,x));
    }
    alignas(32) double z[4];_mm256_store_pd(z,s0);double s=0;for(double x:z)s+=x;
    for(;i<d;++i){double x=double(a[i])-double(b[i]);s+=x*x;}return s;
  }
  template<bool Early=false> __attribute__((target("avx512f"),always_inline)) static inline double l2_avx512_core(const float*a,const float*b,int d,double lim=0,int*used=nullptr){
    if constexpr(Early)*used=d;
    __m512d s0=_mm512_setzero_pd(),s1=s0,s2=s0,s3=s0;
    int i=0;
    for(;i+32<=d;i+=32){
      __m512d x=_mm512_sub_pd(_mm512_cvtps_pd(_mm256_loadu_ps(a+i)),_mm512_cvtps_pd(_mm256_loadu_ps(b+i)));
      s0=_mm512_add_pd(s0,_mm512_mul_pd(x,x));
      x=_mm512_sub_pd(_mm512_cvtps_pd(_mm256_loadu_ps(a+i+8)),_mm512_cvtps_pd(_mm256_loadu_ps(b+i+8)));
      s1=_mm512_add_pd(s1,_mm512_mul_pd(x,x));
      x=_mm512_sub_pd(_mm512_cvtps_pd(_mm256_loadu_ps(a+i+16)),_mm512_cvtps_pd(_mm256_loadu_ps(b+i+16)));
      s2=_mm512_add_pd(s2,_mm512_mul_pd(x,x));
      x=_mm512_sub_pd(_mm512_cvtps_pd(_mm256_loadu_ps(a+i+24)),_mm512_cvtps_pd(_mm256_loadu_ps(b+i+24)));
      s3=_mm512_add_pd(s3,_mm512_mul_pd(x,x));
      if constexpr(Early)if((i+32)%64==0&&i+32<d){
        alignas(64) double z[8];_mm512_store_pd(z,_mm512_add_pd(_mm512_add_pd(s0,s1),_mm512_add_pd(s2,s3)));
        double partial=0;for(double value:z)partial+=value;
        if(partial>lim){*used=i+32;return partial;}
      }
    }
    s0=_mm512_add_pd(s0,s1);s2=_mm512_add_pd(s2,s3);s0=_mm512_add_pd(s0,s2);
    for(;i+8<=d;i+=8){
      __m512d x=_mm512_sub_pd(_mm512_cvtps_pd(_mm256_loadu_ps(a+i)),_mm512_cvtps_pd(_mm256_loadu_ps(b+i)));
      s0=_mm512_add_pd(s0,_mm512_mul_pd(x,x));
    }
    alignas(64) double z[8];_mm512_store_pd(z,s0);double s=0;for(double x:z)s+=x;
    for(;i<d;++i){double x=double(a[i])-double(b[i]);s+=x*x;}return s;
  }
  template<bool Early=false> __attribute__((target("avx2"))) static double l2_avx2(const float*a,const float*b,int d,double lim=0,int*used=nullptr){return l2_avx2_core<Early>(a,b,d,lim,used);}
  template<bool Early=false> __attribute__((target("avx512f"))) static double l2_avx512(const float*a,const float*b,int d,double lim=0,int*used=nullptr){return l2_avx512_core<Early>(a,b,d,lim,used);}
  template<int N> __attribute__((target("avx2"))) static void l2_avx2_many(const float*const*a,const float*b,int d,double*out){
    __m256d s0[N],s1[N],s2[N],s3[N];
    #pragma GCC unroll 4
    for(int k=0;k<N;++k)s0[k]=s1[k]=s2[k]=s3[k]=_mm256_setzero_pd();
    int i=0;
    for(;i+16<=d;i+=16){
      __m256d q0=_mm256_cvtps_pd(_mm_loadu_ps(b+i));
    #pragma GCC unroll 4
      for(int k=0;k<N;++k){__m256d x=_mm256_sub_pd(_mm256_cvtps_pd(_mm_loadu_ps(a[k]+i)),q0);s0[k]=_mm256_add_pd(s0[k],_mm256_mul_pd(x,x));}
      __m256d q1=_mm256_cvtps_pd(_mm_loadu_ps(b+i+4));
    #pragma GCC unroll 4
      for(int k=0;k<N;++k){__m256d x=_mm256_sub_pd(_mm256_cvtps_pd(_mm_loadu_ps(a[k]+i+4)),q1);s1[k]=_mm256_add_pd(s1[k],_mm256_mul_pd(x,x));}
      __m256d q2=_mm256_cvtps_pd(_mm_loadu_ps(b+i+8));
    #pragma GCC unroll 4
      for(int k=0;k<N;++k){__m256d x=_mm256_sub_pd(_mm256_cvtps_pd(_mm_loadu_ps(a[k]+i+8)),q2);s2[k]=_mm256_add_pd(s2[k],_mm256_mul_pd(x,x));}
      __m256d q3=_mm256_cvtps_pd(_mm_loadu_ps(b+i+12));
    #pragma GCC unroll 4
      for(int k=0;k<N;++k){__m256d x=_mm256_sub_pd(_mm256_cvtps_pd(_mm_loadu_ps(a[k]+i+12)),q3);s3[k]=_mm256_add_pd(s3[k],_mm256_mul_pd(x,x));}
    }
    #pragma GCC unroll 4
    for(int k=0;k<N;++k)s0[k]=_mm256_add_pd(_mm256_add_pd(s0[k],s1[k]),_mm256_add_pd(s2[k],s3[k]));
    for(;i+4<=d;i+=4){
      __m256d q=_mm256_cvtps_pd(_mm_loadu_ps(b+i));
    #pragma GCC unroll 4
      for(int k=0;k<N;++k){__m256d x=_mm256_sub_pd(_mm256_cvtps_pd(_mm_loadu_ps(a[k]+i)),q);s0[k]=_mm256_add_pd(s0[k],_mm256_mul_pd(x,x));}
    }
    #pragma GCC unroll 4
    for(int k=0;k<N;++k){
      alignas(32) double z[4];_mm256_store_pd(z,s0[k]);double s=0;for(double x:z)s+=x;
      for(int j=i;j<d;++j){double x=double(a[k][j])-double(b[j]);s+=x*x;}out[k]=s;
    }
  }
  template<int N> __attribute__((target("avx512f"))) static void l2_avx512_many(const float*const*a,const float*b,int d,double*out){
    __m512d s0[N],s1[N],s2[N],s3[N];
    #pragma GCC unroll 4
    for(int k=0;k<N;++k)s0[k]=s1[k]=s2[k]=s3[k]=_mm512_setzero_pd();
    int i=0;
    for(;i+32<=d;i+=32){
      __m512d q0=_mm512_cvtps_pd(_mm256_loadu_ps(b+i));
    #pragma GCC unroll 4
      for(int k=0;k<N;++k){__m512d x=_mm512_sub_pd(_mm512_cvtps_pd(_mm256_loadu_ps(a[k]+i)),q0);s0[k]=_mm512_add_pd(s0[k],_mm512_mul_pd(x,x));}
      __m512d q1=_mm512_cvtps_pd(_mm256_loadu_ps(b+i+8));
    #pragma GCC unroll 4
      for(int k=0;k<N;++k){__m512d x=_mm512_sub_pd(_mm512_cvtps_pd(_mm256_loadu_ps(a[k]+i+8)),q1);s1[k]=_mm512_add_pd(s1[k],_mm512_mul_pd(x,x));}
      __m512d q2=_mm512_cvtps_pd(_mm256_loadu_ps(b+i+16));
    #pragma GCC unroll 4
      for(int k=0;k<N;++k){__m512d x=_mm512_sub_pd(_mm512_cvtps_pd(_mm256_loadu_ps(a[k]+i+16)),q2);s2[k]=_mm512_add_pd(s2[k],_mm512_mul_pd(x,x));}
      __m512d q3=_mm512_cvtps_pd(_mm256_loadu_ps(b+i+24));
    #pragma GCC unroll 4
      for(int k=0;k<N;++k){__m512d x=_mm512_sub_pd(_mm512_cvtps_pd(_mm256_loadu_ps(a[k]+i+24)),q3);s3[k]=_mm512_add_pd(s3[k],_mm512_mul_pd(x,x));}
    }
    #pragma GCC unroll 4
    for(int k=0;k<N;++k)s0[k]=_mm512_add_pd(_mm512_add_pd(s0[k],s1[k]),_mm512_add_pd(s2[k],s3[k]));
    for(;i+8<=d;i+=8){
      __m512d q=_mm512_cvtps_pd(_mm256_loadu_ps(b+i));
    #pragma GCC unroll 4
      for(int k=0;k<N;++k){__m512d x=_mm512_sub_pd(_mm512_cvtps_pd(_mm256_loadu_ps(a[k]+i)),q);s0[k]=_mm512_add_pd(s0[k],_mm512_mul_pd(x,x));}
    }
    #pragma GCC unroll 4
    for(int k=0;k<N;++k){
      alignas(64) double z[8];_mm512_store_pd(z,s0[k]);double s=0;for(double x:z)s+=x;
      for(int j=i;j<d;++j){double x=double(a[k][j])-double(b[j]);s+=x*x;}out[k]=s;
    }
  }
  __attribute__((target("avx2"))) static void l2_avx2_scan(const float*a,const float*b,int d,size_t n,double*out){
    for(size_t i=0;i<n;++i)out[i]=l2_avx2_core(a+i*(size_t)d,b,d);
  }
  __attribute__((target("avx512f"))) static void l2_avx512_scan(const float*a,const float*b,int d,size_t n,double*out){
    for(size_t i=0;i<n;++i)out[i]=l2_avx512_core(a+i*(size_t)d,b,d);
  }
  __attribute__((target("avx512f"))) static void l2_avx512_query128(const float*a,const float*b,int,size_t n,double*out){
    __m512d query[16];
    #pragma GCC unroll 16
    for(int j=0;j<16;++j)query[j]=_mm512_cvtps_pd(_mm256_loadu_ps(b+j*8));
    for(size_t i=0;i<n;++i){
      const float*p=a+i*128;__m512d s0=_mm512_setzero_pd(),s1=s0,s2=s0,s3=s0;
      #pragma GCC unroll 4
      for(int j=0;j<16;j+=4){
        __m512d x0=_mm512_sub_pd(_mm512_cvtps_pd(_mm256_loadu_ps(p+(j+0)*8)),query[j+0]);s0=_mm512_add_pd(s0,_mm512_mul_pd(x0,x0));
        __m512d x1=_mm512_sub_pd(_mm512_cvtps_pd(_mm256_loadu_ps(p+(j+1)*8)),query[j+1]);s1=_mm512_add_pd(s1,_mm512_mul_pd(x1,x1));
        __m512d x2=_mm512_sub_pd(_mm512_cvtps_pd(_mm256_loadu_ps(p+(j+2)*8)),query[j+2]);s2=_mm512_add_pd(s2,_mm512_mul_pd(x2,x2));
        __m512d x3=_mm512_sub_pd(_mm512_cvtps_pd(_mm256_loadu_ps(p+(j+3)*8)),query[j+3]);s3=_mm512_add_pd(s3,_mm512_mul_pd(x3,x3));
      }
      alignas(64) double z[8];_mm512_store_pd(z,_mm512_add_pd(_mm512_add_pd(s0,s1),_mm512_add_pd(s2,s3)));
      double s=0;for(double x:z)s+=x;out[i]=s;
    }
  }
  __attribute__((target("avx512f,avx512bw"))) static void l2_screen128(const int8_t*a,const int8_t*b,const size_t*pos,size_t n,int*out){
    __m512i q[4];
    #pragma GCC unroll 4
    for(int j=0;j<4;++j)q[j]=_mm512_cvtepi8_epi16(_mm256_loadu_si256((const __m256i*)(b+j*32)));
    for(size_t i=0;i<n;++i){const int8_t*p=a+(pos?pos[i]:i)*128;__m512i s=_mm512_setzero_si512();
      #pragma GCC unroll 4
      for(int j=0;j<4;++j){__m512i x=_mm512_sub_epi16(_mm512_cvtepi8_epi16(_mm256_loadu_si256((const __m256i*)(p+j*32))),q[j]);s=_mm512_add_epi32(s,_mm512_madd_epi16(x,x));}
      out[i]=_mm512_reduce_add_epi32(s);
    }
  }
  static double screen_query128(const float*q,double scale,int8_t*out){
    double error=0;for(int j=0;j<128;++j){double v=double(q[j])/scale;out[j]=(int8_t)std::llround(std::max(-127.0,std::min(127.0,v)));double d=v-out[j];error+=d*d;}
    return std::sqrt(error)*(1+1e-12)+std::sqrt(128.0)*(.5+1e-9)+1e-9;
  }
  static double screen_threshold128(double limit,double scale,double error){
    double radius=std::sqrt(limit)*(1+1e-12)/scale+error;return radius*radius*(1+1e-12);
  }
  static void screen_pack128(Matrix&m){
    size_t n=m.codes.size()/128,padded=(n+15)/16*16;m.packed.assign(padded*128,128);m.norms.assign(padded,0);
    for(size_t i=0;i<n;++i)for(int j=0;j<128;++j){int c=m.codes[i*128+j];m.packed[(i/16)*2048+(j/4)*64+(i%16)*4+j%4]=(uint8_t)(c+128);m.norms[i]+=c*c;}
  }
  __attribute__((target("avx512f,avx512vnni"))) static void l2_screen_dot128(const uint8_t*a,const int*norms,const int8_t*q,size_t n,int*out){
    int extra=0;for(int j=0;j<128;++j)extra+=int(q[j])*q[j]+256*int(q[j]);
    for(size_t i=0;i<n;i+=16){__m512i dot=_mm512_setzero_si512(),d1=dot,d2=dot,d3=dot;for(int j=0;j<32;j+=4){int w[4];std::memcpy(w,q+j*4,16);
      dot=_mm512_dpbusd_epi32(dot,_mm512_loadu_si512(a+i*128+j*64),_mm512_set1_epi32(w[0]));d1=_mm512_dpbusd_epi32(d1,_mm512_loadu_si512(a+i*128+(j+1)*64),_mm512_set1_epi32(w[1]));
      d2=_mm512_dpbusd_epi32(d2,_mm512_loadu_si512(a+i*128+(j+2)*64),_mm512_set1_epi32(w[2]));d3=_mm512_dpbusd_epi32(d3,_mm512_loadu_si512(a+i*128+(j+3)*64),_mm512_set1_epi32(w[3]));}dot=_mm512_add_epi32(_mm512_add_epi32(dot,d1),_mm512_add_epi32(d2,d3));
      __m512i sum=_mm512_sub_epi32(_mm512_add_epi32(_mm512_loadu_si512(norms+i),_mm512_set1_epi32(extra)),_mm512_slli_epi32(dot,1));
      _mm512_mask_storeu_epi32(out+i,(__mmask16)((1u<<std::min<size_t>(16,n-i))-1),sum);
    }
  }
  static bool has_avx2(){
    static const bool v=__builtin_cpu_supports("avx2")&&(!std::getenv("LITE_SIMD")||std::string(std::getenv("LITE_SIMD"))!="scalar");
    return v;
  }
  static bool has_avx512(){
    static const bool v=__builtin_cpu_supports("avx512f")&&(!std::getenv("LITE_SIMD")||std::string(std::getenv("LITE_SIMD"))=="auto");
    return v;
  }
  #else
  static bool has_avx2(){
    return false;
  }
  static bool has_avx512(){
    return false;
  }
  #endif
  #else
  static bool has_avx2(){
    return false;
  }
  static bool has_avx512(){
    return false;
  }
  #endif
  #include "lsm_store.hpp"

  struct Engine{
    std::string dir,vecfile;
    std::map<std::string,Table>tabs;
    std::set<std::string>indexes;
    std::map<std::string,int>index_cols;
    std::unique_ptr<lsm::Store> store;
    std::unique_ptr<lsm::Batch> tx,statement;
    bool failed_io=false,in=false,recovering=false,ann_mode=false;
    off_t vec_mark=0,vec_end=0,vec_cursor=0;
    uint64_t ordinal=0;
    int vfd=-1;
    std::string vecbuf;
    off_t vecbuf_base=0;
    std::vector<float>scratch;
    std::unordered_map<std::string,std::string>vcache;
    std::map<std::string,std::vector<VBlock>>vblocks;
    std::map<std::string,std::map<std::string,std::vector<long long>>>sindex;
    std::map<std::string,std::map<long long,std::vector<long long>>>nindex;
    std::map<std::string,std::unordered_map<long long,size_t>>fast_pos;
    std::map<std::string,Matrix>matrices;
    struct HotPostings {std::string table;int col=-1;size_t bytes=0,cap=0;bool ready=false;std::map<std::string,std::vector<size_t>> slots;};
    HotPostings hot_posts;uint64_t hot_post_builds=0;
    struct VectorRef {long long id;off_t off;};
    struct QueryView {int col=0,post_col=-1,rejected_col=-1;uint64_t version=0,used=0;std::vector<VectorRef> rows;std::map<std::string,std::vector<uint32_t>> posts;size_t post_bytes=0;size_t bytes()const{return sizeof(*this)+256+post_bytes+rows.capacity()*sizeof(VectorRef);}};
    std::map<std::string,QueryView> query_views;uint64_t view_version=0,view_clock=0;mutable uint64_t row_decodes=0;
    std::map<std::string,int>vblock_col;
    struct MapWindow {void*ptr;off_t off;size_t bytes;uint64_t used;};
    mutable std::vector<MapWindow> vector_maps;mutable uint64_t map_clock=0,map_calls=0;
    mutable void*vmap=nullptr;
    mutable size_t mapsz=0;
    mutable off_t mapoff=0;
    explicit Engine(std::string d):dir(std::move(d)){
      for(size_t p=1;p<dir.size();++p)if(dir[p]=='/')mkdir(dir.substr(0,p).c_str(),0755);
      lsm::io(mkdir(dir.c_str(),0755)==0||errno==EEXIST,"create data directory");
      bool fresh=!lsm::exists(dir+"/LSM-MANIFEST");
      bool legacy=fresh&&(lsm::exists(dir+"/STATE")||lsm::exists(dir+"/wal.log")||lsm::exists(dir+"/MANIFEST"));
      if(fresh&&!lsm::exists(dir+"/vectors.f32")){lsm::FD f(open((dir+"/vectors.f32").c_str(),O_WRONLY|O_CREAT|O_EXCL,0644));lsm::io(f.n>=0,"create vector file");lsm::sync(f.n);lsm::sync_dir(dir);}
      store=std::make_unique<lsm::Store>(dir,legacy);
      vecfile=dir+"/"+store->vector_name;
      vfd=open(vecfile.c_str(),O_RDWR|O_APPEND);lsm::io(vfd>=0,"open referenced vector file");store->vfd=vfd;store->replay();
      if(store->old_manifest)store->checkpoint();vec_end=(off_t)lsm::file_size(vfd);vecbuf_base=vec_end;
      if(legacy)migrate_legacy();
      if(store->vector_end>(uint64_t)vec_end)throw Err("corruption","committed vector file is incomplete");
      if(store->vector_end!=(uint64_t)vec_end)trunc_vec((off_t)store->vector_end);
      vecbuf_base=vec_end;load_catalog();store->cleanup();
    }
    ~Engine(){clear_maps();if(vfd>=0)close(vfd);}
    static off_t fsize(const std::string&p){struct stat s{};return stat(p.c_str(),&s)==0?s.st_size:0;}
    template<class T> static bool rd(std::ifstream&f,T&v){return bool(f.read((char*)&v,sizeof(v)));}
    static bool rs(std::ifstream&f,std::string&s){uint32_t n=0;if(!rd(f,n)||n>(16u<<20))return false;s.resize(n);return bool(f.read(s.data(),n));}
    void trunc_vec(off_t n){clear_maps();lsm::io(ftruncate(vfd,n)==0,"truncate uncommitted vectors");vec_end=n;}
    [[noreturn]] void commit_io_error(const char*what){failed_io=true;throw Err("io_error",what);}
    void write_all(int fd,const char*p,size_t n){try{lsm::write_all(fd,p,n);}catch(...){failed_io=true;throw;}}
    void sync_commit(int fd){try{lsm::sync(fd,true);}catch(...){failed_io=true;throw;}}
    void flush_vecbuf(){if(vecbuf.empty())return;write_all(vfd,vecbuf.data(),vecbuf.size());vecbuf.clear();vecbuf_base=vec_end;}
    static std::string prefix(const Table&q){return "R"+lsm::part(q.n);}
    static std::string skey(const Table&q,long long id){return prefix(q)+lsm::number(uint64_t(id)^(1ull<<63));}
    static std::string row_payload(const Table&q,const Row&r){std::string s;lsm::u64(s,uint64_t(r.id));lsm::u64(s,r.ordinal);for(size_t i=1;i<q.c.size();++i){lsm::str(s,r.v.at(i-1));lsm::u64(s,uint64_t(r.voff.at(i)));}return s;}
    Row decode_row(const Table&q,const std::string&s)const{++row_decodes;lsm::Decode d{s};Row r;r.id=(long long)d.u64();r.ordinal=d.u64();r.voff.push_back(-1);for(size_t i=1;i<q.c.size();++i){r.v.push_back(d.str());r.voff.push_back((off_t)d.u64());if(q.c[i].t=="VECTOR"&&(r.voff.back()<0||uint64_t(r.voff.back())+uint64_t(q.c[i].d)*4>(uint64_t)vec_end))throw Err("corruption","row vector offset");}d.end();return r;}
    static std::string catalog_key(const Table&q){return "C"+lsm::part(q.n);}
    static std::string catalog(const Table&q){std::string s;lsm::str(s,q.n);lsm::u32(s,(uint32_t)q.c.size());for(auto&c:q.c){lsm::str(s,c.n);lsm::str(s,c.t);lsm::u32(s,(uint32_t)c.d);}lsm::u64(s,q.count);lsm::u64(s,q.row_bytes);return s;}
    static Table decode_catalog(const std::string&s){lsm::Decode d{s};Table q;q.n=d.str();uint32_t n=d.u32();if(n>1024)throw Err("corruption","schema columns");for(uint32_t i=0;i<n;++i){Col c;c.n=d.str();c.t=d.str();c.d=(int)d.u32();q.c.push_back(std::move(c));}q.count=d.u64();q.row_bytes=d.u64();d.end();if(q.c.empty()||q.c[0].t!="BIGINT")throw Err("corruption","schema primary key");return q;}
    bool get(const std::string&key,SRec&r)const{if(tx&&tx->get(key,r))return !r.tomb;return store->get(key,r)&&!r.tomb;}
    std::unique_ptr<lsm::Merge> scan(const std::string&lo,const std::string&hi)const{
      std::vector<std::unique_ptr<lsm::Cursor>> cs;cs.push_back(store->scan(lo,hi,false));if(tx&&!tx->empty())cs.push_back(std::make_unique<lsm::Overlay>(tx->scan(lo,hi)));return std::make_unique<lsm::Merge>(std::move(cs),true);
    }
    bool row_get(const Table&q,long long id,Row&r)const{SRec x;if(!get(skey(q,id),x))return false;r=decode_row(q,x.value);return true;}
    std::unique_ptr<lsm::Merge> rows_get(std::vector<std::string> keys)const{
      std::sort(keys.begin(),keys.end());keys.erase(std::unique(keys.begin(),keys.end()),keys.end());auto shared=std::make_shared<const std::vector<std::string>>(std::move(keys));
      std::vector<std::unique_ptr<lsm::Cursor>> cs;cs.push_back(lsm::selected(store->mem,store->files,shared));if(tx&&!tx->empty())cs.push_back(std::make_unique<lsm::Overlay>(lsm::selected(tx->mem,tx->runs,shared)));return std::make_unique<lsm::Merge>(std::move(cs),true);
    }
    void load_catalog(){
      tabs.clear();indexes.clear();index_cols.clear();clear_caches();
      auto c=scan("C","D");while(c->valid){Table q=decode_catalog(c->row.value);tabs.emplace(q.n,std::move(q));c->next();}
      c=scan("X","Y");while(c->valid){lsm::Decode d{c->row.value};std::string name=d.str(),table=d.str();int col=(int)d.u32();d.end();if(!table.empty()&&(!tabs.count(table)||col<=0||col>=(int)tabs.at(table).c.size()))throw Err("corruption","index catalog");indexes.insert(name);c->next();}
      c=scan("V","W");while(c->valid){lsm::Decode d{c->row.value};std::string table=d.str();int col=(int)d.u32();d.end();if(!tabs.count(table)||col<=0||col>=(int)tabs.at(table).c.size())throw Err("corruption","vector index catalog");index_cols[table]=col;c->next();}
      SRec r;ordinal=0;if(get("O",r)){lsm::Decode d{r.value};ordinal=d.u64();d.end();}
    }
    static std::string index_prefix(const Table&q,int c){return "I"+lsm::part(q.n)+lsm::number((uint64_t)c);}
    static std::string scalar_key(const Col&c,const std::string&v){return c.t=="BIGINT"?lsm::number(uint64_t(std::stoll(unq(v)))^(1ull<<63)):lsm::part(unq(v));}
    static std::string secondary(const Table&q,const Row&r,int c){return index_prefix(q,c)+scalar_key(q.c[c],r.v.at(c-1))+lsm::number(uint64_t(r.id)^(1ull<<63));}
    void row_put(Table&q,const Row*r,const Row*before){
      if(before){for(size_t c=1;c<q.c.size();++c)if(q.c[c].t!="VECTOR"&&(!r||r->v[c-1]!=before->v[c-1]))statement->put(secondary(q,*before,(int)c),{},true);q.row_bytes-=row_payload(q,*before).size();}
      if(r){for(size_t c=1;c<q.c.size();++c)if(q.c[c].t!="VECTOR"&&(!before||r->v[c-1]!=before->v[c-1]))statement->put(secondary(q,*r,(int)c),{});std::string s=row_payload(q,*r);q.row_bytes+=s.size();statement->put(skey(q,r->id),std::move(s));}
      else statement->put(skey(q,before->id),{},true);
      if(!before)++q.count;if(!r)--q.count;
    }
    void clear_caches(){hot_posts={};query_views.clear();++view_version;sindex.clear();nindex.clear();fast_pos.clear();matrices.clear();vblocks.clear();vblock_col.clear();}
    void cool(Table&q){if(hot_posts.table==q.n)hot_posts={};query_views.erase(q.n);++view_version;q.r.clear();q.r.rehash(0);q.order.clear();q.order.shrink_to_fit();q.fast.clear();q.fast.shrink_to_fit();q.hot=false;matrices.erase(q.n);fast_pos.erase(q.n);vblocks.erase(q.n);for(auto&c:q.c){sindex.erase(q.n+"."+c.n);nindex.erase(q.n+"."+c.n);}}
    size_t query_view_bytes()const{size_t n=0;for(auto&x:query_views)n+=x.first.capacity()+x.second.bytes();return n;}
    QueryView* query_view(Table&q,int col){
      const char*e=getenv("LITE_QUERY_VIEW_MB");size_t cap=(e?size_t(std::max(0,std::atoi(e))):8)<<20;
      while(!query_views.empty()&&query_view_bytes()>cap)query_views.erase(query_views.begin());
      size_t overhead=sizeof(QueryView)+256+q.n.capacity();if(cap<overhead||q.count>(cap-overhead)/sizeof(VectorRef))return nullptr;
      auto it=query_views.find(q.n);if(it!=query_views.end()&&it->second.col==col&&it->second.version==view_version){it->second.used=++view_clock;return &it->second;}
      query_views.erase(q.n);size_t needed=overhead+q.count*sizeof(VectorRef);
      while(!query_views.empty()&&query_view_bytes()>cap-needed){auto victim=std::min_element(query_views.begin(),query_views.end(),[](const auto&a,const auto&b){return a.second.used<b.second.used;});query_views.erase(victim);}
      QueryView v;v.col=col;v.version=view_version;v.used=++view_clock;v.rows.reserve(q.count);auto p=prefix(q);auto c=scan(p,lsm::upper(p));
      while(c->valid){lsm::Decode d{c->row.value};VectorRef r{(long long)d.u64(),-1};d.u64();for(size_t i=1;i<q.c.size();++i){size_t n=d.u32();d.need(n);d.p+=n;off_t off=(off_t)d.u64();if(q.c[i].t=="VECTOR"&&(off<0||uint64_t(off)+uint64_t(q.c[i].d)*4>(uint64_t)vec_end))throw Err("corruption","query view vector offset");if((int)i==col)r.off=off;}d.end();
        if(c->row.key!=skey(q,r.id)||(!v.rows.empty()&&r.id<=v.rows.back().id)||v.rows.size()>=q.count)throw Err("corruption","query view row identity/count");v.rows.push_back(r);c->next();}
      if(v.rows.size()!=q.count)throw Err("corruption","query view row count");if(v.rows.capacity()*sizeof(VectorRef)+overhead>cap)return nullptr;return &query_views.emplace(q.n,std::move(v)).first->second;
    }
    const std::vector<uint32_t>* query_postings(Table&q,QueryView&v,int col,const std::string&key){
      if(v.rejected_col==col||v.rows.size()>UINT32_MAX)return nullptr;if(v.post_col!=col){v.rejected_col=col;const char*e=getenv("LITE_QUERY_VIEW_MB");size_t cap=(e?size_t(std::max(0,std::atoi(e))):8)<<20;size_t held=query_view_bytes();if(held>=cap)return nullptr;size_t available=cap-held,bytes=0;std::map<std::string,std::vector<uint32_t>> posts;
        auto p=index_prefix(q,col);auto c=scan(p,lsm::upper(p));while(c->valid){const auto&k=c->row.key;if(k.size()<p.size()+8)throw Err("corruption","query posting key");std::string value=k.substr(p.size(),k.size()-p.size()-8);auto it=posts.find(value);
          if(it==posts.end()){size_t n=std::max<size_t>(15,value.capacity())+sizeof(decltype(posts)::value_type)+128;if(n>available-bytes)return nullptr;bytes+=n;it=posts.emplace(std::move(value),std::vector<uint32_t>{}).first;}
          auto&list=it->second;if(list.size()==list.capacity()){size_t next=std::max<size_t>(4,list.capacity()*2),n=(next-list.capacity())*sizeof(uint32_t);if(n>available-bytes)return nullptr;bytes+=n;list.reserve(next);}
          long long id=(long long)(lsm::number_value(k,k.size()-8)^(1ull<<63));auto row=std::lower_bound(v.rows.begin(),v.rows.end(),id,[](const VectorRef&r,long long x){return r.id<x;});if(row==v.rows.end()||row->id!=id)throw Err("corruption","query posting row");list.push_back((uint32_t)(row-v.rows.begin()));c->next();}
        v.posts=std::move(posts);v.post_bytes=bytes;v.post_col=col;v.rejected_col=-1;
      }static const std::vector<uint32_t> empty;auto it=v.posts.find(key);return it==v.posts.end()?&empty:&it->second;
    }
    bool ensure_hot(Table&q){
      if(q.hot)return true;
      const char*e=std::getenv("LITE_ROW_CACHE_MB");size_t cap=e?size_t(std::max(0,std::atoi(e)))<<20:32u<<20;
      uint64_t estimate=q.row_bytes+q.count*(384+uint64_t(q.c.size())*160);if(estimate>cap)return false;
      for(auto&t:tabs)if(t.second.hot)cool(t.second);
      auto p=prefix(q);auto c=scan(p,lsm::upper(p));size_t bytes=0;
      while(c->valid){auto r=decode_row(q,c->row.value);bytes+=c->row.value.size()+384+q.c.size()*160;if(bytes>cap){cool(q);return false;}q.order.push_back(r.id);q.r.emplace(r.id,std::move(r));c->next();}
      if(q.r.size()!=q.count)throw Err("corruption","catalog row count mismatch");
      std::sort(q.order.begin(),q.order.end(),[&](long long a,long long b){return q.r.at(a).ordinal<q.r.at(b).ordinal;});q.hot=true;rebuild_scalar(q);return true;
    }
    void begin(bool=true){if(in)throw Err("transaction_active","already active");tx=store->batch();vec_mark=vec_end;in=true;}
    void rollback(){if(failed_io||store->failed)throw Err("io_error","restart required after failed commit");if(!in)return;statement.reset();tx.reset();vecbuf.clear();trunc_vec(vec_mark);vecbuf_base=vec_end;in=false;load_catalog();}
    void commit(){if(!in)return;try{if(!tx->empty()){flush_vecbuf();if(!store->vector_wal){sync_commit(vfd);store->vector_synced=(uint64_t)vec_end;}store->commit(*tx,(uint64_t)vec_end);}tx.reset();in=false;vcache.clear();}catch(...){failed_io=true;throw;}}
    void flush_sst(){if(failed_io||store->failed)throw Err("io_error","restart required after failed commit");try{store->flush();}catch(...){failed_io=true;throw;}}
    void compact(){if(in)throw Err("transaction_active","maintenance during transaction");try{store->compact(true);}catch(...){failed_io=true;throw;}}
    void checkpoint(){if(failed_io||store->failed)throw Err("io_error","restart required after failed commit");if(in)throw Err("transaction_active","checkpoint during transaction");try{store->checkpoint();}catch(...){failed_io=true;throw;}}
    void gc(){
      if(in)throw Err("transaction_active","GC during transaction");checkpoint();
      std::string name=store->fresh("vectors");std::string path=dir+"/"+name;lsm::FD vf(open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL,0644));lsm::io(vf.n>=0,"create vector generation");off_t end=0;
      auto c=store->scan();std::unique_ptr<lsm::Writer>w;std::vector<lsm::F> out;
      try{while(c->valid){SRec r=c->row;if(!r.key.empty()&&r.key[0]=='R'){
          // Row keys are schema-qualified; locate the small catalog entry.
          Table*q=nullptr;for(auto&t:tabs){auto p=prefix(t.second);if(r.key.compare(0,p.size(),p)==0){q=&t.second;break;}}if(!q)throw Err("corruption","orphan row schema");
          Row row=decode_row(*q,r.value);for(size_t i=1;i<q->c.size();++i)if(q->c[i].t=="VECTOR"){auto v=read_vec(row.voff[i],q->c[i].d);lsm::write_all(vf.n,(const char*)v.data(),v.size()*4);row.voff[i]=end;end+=v.size()*4;}r.value=row_payload(*q,row);
        }if(!w)w=std::make_unique<lsm::Writer>(store->allocate(1));w->add(r);if(w->file->bytes+w->block.size()>=(size_t)envint("LITE_SST_BYTES",8<<20)){out.push_back(w->finish());w.reset();}c->next();}
        if(w)out.push_back(w->finish());lsm::sync(vf.n);store->replace(out,name,end);
        clear_maps();close(vfd);vecfile=path;vfd=open(path.c_str(),O_RDWR|O_APPEND);lsm::io(vfd>=0,"open compacted vectors");store->vfd=vfd;vec_end=end;vecbuf_base=end;load_catalog();
      }catch(...){failed_io=true;throw;}
    }
    J exec(const std::string&sql){
      if(failed_io||store->failed)throw Err("io_error","restart required after failed commit");auto t=tok(sql);if(t.empty())throw Err("invalid_request","empty");std::string k=up(t[0]);
      if(k=="BEGIN"){begin();return jo();}if(k=="COMMIT"){commit();return jo();}if(k=="ROLLBACK"){rollback();return jo();}
      if(k=="SELECT"){try{flush_vecbuf();return select(t);}catch(const Err&e){if(e.code=="io_error"||e.code=="corruption")failed_io=true;throw;}}
      bool autocommit=!in;if(autocommit)begin();flush_vecbuf();off_t mark=vec_end;statement=store->batch();
      try{J result=dispatch(t);std::string o;lsm::u64(o,ordinal);statement->put("O",std::move(o));
        // The statement is private until every row has passed validation.
        try{tx->absorb(*statement);}catch(...){failed_io=true;throw;}statement.reset();if(autocommit)commit();return result;
      }catch(const Err&e){if(e.code=="io_error"||e.code=="corruption")failed_io=true;if(failed_io||store->failed)throw;statement.reset();vecbuf.clear();trunc_vec(mark);vecbuf_base=vec_end;load_catalog();if(autocommit)rollback();throw;}
      catch(...){if(failed_io||store->failed)throw;statement.reset();vecbuf.clear();trunc_vec(mark);vecbuf_base=vec_end;load_catalog();if(autocommit)rollback();throw;}
    }
    J dispatch(const std::vector<std::string>&t){auto k=up(t.at(0));if(k=="CREATE"&&t.size()>1&&up(t[1])=="VECTOR")return create_index(t);if(k=="CREATE")return create(t,true);if(k=="INSERT")return insert(t,true);if(k=="UPDATE")return update(t,true);if(k=="DELETE")return erase(t,true);throw Err("unsupported_sql","statement");}
    static bool inttok(const std::string&x){
      size_t p=!x.empty()&&(x[0]=='-'||x[0]=='+');
      if(p==x.size())return false;
      for(;p<x.size();++p)if(!std::isdigit((unsigned char)x[p]))return false;
      return true;
    }
    static long long integer(const std::string&s){
      if(!inttok(s))throw Err("invalid_value","BIGINT requires an integer");
      try{return std::stoll(s);}catch(...){throw Err("invalid_value","BIGINT out of range");}
    }
    static void normalize_scalar(const Col&c,std::string&v){
      if(c.t=="BIGINT")v=std::to_string(integer(v));
      else if(c.t=="VARCHAR"){
        if(v.size()<2||v.front()!='\''||v.back()!='\'')throw Err("invalid_value","VARCHAR requires a string");
        size_t chars=0;for(size_t i=1;i+1<v.size();++i)if(((unsigned char)v[i]&0xc0)!=0x80)++chars;
        if(chars>(size_t)c.d)throw Err("invalid_value","VARCHAR length");
      }
    }
    static void put32(std::string&o,uint32_t v){
      for(int i=0;i<4;++i)o.push_back((char)(v>>(i*8)));
    }
    static void put64(std::string&o,uint64_t v){
      for(int i=0;i<8;++i)o.push_back((char)(v>>(i*8)));
    }
    static bool get32(const std::string&s,size_t&p,uint32_t&v){
      if(p+4>s.size())return false;
      v=(unsigned char)s[p]|((uint32_t)(unsigned char)s[p+1]<<8)|
        ((uint32_t)(unsigned char)s[p+2]<<16)|((uint32_t)(unsigned char)s[p+3]<<24);
      p+=4;
      return true;
    }
    static bool get64(const std::string&s,size_t&p,uint64_t&v){
      if(p+8>s.size())return false;
      v=0;
      for(int i=0;i<8;++i)v|=(uint64_t)(unsigned char)s[p+i]<<(i*8);
      p+=8;
      return true;
    }
    bool decode_tokens(const std::string&s,std::vector<std::string>&t)const{
      if(s.size()<5)return false;
      uint8_t op=(uint8_t)s[0];
      uint32_t n=0;
      size_t p=1;
      if(op<1||op>5||!get32(s,p,n)||n==0||n>(s.size()-5)/5)return false;
      t.clear();
      for(uint32_t i=0;i<n;++i){
        if(p>=s.size())return false;
        uint8_t tag=(uint8_t)s[p++];
        if(tag==1){
          uint64_t z=0;
          if(!get64(s,p,z))return false;
          t.push_back(std::to_string((long long)z));
        }
        else if(tag>=2&&tag<=4){
          uint32_t z=0;
          if(!get32(s,p,z)||z>(1u<<20)||p+z>s.size())return false;
          t.emplace_back(s.data()+p,z);
          p+=z;
        }
        else return false;
      }
      if(p!=s.size()||t.empty())return false;
      std::string k=up(t[0]);
      if((op==1||op==2)&&k!="CREATE")return false;
      if(op==3&&k!="INSERT")return false;
      if(op==4&&k!="UPDATE")return false;
      if(op==5&&k!="DELETE")return false;
      return true;
    }
    int ci(const Table&q,const std::string&n)const{
      for(size_t i=0;i<q.c.size();++i)if(up(q.c[i].n)==up(n))return (int)i;
      return -1;
    }
    static void parse_into(const std::string&s,int want,std::vector<float>&v){
      std::string z=unq(s);
      if(z.size()<2||z.front()!='['||z.back()!=']')throw Err("invalid_value","vector");
      v.clear();
      if(want>0)v.reserve(want);
      const char*p=z.c_str()+1;
      const char*e=z.c_str()+z.size()-1;
      const char*setting=std::getenv("LITE_FAST_PARSE");bool fast=!setting||std::atoi(setting)!=0;
      while(p<e){
        while(p<e&&std::isspace((unsigned char)*p))++p;
        if(p>=e)break;
        char*tail=nullptr;double val=0;
        // Keep strtod's leading-plus and range/underflow behavior on the fallback.
        if(fast&&*p!='+'){auto result=std::from_chars(p,e,val,std::chars_format::general);if(result.ec==std::errc{})tail=const_cast<char*>(result.ptr);}
        if(!tail)val=std::strtod(p,&tail);
        if(tail==p||tail>e)throw Err("invalid_value","vector number");
        for(const char*q=p;q<tail;++q)if(!std::isdigit((unsigned char)*q)&&*q!='.'&&*q!='e'&&*q!='E'&&*q!='+'&&*q!='-')throw Err("invalid_value","vector number");
        p=tail;float x=(float)val;
        if(!std::isfinite(x))throw Err("invalid_value","nonfinite vector");
        v.push_back(x);
        while(p<e&&std::isspace((unsigned char)*p))++p;
        if(p<e){if(*p++!=',')throw Err("invalid_value","vector separator");
          while(p<e&&std::isspace((unsigned char)*p))++p;
          if(p==e)throw Err("invalid_value","trailing vector comma");}
      }
      if(want>=0&&(int)v.size()!=want)throw Err("dimension_mismatch","vector dimension");
    }
    std::vector<float> vec(const std::string&s,int want=-1)const{
      std::vector<float>v;
      parse_into(s,want,v);
      return v;
    }
    off_t append_vec(const std::string&s,int d){
      if(vecbuf.size()>=(1u<<20))flush_vecbuf();
      auto it=vcache.find(s);
      off_t off=vec_end;
      if(vecbuf.empty())vecbuf_base=vec_end;
      if(it!=vcache.end()&&it->second.size()==size_t(d*4)){
        vecbuf.append(it->second);
        vec_end+=it->second.size();
        return off;
      }
      parse_into(s,d,scratch);
      std::string raw((char*)scratch.data(),scratch.size()*4);
      if(vcache.size()<256&&s.size()<8192)vcache.emplace(s,raw);
      vecbuf.append(raw);
      vec_end+=raw.size();
      return off;
    }
    void clear_maps()const{for(auto&w:vector_maps)munmap(w.ptr,w.bytes);vector_maps.clear();vmap=nullptr;mapsz=0;mapoff=0;}
    size_t mapped_bytes()const{size_t n=0;for(auto&w:vector_maps)n+=w.bytes;return n;}
    void map_window(off_t off,size_t need)const{
      if(vmap&&off>=mapoff&&size_t(off-mapoff)+need<=mapsz)return;
      vmap=nullptr;mapsz=0;mapoff=0;for(auto&w:vector_maps)if(off>=w.off&&size_t(off-w.off)+need<=w.bytes){w.used=++map_clock;vmap=w.ptr;mapsz=w.bytes;mapoff=w.off;return;}
      if(vec_end&&vfd>=0){size_t win=(size_t)envint("LITE_MMAP_MB",32)<<20;off_t base=(off/(off_t)win)*(off_t)win;size_t len=(size_t)std::min<off_t>(std::max<size_t>(win,size_t(off-base)+need),(off_t)vec_end-base);
        const char*e=getenv("LITE_MMAP_KEEP_MB");size_t keep=(e?size_t(std::max(0,std::atoi(e))):1024)<<20;
        for(auto it=vector_maps.begin();it!=vector_maps.end();)if(it->off==base){munmap(it->ptr,it->bytes);it=vector_maps.erase(it);}else ++it;
        while(!vector_maps.empty()&&mapped_bytes()+len>std::max(keep,len)){auto it=std::min_element(vector_maps.begin(),vector_maps.end(),[](const MapWindow&a,const MapWindow&b){return a.used<b.used;});munmap(it->ptr,it->bytes);vector_maps.erase(it);}
        void*p=mmap(nullptr,len,PROT_READ,MAP_SHARED,vfd,base);++map_calls;if(p!=MAP_FAILED){vector_maps.push_back({p,base,len,++map_clock});vmap=p;mapsz=len;mapoff=base;}
      }
    }
    void ensure_map()const{map_window(0,1);}
    const float* vptr(off_t off,int d)const{
      if(off<0)return nullptr;
      if(off>=vecbuf_base&&size_t(off-vecbuf_base+d*4)<=vecbuf.size())return (const float*)(vecbuf.data()+off-vecbuf_base);
      if(vfd>=0){map_window(off,d*4);if(vmap&&off>=mapoff&&size_t(off-mapoff+d*4)<=mapsz)return (const float*)((const char*)vmap+off-mapoff);}
      return nullptr;
    }
    double l2p(const float*p,int d,const std::vector<float>&q)const{
      if(!p)return 0;
      if(d>=64&&has_avx512())return l2_avx512(p,q.data(),d);
      if(d>=32&&has_avx2())return l2_avx2(p,q.data(),d);
      double z=0;for(int i=0;i<d;++i){double x=double(p[i])-double(q[i]);z+=x*x;}return z;
    }
    double l2(off_t off,int d,const std::vector<float>&q)const{
      if(off<0)return 0;
      const float*p=vptr(off,d);
      if(p){
        if(d>=64&&has_avx512())return l2_avx512(p,q.data(),d);
        if(d>=32&&has_avx2())return l2_avx2(p,q.data(),d);
        double z=0;
        for(int i=0;i<d;++i){
          double x=double(p[i])-double(q[i]);
          z+=x*x;
        }
        return z;
      }
      if(vfd>=0){
        std::vector<float>v(d);
        auto bytes=lsm::read_at(vfd,off,(size_t)d*4);std::memcpy(v.data(),bytes.data(),bytes.size());
        if(d>=64&&has_avx512())return l2_avx512(v.data(),q.data(),d);
        if(d>=32&&has_avx2())return l2_avx2(v.data(),q.data(),d);
        double z=0;
        for(int i=0;i<d;++i){
          double x=double(v[i])-double(q[i]);
          z+=x*x;
        }
        return z;
      }
      return 0;
    }
    double l2_limit(const float*p,off_t off,int d,const std::vector<float>&q,double lim,int&used)const{
      used=d;if(!p)p=vptr(off,d);if(!p)return l2(off,d,q);
      if(d>=64&&has_avx512())return l2_avx512<true>(p,q.data(),d,lim,&used);
      if(d>=32&&has_avx2())return l2_avx2<true>(p,q.data(),d,lim,&used);
      double z=0;for(int i=0;i<d;++i){double x=double(p[i])-double(q[i]);z+=x*x;}
      return z;
    }
    std::vector<float> read_vec(off_t off,int d)const{
      std::vector<float>v(d);
      if(off<0)return v;
      if(const float*p=vptr(off,d)){std::memcpy(v.data(),p,d*4);return v;}
      if(vfd<0)return v;
      auto bytes=lsm::read_at(vfd,off,(size_t)d*4);std::memcpy(v.data(),bytes.data(),bytes.size());
      return v;
    }
    void ensure_matrix(Table&q,int c){
      if(c<=0||c>=(int)q.c.size()||q.c[c].t!="VECTOR")return;
      const char*setting=std::getenv("LITE_MATRIX_MB");size_t lim=(size_t)(setting?std::max(0,std::atoi(setting)):64)<<20;
      size_t bytes=q.order.size()*(size_t)q.c[c].d*sizeof(float);if(!lim)return;
      auto it=matrices.find(q.n);if(it!=matrices.end()&&it->second.col==c&&it->second.values*sizeof(float)==bytes)return;
      if(bytes>lim)return;
      if(q.fast.size()!=q.order.size())refresh_fast(q);
      Matrix m;m.col=c;m.dim=q.c[c].d;m.values=q.fast.size()*(size_t)m.dim;
      bool direct=!q.fast.empty()&&q.fast[0]&&m.values<=size_t(INT32_MAX/4)&&(!getenv("LITE_MATRIX_VIEW")||std::atoi(getenv("LITE_MATRIX_VIEW"))!=0);off_t first=direct?q.fast[0]->voff[c]:-1;
      for(size_t i=0;direct&&i<q.fast.size();++i)if(!q.fast[i]||q.fast[i]->voff[c]!=first+off_t(i)*m.dim*4)direct=false;
      const float*source=direct?vptr(first,(int)m.values):nullptr;if(source)m.mapped_off=first;else{m.v.resize(m.values);
        for(size_t i=0;i<q.fast.size();++i)if(q.fast[i]){const float*p=vptr(q.fast[i]->voff[c],m.dim);if(p)std::memcpy(m.v.data()+i*m.dim,p,m.dim*sizeof(float));
          else{auto x=read_vec(q.fast[i]->voff[c],m.dim);std::memcpy(m.v.data()+i*m.dim,x.data(),m.dim*sizeof(float));}}source=m.v.data();
      }
      if(m.dim==128&&has_avx512()&&__builtin_cpu_supports("avx512bw")&&(_mm_getcsr()&0xe040u)==0&&m.values<=(2u<<20)&&bytes+m.values<=lim&&(!std::getenv("LITE_DISTANCE_SCREEN")||std::atoi(std::getenv("LITE_DISTANCE_SCREEN")))){
        double bound=0;for(size_t i=0;i<m.values;++i)bound=std::max(bound,std::abs(double(source[i])));m.scale=bound/127;
        if(m.scale>0){m.codes.resize(m.values);for(size_t i=0;i<m.values;++i)m.codes[i]=(int8_t)std::llround(double(source[i])/m.scale);
          if(__builtin_cpu_supports("avx512vnni")&&bytes+2*m.codes.size()+q.fast.size()*4+2112<=lim&&(!std::getenv("LITE_SCREEN_DOT")||std::atoi(std::getenv("LITE_SCREEN_DOT"))))screen_pack128(m);}
      }
      matrices[q.n]=std::move(m);
    }
    const float* matrix_ptr(const Table&q,int c,long long id)const{
      auto m=matrices.find(q.n);if(m==matrices.end()||m->second.col!=c)return nullptr;
      auto p=fast_pos.find(q.n);if(p==fast_pos.end())return nullptr;auto i=p->second.find(id);if(i==p->second.end())return nullptr;
      size_t off=i->second*(size_t)m->second.dim;if(off+m->second.dim>m->second.values)return nullptr;return m->second.mapped_off>=0?vptr(m->second.mapped_off+off*4,m->second.dim):m->second.v.data()+off;
    }
    bool match(const Table&q,const Row&r,const std::vector<Cond>&cs,size_t proven=std::numeric_limits<size_t>::max())const{
      for(size_t j=0;j<cs.size();++j){
        if(j==proven)continue;
        const auto&c=cs[j];
        std::string a=c.col==0?std::to_string(r.id):unq(r.v[c.col-1]),b=unq(c.val);
        bool num=q.c[c.col].t=="BIGINT";
        if(num){
          long long x=std::stoll(a),y=std::stoll(b);
          if(c.op=="="&&x!=y)return false;
          if((c.op=="!="||c.op=="<>")&&x==y)return false;
          if(c.op==">"&&x<=y)return false;
          if(c.op=="<"&&x>=y)return false;
          if(c.op==">="&&x<y)return false;
          if(c.op=="<="&&x>y)return false;
        }
        else{
          if(c.op=="="&&a!=b)return false;
          if((c.op=="!="||c.op=="<>")&&a==b)return false;
          if(c.op==">"&&a<=b)return false;
          if(c.op=="<"&&a>=b)return false;
          if(c.op==">="&&a<b)return false;
          if(c.op=="<="&&a>b)return false;
        }
      }
      return true;
    }
    std::vector<Cond> conditions(const Table&q,const std::vector<std::string>&t,size_t a,size_t z)const{
      std::vector<Cond>r;
      while(a<z){
        if(up(t[a])=="AND"){
          ++a;
          continue;
        }
        if(a+2>=z)throw Err("invalid_request","where");
        Cond c;
        c.col=ci(q,t[a++]);
        c.op=t[a++];
        c.val=t[a++];
        if(c.col<0)throw Err("unknown_column","where");
        if(q.c[c.col].t=="BIGINT")c.val=std::to_string(integer(unq(c.val)));
        if(c.op!="="&&c.op!="!="&&c.op!="<>"&&c.op!="<"&&c.op!=">"&&c.op!="<="&&c.op!=">=")throw Err("unsupported_sql","comparison");
        r.push_back(c);
      }
      return r;
    }
    void scalar_add(Table&q,long long id){
      auto&row=q.r[id];
      sindex[q.n+"."+q.c[0].n][std::to_string(id)].push_back(id);nindex[q.n+"."+q.c[0].n][id].push_back(id);
      for(size_t c=1;c<q.c.size();++c)if(q.c[c].t!="VECTOR"){
        std::string k=q.n+"."+q.c[c].n,v=unq(row.v[c-1]);
        sindex[k][v].push_back(id);
        if(q.c[c].t=="BIGINT")nindex[k][std::stoll(v)].push_back(id);
      }
    }
    void scalar_remove(Table&q,long long id){
      if(!q.r.count(id))return;
      auto&row=q.r[id];
      auto&iv=sindex[q.n+"."+q.c[0].n][std::to_string(id)];iv.erase(std::remove(iv.begin(),iv.end(),id),iv.end());auto&iw=nindex[q.n+"."+q.c[0].n][id];iw.erase(std::remove(iw.begin(),iw.end(),id),iw.end());
      for(size_t c=1;c<q.c.size();++c)if(q.c[c].t!="VECTOR"){
        std::string k=q.n+"."+q.c[c].n,v0=unq(row.v[c-1]);
        auto&v=sindex[k][v0];
        v.erase(std::remove(v.begin(),v.end(),id),v.end());
        if(q.c[c].t=="BIGINT"){auto&w=nindex[k][std::stoll(v0)];w.erase(std::remove(w.begin(),w.end(),id),w.end());}
      }
    }
    void refresh_fast(Table&q){
      if(hot_posts.table==q.n)hot_posts={};
      q.fast.clear();
      q.fast.reserve(q.order.size());
      for(auto id:q.order){
        auto it=q.r.find(id);
        q.fast.push_back(it==q.r.end()?nullptr:&it->second);
      }
      int vc=-1;for(size_t i=1;i<q.c.size();++i)if(q.c[i].t=="VECTOR"){vc=(int)i;break;}
      if(vc>0)std::sort(q.fast.begin(),q.fast.end(),[vc](const Row*a,const Row*b){
        if(!a||!b)return a!=nullptr;
        off_t x=vc<(int)a->voff.size()?a->voff[vc]:-1,y=vc<(int)b->voff.size()?b->voff[vc]:-1;
        if(x!=y)return x<y;return a->id<b->id;
      });
      auto&pos=fast_pos[q.n];pos.clear();pos.reserve(q.fast.size()*2+1);
      for(size_t i=0;i<q.fast.size();++i)if(q.fast[i])pos[q.fast[i]->id]=i;
    }
    void rebuild_scalar(Table&q){
      if(hot_posts.table==q.n)hot_posts={};
      q.fast.clear();
      fast_pos.erase(q.n);matrices.erase(q.n);vblocks.erase(q.n);
      sindex.erase(q.n+"."+q.c[0].n);nindex.erase(q.n+"."+q.c[0].n);
      for(size_t c=1;c<q.c.size();++c)if(q.c[c].t!="VECTOR"){sindex.erase(q.n+"."+q.c[c].n);nindex.erase(q.n+"."+q.c[c].n);}
      for(auto&kv:q.r)scalar_add(q,kv.first);
    }
    const std::vector<size_t>* hot_postings(const Table&q,const Cond&c){
      const char*e=std::getenv("LITE_HOT_POSTINGS_KB");size_t cap=size_t(e?std::max(0,std::atoi(e)):1024)<<10;
      if(!cap){hot_posts={};return nullptr;}
      if(hot_posts.table!=q.n||hot_posts.col!=c.col||hot_posts.cap!=cap){
        hot_posts={};hot_posts.table=q.n;hot_posts.col=c.col;hot_posts.cap=cap;++hot_post_builds;
        size_t bytes=sizeof(HotPostings)+q.n.capacity();bool fits=bytes<=cap;
        for(size_t p=0;fits&&p<q.fast.size();++p)if(q.fast[p]){
          const Row&r=*q.fast[p];std::string key=c.col?unq(r.v[c.col-1]):std::to_string(r.id);
          auto it=hot_posts.slots.find(key);
          if(it==hot_posts.slots.end()){size_t need=128+key.size();if(need>cap-bytes){fits=false;break;}bytes+=need;it=hot_posts.slots.emplace(std::move(key),std::vector<size_t>{}).first;}
          auto&v=it->second;if(v.size()==v.capacity()){size_t next=std::max<size_t>(4,v.capacity()*2),need=(next-v.capacity())*sizeof(size_t);if(need>cap-bytes){fits=false;break;}bytes+=need;v.reserve(next);}v.push_back(p);
        }
        if(fits){hot_posts.bytes=bytes;hot_posts.ready=true;}else hot_posts.slots.clear();
      }
      if(!hot_posts.ready)return nullptr;static const std::vector<size_t> empty;
      auto it=hot_posts.slots.find(unq(c.val));return it==hot_posts.slots.end()?&empty:&it->second;
    }
    void build_vblocks(Table&q,int bc){
      auto&bs=vblocks[q.n];
      if(!bs.empty()&&vblock_col[q.n]==bc&&bs[0].lo.size()==(size_t)q.c[bc].d)return;
      bs.clear();vblock_col[q.n]=bc;
      if(q.fast.size()!=q.order.size())refresh_fast(q);
      int d=q.c[bc].d,sd=d;
      for(size_t b=0;b<q.order.size();b+=64){
        VBlock w;w.begin=b;w.end=std::min(q.order.size(),b+64);w.lo.assign(sd,std::numeric_limits<float>::infinity());w.hi.assign(sd,-std::numeric_limits<float>::infinity());
        bool any=false;for(size_t p=w.begin;p<w.end;++p)if(p<q.fast.size()&&q.fast[p]){
          const float*v=vptr(q.fast[p]->voff[bc],d);std::vector<float>fallback;if(!v){fallback=read_vec(q.fast[p]->voff[bc],d);v=fallback.data();}
          any=true;
          for(int j=0;j<sd;++j){int x=(j*d)/sd;w.lo[j]=std::min(w.lo[j],v[x]);w.hi[j]=std::max(w.hi[j],v[x]);}
        }
        w.center.assign(sd,0);double rr=0;if(any)for(int j=0;j<sd;++j){w.center[j]=(w.lo[j]+w.hi[j])*.5f;double e=(w.hi[j]-w.lo[j])*.5;rr+=e*e;}w.radius=(float)std::sqrt(rr);bs.push_back(std::move(w));
      }
    }
    void rebuild_index(Table&q){vblocks.erase(q.n);}
    J create_index(const std::vector<std::string>&t){
      if(t.size()!=9||up(t[1])!="VECTOR"||up(t[2])!="INDEX"||up(t[4])!="ON"||t[8]!=")")throw Err("unsupported_sql","index");
      if(indexes.count(t[3]))throw Err("duplicate_index","index");
      auto&q=tabs.at(t[5]);
      if(t[6]!="(")throw Err("invalid_request","index");
      int c=ci(q,t[7]);
      if(c<0||q.c[c].t!="VECTOR")throw Err("unsupported_type","index column");
      indexes.insert(t[3]);
      index_cols[q.n]=c;
      rebuild_index(q);
      std::string s;lsm::str(s,t[3]);lsm::str(s,q.n);lsm::u32(s,(uint32_t)c);statement->put("X"+lsm::part(t[3]),std::move(s));
      s.clear();lsm::str(s,q.n);lsm::u32(s,(uint32_t)c);statement->put("V"+lsm::part(q.n),std::move(s));
      return jo();
    }
    J create(const std::vector<std::string>&t,bool rec){
      if(t.size()<5||up(t[1])!="TABLE")throw Err("unsupported_sql","create");
      std::string name=t[2];
      if(tabs.count(name))throw Err("table_exists","table");
      Table q;
      q.n=name;
      for(size_t i=4;i<t.size()&&t[i]!=")";){
        if(t[i]==","){
          ++i;
          continue;
        }
        if(up(t[i])=="PRIMARY"){
          i+=3;
          continue;
        }
        if(i+1>=t.size())throw Err("invalid_request","column");
        Col c;
        c.n=t[i++];
        c.t=up(t[i++]);
        if(c.t=="VARCHAR"||c.t=="VECTOR"){
          if(i+2>=t.size()||t[i]!="(")throw Err("invalid_request","type");
          if(!inttok(t[i+1]))throw Err("invalid_request","type length");
          c.d=std::stoi(t[i+1]);
          if(c.d<=0)throw Err("invalid_value","type length must be positive");
          i+=3;
        }
        else if(c.t!="BIGINT")throw Err("unsupported_type","type");
        q.c.push_back(c);
      }
      if(q.c.empty()||q.c.front().t!="BIGINT")throw Err("invalid_request","BIGINT primary key required");
      if(q.c.size()>1024)throw Err("invalid_request","at most 1024 columns");
      tabs[name]=q;
      (void)rec;statement->put(catalog_key(q),catalog(q));
      return jo();
    }

    J insert(const std::vector<std::string>&t,bool){
      if(t.size()<6||up(t[1])!="INTO"||up(t[3])!="VALUES")throw Err("unsupported_sql","insert");auto&q=tabs.at(t[2]);cool(q);
      std::vector<std::vector<std::string>>rows;
      for(size_t p=4;p<t.size();){if(t[p++]!="(")throw Err("invalid_request","values");std::vector<std::string>v;while(p<t.size()&&t[p]!=")"){if(t[p]==","){++p;continue;}v.push_back(t[p++]);}if(p==t.size())throw Err("invalid_request","values");++p;if(v.size()!=q.c.size())throw Err("column_count","values");rows.push_back(std::move(v));while(p<t.size()&&t[p]!="(")++p;}
      std::set<long long>seen;
      for(auto&v:rows){long long id=integer(v[0]);v[0]=std::to_string(id);Row r;if(row_get(q,id,r)||!seen.insert(id).second)throw Err("duplicate_key","key");for(size_t c=1;c<v.size();++c)normalize_scalar(q.c[c],v[c]);}
      // Vector validation and encoding happen together in append_vec. A failed
      // row discards the private statement and truncates only its vector tail.
      for(auto&v:rows){Row r;r.id=integer(v[0]);r.ordinal=++ordinal;r.v.assign(v.begin()+1,v.end());r.voff.assign(q.c.size(),-1);
        for(size_t c=1;c<v.size();++c)if(q.c[c].t=="VECTOR"){if(recovering&&v[c]=="@"){r.voff[c]=vec_cursor;vec_cursor+=q.c[c].d*4;}else r.voff[c]=append_vec(v[c],q.c[c].d);r.v[c-1].clear();}row_put(q,&r,nullptr);}
      statement->put(catalog_key(q),catalog(q));J x=jo();x.o["affected_rows"]=ji(rows.size());return x;
    }
    template<class Fn> void each_row(const Table&q,const std::vector<Cond>&cs,Fn fn)const{
      // Primary equality needs no range iterator or row-cache materialization.
      for(auto&c:cs)if(c.col==0&&c.op=="="){Row r;if(row_get(q,integer(c.val),r)&&match(q,r,cs))fn(r);return;}
      std::string lo=prefix(q),hi=lsm::upper(lo);int secondary_col=-1;const char*mode=getenv("LITE_LSM_READ");bool sequential=mode&&std::string(mode)=="scan";
      for(auto&c:cs)if(!sequential&&c.col>0&&q.c[c.col].t!="VECTOR"&&c.op=="="){lo=index_prefix(q,c.col)+scalar_key(q.c[c.col],c.val);hi=lsm::upper(lo);secondary_col=c.col;break;}
      if(secondary_col<0)for(auto&c:cs)if(c.col==0&&c.op!="!="){auto k=skey(q,integer(c.val));if(c.op==">=")lo=std::max(lo,k);else if(c.op==">")lo=std::max(lo,lsm::upper(k));else if(c.op=="<")hi=std::min(hi,k);else if(c.op=="<=")hi=std::min(hi,lsm::upper(k));}
      if(!hi.empty()&&lo>=hi)return;auto c=scan(lo,hi);while(c->valid){
        if(secondary_col<0){auto r=decode_row(q,c->row.value);if(match(q,r,cs))fn(r);c->next();continue;}
        std::vector<std::string> keys;keys.reserve(1024);while(c->valid&&keys.size()<1024){long long id=(long long)(lsm::number_value(c->row.key,c->row.key.size()-8)^(1ull<<63));keys.push_back(skey(q,id));c->next();}
        size_t count=0;auto rows=rows_get(keys);while(rows->valid){auto r=decode_row(q,rows->row.value);if(match(q,r,cs))fn(r);++count;rows->next();}if(count!=keys.size())throw Err("corruption","dangling scalar index");
      }
    }
    J update(const std::vector<std::string>&t,bool){
      if(t.size()<6||up(t[2])!="SET")throw Err("unsupported_sql","update");auto&q=tabs.at(t[1]);cool(q);size_t p=3;std::vector<std::pair<int,std::string>>as;
      while(p<t.size()&&up(t[p])!="WHERE"){if(t[p]==","){++p;continue;}int c=ci(q,t[p++]);if(c<0)throw Err("unknown_column","set");if(!c)throw Err("immutable_key","id");if(p>=t.size()||t[p++]!="="||p>=t.size())throw Err("invalid_request","assignment");std::string v=t[p++];normalize_scalar(q.c[c],v);if(q.c[c].t=="VECTOR"&&!(recovering&&v=="@"))parse_into(v,q.c[c].d,scratch);as.emplace_back(c,std::move(v));}
      std::vector<Cond>cs;if(p<t.size())cs=conditions(q,t,p+1,t.size());uint64_t n=0;
      each_row(q,cs,[&](const Row&before){Row r=before;for(auto&a:as){if(q.c[a.first].t=="VECTOR"){r.v[a.first-1].clear();if(recovering&&a.second=="@"){r.voff[a.first]=vec_cursor;vec_cursor+=q.c[a.first].d*4;}else r.voff[a.first]=append_vec(a.second,q.c[a.first].d);}else r.v[a.first-1]=a.second;}row_put(q,&r,&before);++n;});
      statement->put(catalog_key(q),catalog(q));J x=jo();x.o["affected_rows"]=ji(n);return x;
    }
    J erase(const std::vector<std::string>&t,bool){if(t.size()<3||up(t[1])!="FROM")throw Err("unsupported_sql","delete");auto&q=tabs.at(t[2]);cool(q);std::vector<Cond>cs;if(t.size()>3){if(up(t[3])!="WHERE")throw Err("invalid_request","delete predicate");cs=conditions(q,t,4,t.size());}uint64_t n=0;each_row(q,cs,[&](const Row&r){row_put(q,nullptr,&r);++n;});statement->put(catalog_key(q),catalog(q));J x=jo();x.o["affected_rows"]=ji(n);return x;}
    void migrate_legacy(){
      // The old STATE/WAL remain untouched until a new authoritative manifest
      // is durable. Interrupted imports have only unreachable generation files.
      recovering=true;uint64_t legacy_tx=0;off_t boundary=0;
      auto put=[&](std::string key,std::string value){SRec r{std::move(key),std::move(value),store->seq+1,false};store->apply(r);};
      auto import_row=[&](Table&q,Row&r){r.ordinal=++ordinal;std::string s=row_payload(q,r);q.row_bytes+=s.size();++q.count;put(skey(q,r.id),std::move(s));for(size_t c=1;c<q.c.size();++c)if(q.c[c].t!="VECTOR")put(secondary(q,r,(int)c),{});};
      std::ifstream f(dir+"/STATE",std::ios::binary);uint32_t magic=0;
      if(f.read((char*)&magic,4)&&magic==0x4c535432u){uint32_t ver=0,nt=0;int64_t end=0;
        if(!rd(f,ver)||(ver!=2&&ver!=3)||!rd(f,legacy_tx))throw Err("corruption","legacy state header");
        if(ver==3){if(!rd(f,end)||end<0||end>vec_end)throw Err("corruption","legacy vector boundary");boundary=end;}
        else{if(fsize(dir+"/wal.log"))throw Err("corruption","legacy v2 STATE has nonempty WAL");boundary=vec_end;}
        store->vector_end=boundary;if(!rd(f,nt)||nt>1000000u)throw Err("corruption","legacy table count");
        for(uint32_t i=0;i<nt;++i){Table q;uint32_t nc=0;uint64_t nr=0;if(!rs(f,q.n)||!rd(f,nc)||nc==0||nc>1024)throw Err("corruption","legacy schema");
          for(uint32_t j=0;j<nc;++j){Col c;if(!rs(f,c.n)||!rs(f,c.t)||!rd(f,c.d))throw Err("corruption","legacy column");q.c.push_back(std::move(c));}
          if(!rd(f,nr)||nr>1000000000ull)throw Err("corruption","legacy row count");for(uint64_t j=0;j<nr;++j){Row r;if(!rd(f,r.id))throw Err("corruption","legacy row");r.voff.push_back(-1);for(size_t c=1;c<q.c.size();++c){std::string v;int64_t off;if(!rs(f,v)||!rd(f,off))throw Err("corruption","legacy row field");r.v.push_back(std::move(v));r.voff.push_back((off_t)off);}import_row(q,r);}
          put(catalog_key(q),catalog(q));tabs.emplace(q.n,std::move(q));
        }
        uint32_t n=0;if(!rd(f,n)||n>1000000u)throw Err("corruption","legacy indexes");for(uint32_t i=0;i<n;++i){std::string s;if(!rs(f,s))throw Err("corruption","legacy index name");indexes.insert(s);}
        if(!rd(f,n)||n>1000000u)throw Err("corruption","legacy index columns");for(uint32_t i=0;i<n;++i){std::string s;int c;if(!rs(f,s)||!rd(f,c))throw Err("corruption","legacy index column");index_cols[s]=c;}
      }else if(fsize(dir+"/STATE")){
        f.close();std::ifstream json(dir+"/STATE");std::string line;std::getline(json,line);J z=parse(line);if(fsize(dir+"/wal.log"))throw Err("corruption","legacy JSON STATE has nonempty WAL");boundary=vec_end;store->vector_end=boundary;
        for(auto&x:z.o["t"].a){Table q;q.n=x.o["name"].s;for(auto&c:x.o["c"].a)q.c.push_back({c.o["n"].s,c.o["t"].s,(int)c.o["d"].i});for(auto&y:x.o["r"].a){Row r;r.id=y.o["id"].i;for(auto&v:y.o["v"].a)r.v.push_back(v.s);r.voff.push_back(-1);for(auto&o:y.o["o"].a)r.voff.push_back((off_t)o.i);import_row(q,r);}put(catalog_key(q),catalog(q));tabs.emplace(q.n,std::move(q));}
        for(auto&i:z.o["i"].a)indexes.insert(i.s);for(auto&i:z.o["c"].o)index_cols[i.first]=(int)i.second.i;
      }
      // Preserve the information the legacy format actually stored: reserved
      // names and each table's selected vector column, without inventing owners.
      for(auto&name:indexes){std::string s;lsm::str(s,name);lsm::str(s,{});lsm::u32(s,0);put("X"+lsm::part(name),s);}
      for(auto&kv:index_cols){std::string s;lsm::str(s,kv.first);lsm::u32(s,kv.second);put("V"+lsm::part(kv.first),s);}
      std::string o;lsm::u64(o,ordinal);put("O",o);store->flush();vec_cursor=boundary;
      std::ifstream w(dir+"/wal.log",std::ios::binary);
      struct OldFrame{uint64_t tx=0;uint16_t kind=0;std::string payload;};
      auto frame=[&](OldFrame& a){uint32_t m,n,crc;uint16_t flags;if(!w.read((char*)&m,4))return false;if(!rd(w,n)||!rd(w,a.tx)||!rd(w,a.kind)||!rd(w,flags)||!rd(w,crc))return false;if(m!=WAL_MAGIC||n>(1u<<20))throw Err("corruption","legacy WAL header");a.payload.assign(n,0);if(!w.read(a.payload.data(),n))return false;if(crc32c(a.payload.data(),n)!=crc)throw Err("corruption","legacy WAL checksum");size_t pad=(8-(24+n)%8)%8;if(pad&&!w.ignore((std::streamsize)pad))return false;return true;};
      OldFrame a;std::streampos begin=0;uint64_t active=0;
      while(frame(a)){
        if(a.kind==1){active=a.tx;begin=w.tellg();}
        else if(a.kind==3&&active==a.tx){if(a.tx>legacy_tx){auto end=w.tellg();w.seekg(begin);begin_transaction_import();OldFrame b;
            while(frame(b)&&b.kind==2){std::vector<std::string>t;if(!decode_tokens(b.payload,t))throw Err("corruption","legacy WAL payload");statement=store->batch();dispatch(t);std::string order;lsm::u64(order,ordinal);statement->put("O",order);tx->absorb(*statement);statement.reset();}
            if(!w||b.kind!=3||b.tx!=active||vec_cursor>vec_end)throw Err("corruption","legacy WAL transaction");
            store->commit(*tx,(uint64_t)vec_cursor);tx.reset();in=false;boundary=vec_cursor;w.seekg(end);
          }active=0;}
      }
      store->vector_end=boundary;store->finish_migration();recovering=false;in=false;tx.reset();statement.reset();
    }
    void begin_transaction_import(){tx=store->batch();in=true;}
    J cold_select(Table&q,const std::vector<Cond>&cs,const std::vector<std::string>&proj,bool metric,int mc,const std::vector<float>&qv,size_t dp,const std::string&alias,bool ordered,bool distance_order,int oc,bool desc,uint64_t limit,bool profile){
      using Clock=std::chrono::steady_clock;auto start=profile?Clock::now():Clock::time_point{};store->read_cache->begin_trace(profile);auto read_start=*store->read_cache->stats;
      uint64_t maps_start=map_calls;uint64_t decoded_start=row_decodes;const Cond* equality=nullptr;bool eligible=metric&&distance_order&&limit;const char*read_mode=getenv("LITE_LSM_READ");if(read_mode&&std::string(read_mode)!="auto")eligible=false;std::vector<std::pair<const Cond*,long long>> primary;
      for(auto&c:cs){if(!c.col)primary.push_back({&c,integer(c.val)});else if(c.op=="="&&q.c[c.col].t!="VECTOR"&&(!equality||(equality->col==c.col&&scalar_key(q.c[c.col],equality->val)==scalar_key(q.c[c.col],c.val))))equality=&c;else eligible=false;}
      QueryView* view=eligible?query_view(q,mc):nullptr;
      auto matches_id=[&](long long id){for(auto&x:primary){auto&op=x.first->op;auto n=x.second;if((op=="="&&id!=n)||(op=="!="&&id==n)||(op=="<"&&id>=n)||(op=="<="&&id>n)||(op==">"&&id<=n)||(op==">="&&id<n))return false;}return true;};
      struct Hit {Row row;double d=0;size_t bytes=0;};
      auto better=[&](const Hit&a,const Hit&b){int c=0;
        if(!ordered)return a.row.ordinal<b.row.ordinal;
        if(distance_order)c=(a.d>b.d)-(a.d<b.d);
        else if(oc==0)c=(a.row.id>b.row.id)-(a.row.id<b.row.id);
        else if(q.c[oc].t=="BIGINT"){auto x=integer(unq(a.row.v[oc-1])),y=integer(unq(b.row.v[oc-1]));c=(x>y)-(x<y);}
        else{auto x=unq(a.row.v[oc-1]),y=unq(b.row.v[oc-1]);c=(x>y)-(x<y);}
        return c?(desc?c>0:c<0):a.row.id<b.row.id;
      };
      std::priority_queue<Hit,std::vector<Hit>,decltype(better)> top(better);uint64_t candidates=0,distances=0,updates=0;size_t held=0;
      auto consider=[&](const Row&r,off_t off){++candidates;Hit h{r,0,0};if(metric){h.d=l2(off,q.c[mc].d,qv);++distances;}
        if(top.size()>=limit&&!better(h,top.top()))return;
        if(top.size()>=limit){held-=top.top().bytes;top.pop();}
        h.bytes=view?256:row_payload(q,r).size()+256;held+=h.bytes;
        if(held>(64u<<20))throw Err("resource_limit","query result exceeds 64 MiB; use LIMIT");top.push(std::move(h));++updates;
      };
      if(view){auto visit=[&](const VectorRef&r){if(!matches_id(r.id))return;++candidates;double d=l2(r.off,q.c[mc].d,qv);++distances;
          if(top.size()>=limit){const auto&worst=top.top();if((desc?d<worst.d:d>worst.d)||(d==worst.d&&r.id>=worst.row.id))return;held-=worst.bytes;top.pop();}
          Hit h;h.row.id=r.id;h.d=d;h.bytes=256;held+=h.bytes;if(held>(64u<<20))throw Err("resource_limit","query result exceeds 64 MiB; use LIMIT");top.push(std::move(h));++updates;};
        if(!equality){for(auto&r:view->rows)visit(r);}else if(auto posts=query_postings(q,*view,equality->col,scalar_key(q.c[equality->col],equality->val))){for(auto i:*posts)visit(view->rows.at(i));}else{auto p=index_prefix(q,equality->col)+scalar_key(q.c[equality->col],equality->val);auto c=scan(p,lsm::upper(p));auto at=view->rows.begin();
          while(c->valid){long long id=(long long)(lsm::number_value(c->row.key,c->row.key.size()-8)^(1ull<<63));at=std::lower_bound(at,view->rows.end(),id,[](const VectorRef&r,long long key){return r.id<key;});if(at==view->rows.end()||at->id!=id)throw Err("corruption","query view dangling index");visit(*at);c->next();}}
      }else if(limit)each_row(q,cs,[&](const Row&r){consider(r,metric?r.voff[mc]:-1);});
      double scan_ms=profile?std::chrono::duration<double,std::milli>(Clock::now()-start).count():0;auto phase=profile?Clock::now():Clock::time_point{};
      std::vector<Hit> hits;hits.reserve(top.size());while(!top.empty()){hits.push_back(top.top());top.pop();}std::reverse(hits.begin(),hits.end());
      bool materialize=view&&std::any_of(proj.begin(),proj.end(),[&](const std::string&p){return ci(q,p)!=0;});if(materialize){size_t expanded=0;for(auto&h:hits){if(!row_get(q,h.row.id,h.row))throw Err("corruption","query view result missing");expanded+=row_payload(q,h.row).size()+256;if(expanded>(64u<<20))throw Err("resource_limit","query result exceeds 64 MiB; use LIMIT");}}
      J out=jo(),cols=ja(),rows=ja();for(auto&p:proj)cols.a.push_back(js(p));if(metric)cols.a.insert(cols.a.begin()+dp,js(alias));size_t output_bytes=0;
      for(auto&h:hits){J a=ja();for(auto&p:proj){int c=ci(q,p);if(!c)a.a.push_back(ji(h.row.id));else if(q.c[c].t=="BIGINT")a.a.push_back(ji(integer(unq(h.row.v[c-1]))));else if(q.c[c].t=="VECTOR"){J ar=ja();auto v=read_vec(h.row.voff[c],q.c[c].d);output_bytes+=v.size()*sizeof(J);if(output_bytes>(64u<<20))throw Err("resource_limit","query result exceeds 64 MiB; use LIMIT");for(float x:v)ar.a.push_back(jd(x));a.a.push_back(std::move(ar));}else{output_bytes+=h.row.v[c-1].size();a.a.push_back(js(unq(h.row.v[c-1])));}}if(metric)a.a.insert(a.a.begin()+dp,jd(std::sqrt(h.d)));rows.a.push_back(std::move(a));}
      out.o["columns"]=std::move(cols);out.o["rows"]=std::move(rows);J mt=jo();mt.o["planner_path"]=js("exact_scan");mt.o["storage_path"]=js("lsm");mt.o["exact_path"]=js(metric?"scan":"generic");if(ann_mode)mt.o["ann_fallback"]=jb(true);
      mt.o["lsm_access"]=js(view?"compact":"rows");if(profile){mt.o["query_view_bytes"]=ji(query_view_bytes());mt.o["full_row_decodes"]=ji(row_decodes-decoded_start);mt.o["vector_map_creations"]=ji(map_calls-maps_start);mt.o["mapped_virtual_bytes"]=ji(mapped_bytes());mt.o["compact_filter_index"]=jb(view&&equality&&view->post_col==equality->col);}
      mt.o["candidate_count"]=ji(candidates);mt.o["filtered_candidates"]=ji(candidates);mt.o["raw_candidates"]=ji(candidates);mt.o["exact_evaluations"]=ji(distances);mt.o["full_distance_evaluations"]=ji(distances);mt.o["distance_dimensions"]=ji(metric?distances*q.c[mc].d:0);mt.o["heap_updates"]=ji(updates);
      for(auto k:{"probed_lists","distance_pruned","blocks_considered","blocks_pruned","block_pruned_candidates"})mt.o[k]=ji(0);
      if(profile){J times=jo();for(auto k:{"prepare","matrix","block_build","bounds","block_sort","vector_distance","topk"})times.o[k]=jd(0);times.o["scan_total"]=jd(scan_ms);times.o["result"]=jd(std::chrono::duration<double,std::milli>(Clock::now()-phase).count());times.o["select_total"]=jd(std::chrono::duration<double,std::milli>(Clock::now()-start).count());mt.o["profile_ms"]=std::move(times);}if(profile){auto&r=*store->read_cache->stats;mt.o["sst_reader_initializations"]=ji(r.readers-read_start.readers);mt.o["sst_index_bytes"]=ji(r.index_bytes-read_start.index_bytes);mt.o["sst_block_reads"]=ji(r.blocks-read_start.blocks);mt.o["sst_block_bytes"]=ji(r.block_bytes-read_start.block_bytes);mt.o["sst_record_decodes"]=ji(r.decodes-read_start.decodes);mt.o["sst_block_hits"]=ji(r.hits-read_start.hits);mt.o["sst_seeks"]=ji(r.seeks-read_start.seeks);mt.o["read_cache_bytes"]=ji(store->read_cache->used);mt.o["read_live_bytes"]=ji(r.live);mt.o["read_peak_bytes"]=ji(r.peak);mt.o["read_open_files"]=ji(r.handles);mt.o["sst_cursor_initializations"]=ji(r.cursors-read_start.cursors);mt.o["sst_block_requests"]=ji(r.requests-read_start.requests);mt.o["sst_unique_blocks"]=ji(store->read_cache->touched.size());mt.o["sst_unique_blocks_exact"]=jb(!store->read_cache->trace_full);mt.o["read_trace_estimated_bytes"]=ji(store->read_cache->touched.size()*64);}out.o["metrics"]=std::move(mt);return out;
    }
    std::vector<float> query_vec(const std::vector<std::string>&t)const{
      for(auto&x:t)if(x.find("[")!=std::string::npos)return vec(x);
      throw Err("invalid_request","distance vector");
    }
    J select(const std::vector<std::string>&t){
      // Until IVF/PQ recall is validated, ANN uses the exact executor too.
      // This is a data-independent correctness fallback, not a cached answer.
      using Clock=std::chrono::steady_clock;
      bool profile=std::getenv("LITE_PROFILE")&&std::string(std::getenv("LITE_PROFILE"))=="1";
      auto stamp=[&](){return profile?Clock::now():Clock::time_point{};};
      auto elapsed=[&](Clock::time_point a){return profile?std::chrono::duration<double,std::milli>(Clock::now()-a).count():0.0;};
      auto query_start=stamp(),phase=query_start;
      double prepare_ms=0,matrix_ms=0,block_build_ms=0,bounds_ms=0,block_sort_ms=0,distance_ms=0,topk_ms=0,scan_ms=0;
      uint64_t filtered_candidates=0,blocks_considered=0,blocks_pruned=0,block_pruned_candidates=0,heap_updates=0;
      uint64_t posting_build_start=hot_post_builds;

      size_t from=0;
      for(;from<t.size();++from)if(up(t[from])=="FROM")break;
      if(from+1>=t.size())throw Err("invalid_request","from");
      auto&q=tabs.at(t[from+1]);
      size_t wi=t.size(),oi=t.size(),li=t.size();
      for(size_t i=from+2;i<t.size();++i){
        if(up(t[i])=="WHERE"&&wi==t.size())wi=i;
        if(up(t[i])=="ORDER"&&oi==t.size())oi=i;
        if(up(t[i])=="LIMIT"&&li==t.size())li=i;
      }
      size_t e=std::min(oi,li);
      if(li<t.size()&&(li+1>=t.size()||!inttok(t[li+1])||t[li+1][0]=='-'))throw Err("invalid_request","limit");
      if(oi<t.size()&&(oi+2>=t.size()||up(t[oi+1])!="BY"))throw Err("invalid_request","order by");
      std::vector<Cond>cs;
      if(wi<t.size())cs=conditions(q,t,wi+1,e);
      bool metric=false;
      std::string alias="distance";
      int mcol=-1;
      size_t distance_projection=0;
      std::vector<std::string>proj;
      for(size_t i=1;i<from;){
        if(t[i]==","){
          ++i;
          continue;
        }
        if(up(t[i])=="L2_DISTANCE"){
          if(metric)throw Err("unsupported_sql","multiple distance projections");
          distance_projection=proj.size();
          metric=true;
          if(i+2<from)mcol=ci(q,t[i+2]);
          ++i;
          while(i<from&&t[i]!=")")++i;
          if(i<from)++i;
          if(i<from&&up(t[i])=="AS"){
            if(i+1<from)alias=t[i+1];
            i+=2;
          }
        }
        else proj.push_back(t[i++]);
      }
      if(metric&&mcol<0){
        for(size_t i=0;i<q.c.size();++i)if(q.c[i].t=="VECTOR"){
          mcol=(int)i;
          break;
        }
      }

      if(proj.size()==1&&proj[0]=="*"){
        proj.clear();
        for(auto&c:q.c)proj.push_back(c.n);
      }
      for(const auto&p:proj)if(ci(q,p)<0)throw Err("unknown_column","projection");
      bool distance_order=oi<t.size()&&(up(t[oi+2])=="L2_DISTANCE"||t[oi+2]==alias)&&metric;
      int order_col=oi<t.size()&&!distance_order?ci(q,t[oi+2]):-1;
      if(oi<t.size()&&!distance_order&&order_col<0)throw Err("unknown_column","order by");
      std::vector<float>qv;
      if(metric){
        qv=query_vec(t);
        if(mcol<=0||mcol>=(int)q.c.size()||(int)qv.size()!=q.c[mcol].d)throw Err("dimension_mismatch","query");
      }
      if(!ensure_hot(q))return cold_select(q,cs,proj,metric,mcol,qv,distance_projection,alias,oi<t.size(),distance_order,order_col,oi+3<t.size()&&up(t[oi+3])=="DESC",li<t.size()?std::stoull(t[li+1]):UINT64_MAX,profile);
      if(metric&&q.fast.size()!=q.order.size())refresh_fast(q);
      struct H{
        long long id;
        double d;
        double ord;
      };
      // vptr maps on demand. Empty/index-only candidates must not remap vector data.
      std::vector<long long>owned_scan,empty_scan;
      const std::vector<long long>*scan=&q.order;
      size_t proven=cs.size();
      for(size_t j=0;j<cs.size();++j){const auto&c=cs[j];if(q.c[c.col].t=="VECTOR")continue;
        std::string k=q.n+"."+q.c[c.col].n;std::vector<long long>range;
        const std::vector<long long>*cand=nullptr;
        if(c.op=="="){
          auto it=sindex.find(k);
          if(it!=sindex.end()){auto jt=it->second.find(unq(c.val));cand=jt==it->second.end()?&empty_scan:&jt->second;}
        }
        else if(q.c[c.col].t=="BIGINT"&&c.op!="!="){
          auto it=nindex.find(k);
          if(it!=nindex.end()){
            bool used=true;long long y=std::stoll(c.val);auto a=it->second.begin(),b=it->second.end();
            if(c.op==">")a=it->second.upper_bound(y);else if(c.op==">=")a=it->second.lower_bound(y);else if(c.op=="<")b=it->second.lower_bound(y);else if(c.op=="<=")b=it->second.upper_bound(y);else used=false;
            if(used){for(;a!=b;++a)range.insert(range.end(),a->second.begin(),a->second.end());cand=&range;}
          }
        }
        if(cand&&cand->size()<scan->size()){
          if(cand==&range){owned_scan=std::move(range);scan=&owned_scan;}else scan=cand;
          proven=j;
        }
      }
      size_t ask=li<t.size()?std::stoull(t[li+1]):10;
      int probes_used=0;
      size_t raw_candidates=0;
      bool exact_topk=metric&&oi+2<t.size()&&li<t.size()&&
        (up(t[oi+2])=="L2_DISTANCE"||t[oi+2]==alias)&&
        !(oi+3<t.size()&&up(t[oi+3])=="DESC");
      const char* path_env=std::getenv("LITE_EXACT_PATH");
      std::string exact_path=path_env?path_env:"auto";
      if(exact_path!="auto"&&exact_path!="scan"&&exact_path!="block")throw Err("invalid_request","LITE_EXACT_PATH must be auto, scan or block");
      // Auto currently selects scan: shuffled high-dimensional blocks rarely prune.
      bool block_exact=exact_topk&&exact_path=="block";
      if(!path_env&&std::getenv("LITE_BLOCK_EXACT"))block_exact=exact_topk&&std::atoi(std::getenv("LITE_BLOCK_EXACT"))!=0;
      std::vector<size_t>owned_positions;const std::vector<size_t>*direct_positions=nullptr;
      if(exact_topk&&!block_exact&&ask&&cs.size()==1&&cs[0].op=="="&&q.c[cs[0].col].t!="VECTOR")direct_positions=hot_postings(q,cs[0]);
      const std::vector<size_t>&positions=direct_positions?*direct_positions:owned_positions;
      const bool dense=exact_topk&&!block_exact&&cs.empty();
      if(dense)filtered_candidates=q.fast.size();
      else if(direct_positions)filtered_candidates=positions.size();
      else if(exact_topk){
        owned_positions.reserve(scan->size());
        if(scan==&q.order){
          for(size_t p=0;p<q.fast.size();++p)if(q.fast[p]&&match(q,*q.fast[p],cs,proven))owned_positions.push_back(p);
        }else{
          const auto&pos=fast_pos.at(q.n);
          for(auto id:*scan){auto it=pos.find(id);if(it!=pos.end()&&q.fast[it->second]&&match(q,*q.fast[it->second],cs,proven))owned_positions.push_back(it->second);}
          // Sort integer positions once, without tree lookups in the comparator.
          if(!std::is_sorted(positions.begin(),positions.end()))std::sort(owned_positions.begin(),owned_positions.end());
        }
        filtered_candidates=positions.size();
      }else if(metric){
        owned_scan=*scan;scan=&owned_scan;
        std::sort(owned_scan.begin(),owned_scan.end(),[&](long long a,long long b){
          off_t x=q.r.at(a).voff[mcol],y=q.r.at(b).voff[mcol];if(x!=y)return x<y;return a<b;
        });
      }
      prepare_ms=elapsed(phase);phase=stamp();
      const bool linear=exact_topk&&!block_exact&&!positions.empty()&&positions.back()-positions.front()+1==positions.size()&&(!std::getenv("LITE_CONTIGUOUS_SCAN")||std::atoi(std::getenv("LITE_CONTIGUOUS_SCAN"))!=0);
      if(metric&&(exact_topk?(ask&&filtered_candidates>2048):(cs.empty()||scan->size()>2048)))ensure_matrix(q,mcol);
      matrix_ms=elapsed(phase);
      std::vector<H>h;
      uint64_t early_pruned=0,early_dims=0,distance_calls=0,batched_vectors=0,matrix_scan_vectors=0,screened_vectors=0,screen_rejected=0;
      const Matrix*matrix=nullptr;auto mci=matrices.find(q.n);if(mci!=matrices.end()&&mci->second.col==mcol)matrix=&mci->second;
      const float*matrix_base=nullptr;if(matrix){if(matrix->mapped_off>=0){if(!block_exact)matrix_base=vptr(matrix->mapped_off,(int)matrix->values);}else matrix_base=matrix->v.data();if(!matrix_base)matrix=nullptr;}
      auto worse=[](const H&a,const H&b){
        if(a.d!=b.d)return a.d<b.d;
        return a.id<b.id;
      };
      std::priority_queue<H,std::vector<H>,decltype(worse)>top(worse);
      auto add_hit=[&](long long id,const Row*fast_row=nullptr){
        const Row*row=fast_row;
        if(!row){
          auto it=q.r.find(id);
          if(it==q.r.end())return;
          row=&it->second;
        }
        if(!match(q,*row,cs,proven))return;
        double d=0;
        if(metric){
          if(row->voff.size()>=(size_t)q.c.size()&&row->voff[mcol]>=0){
            ++distance_calls;
            int used=q.c[mcol].d;bool cut=false;const float*mp=matrix_ptr(q,mcol,row->id);
            if(mp)d=l2p(mp,q.c[mcol].d,qv);
            else d=l2(row->voff[mcol],q.c[mcol].d,qv);
            early_dims+=used;if(cut){++early_pruned;return;}
          }
          else{
            auto v=vec(row->v[mcol-1],q.c[mcol].d);
            for(size_t k=0;k<v.size();++k){
              double z=v[k]-qv[k];
              d+=z*z;
            }
            ++distance_calls;early_dims+=v.size();
          }
        }
        double ov=0;
        if(oi<t.size()){
          std::string on=oi+2<t.size()?t[oi+2]:"";
          if(up(on)=="L2_DISTANCE"||on==alias)ov=d;
          else{
            int c=ci(q,on);
            if(c==0)ov=id;
            else if(c>0&&q.c[c].t=="BIGINT")ov=std::stod(unq(row->v[c-1]));
          }
        }
        H hh{id,d,ov};
        if(block_exact){
          if(!ask)return;
          if(top.size()<ask)top.push(hh);
          else if(worse(hh,top.top())){top.pop();top.push(hh);}
        }
        else h.push_back(hh);
      };
      if(exact_topk){
        const bool small_filtered=direct_positions&&filtered_candidates&&filtered_candidates<=1024;const char*batch_setting=std::getenv("LITE_L2_BATCH");int width=!batch_setting||std::string(batch_setting)=="auto"?(dense||linear?1:(small_filtered?4:2)):envint("LITE_L2_BATCH",1);
        if(width!=1&&width!=2&&width!=4)throw Err("invalid_request","LITE_L2_BATCH must be auto, 1, 2 or 4");
        bool early=std::getenv("LITE_EARLY")&&std::atoi(std::getenv("LITE_EARLY"))!=0;
        bool late_id=!std::getenv("LITE_LATE_ID")||std::atoi(std::getenv("LITE_LATE_ID"))!=0;
        int dim=q.c[mcol].d;if(early||!(dim>=32&&has_avx2()))width=1;
        bool matrix_scan=(dense||linear)&&matrix&&late_id&&!early&&width==1&&dim>=32&&has_avx2()&&(!std::getenv("LITE_MATRIX_SCAN")||std::atoi(std::getenv("LITE_MATRIX_SCAN"))!=0);
        auto scan_kernel=dim>=64&&has_avx512()?l2_avx512_scan:l2_avx2_scan;
        if(dim==128&&has_avx512()&&(!std::getenv("LITE_QUERY_WIDE")||std::atoi(std::getenv("LITE_QUERY_WIDE"))))scan_kernel=l2_avx512_query128;
        bool screen=!small_filtered&&matrix&&!matrix->codes.empty()&&!block_exact&&!early&&late_id&&(_mm_getcsr()&0xe040u)==0&&(!std::getenv("LITE_DISTANCE_SCREEN")||std::atoi(std::getenv("LITE_DISTANCE_SCREEN"))!=0);
        int8_t screen_query[128];double screen_error=screen?screen_query128(qv.data(),matrix->scale,screen_query):0;
        // Interleave independent vectors, retaining each ISA's original double reduction order.
        // Diagnostic clocks run once per batch, never once per vector.
        auto score=[&](size_t begin,size_t end){
          for(size_t base=begin;base<end;){
            H batch[256];size_t count=0;auto dt=stamp();
            size_t stop=std::min(end,base+(screen&&base==begin?16:256));double limit=top.size()>=ask?top.top().d:std::numeric_limits<double>::infinity();
            if(screen){
              int ds[256];if(dense&&!matrix->packed.empty())l2_screen_dot128(matrix->packed.data()+base*128,matrix->norms.data()+base,screen_query,stop-base,ds);
              else l2_screen128(matrix->codes.data()+(dense?base*128:0),screen_query,dense?nullptr:positions.data()+base,stop-base,ds);double threshold=screen_threshold128(limit,matrix->scale,screen_error);
              screened_vectors+=stop-base;early_dims+=(stop-base)*128;
              for(size_t i=base;i<stop;++i){if(ds[i-base]>threshold){++screen_rejected;continue;}size_t p=dense?i:positions[i];if(!q.fast[p])continue;
                double d=l2p(matrix_base+p*128,128,qv);++distance_calls;early_dims+=128;batch[count++]={(long long)p,d,d};}
            }else if(matrix_scan){
              double ds[256];scan_kernel(matrix_base+(dense?base:positions[base])*(size_t)dim,qv.data(),dim,stop-base,ds);
              for(size_t i=base;i<stop;++i){size_t p=dense?i:positions[i];if(q.fast[p])batch[count++]={(long long)p,ds[i-base],ds[i-base]};}
              matrix_scan_vectors+=stop-base;distance_calls+=stop-base;early_dims+=(stop-base)*(size_t)dim;
            }else for(size_t i=base;i<stop;){
              size_t p=dense?i:positions[i];if(!q.fast[p]){++i;continue;}const Row&r=*q.fast[p];
              const float*mp=matrix?matrix_base+p*(size_t)matrix->dim:nullptr;
              if(width>1&&i+(size_t)width<=stop){
                const Row*rows[4];const float*ptrs[4];double ds[4];bool ready=true;
                if(!matrix)vptr(r.voff[mcol],dim);
                for(int k=0;k<width;++k){
                  size_t pos=dense?i+k:positions[i+k];rows[k]=q.fast[pos];if(!rows[k]){ready=false;break;}
                  if(matrix)ptrs[k]=matrix_base+pos*(size_t)dim;
                  else{off_t off=rows[k]->voff[mcol];
                  if(off>=vecbuf_base&&size_t(off-vecbuf_base+dim*4)<=vecbuf.size())ptrs[k]=(const float*)(vecbuf.data()+off-vecbuf_base);
                  else if(vmap&&off>=mapoff&&size_t(off-mapoff+dim*4)<=mapsz)ptrs[k]=(const float*)((const char*)vmap+off-mapoff);
                  else{ready=false;break;}}
                }
                if(ready){
                  if(dim>=64&&has_avx512()){if(width==4)l2_avx512_many<4>(ptrs,qv.data(),dim,ds);else l2_avx512_many<2>(ptrs,qv.data(),dim,ds);}
                  else{if(width==4)l2_avx2_many<4>(ptrs,qv.data(),dim,ds);else l2_avx2_many<2>(ptrs,qv.data(),dim,ds);}
                  for(int k=0;k<width;++k)batch[count++]={late_id?(long long)(dense?i+k:positions[i+k]):rows[k]->id,ds[k],ds[k]};
                  batched_vectors+=width;distance_calls+=width;early_dims+=(uint64_t)width*dim;i+=width;continue;
                }
              }
              int used=dim;double d=early?l2_limit(mp,r.voff[mcol],dim,qv,limit,used):mp?l2p(mp,dim,qv):l2(r.voff[mcol],dim,qv);
              ++distance_calls;early_dims+=used;++i;if(used<dim){++early_pruned;continue;}
              batch[count++]={late_id?(long long)p:r.id,d,d};
            }
            distance_ms+=elapsed(dt);dt=stamp();
            for(size_t i=0;i<count;++i){
              if(late_id){if(top.size()>=ask&&batch[i].d>top.top().d)continue;batch[i].id=q.fast[(size_t)batch[i].id]->id;}
              if(top.size()<ask){top.push(batch[i]);++heap_updates;}
              else if(worse(batch[i],top.top())){top.pop();top.push(batch[i]);++heap_updates;}
            }
            topk_ms+=elapsed(dt);
            base=stop;
          }
        };
        phase=stamp();
        if(block_exact&&ask&&!positions.empty()){
          build_vblocks(q,mcol);
          block_build_ms=elapsed(phase);phase=stamp();
          const auto&bs=vblocks.at(q.n);
          struct Visit{double bound;size_t begin,end;};
          std::vector<Visit>visits;visits.reserve(bs.size());
          for(size_t begin=0;begin<positions.size();){
            size_t bi=positions[begin]/64,end=begin+1;
            while(end<positions.size()&&positions[end]/64==bi)++end;
            double bound=0;
            for(int j=0;j<q.c[mcol].d;++j){
              double x=qv[j],delta=x<bs[bi].lo[j]?double(bs[bi].lo[j])-x:x>bs[bi].hi[j]?x-double(bs[bi].hi[j]):0;
              bound+=delta*delta;
            }
            // Only the box bound is used. The persisted float sphere radius is
            // not outward-rounded. Discount for float SIMD rounding as well.
            double margin=std::min(1.0,16.0*(q.c[mcol].d+1)*std::numeric_limits<float>::epsilon());
            bound=std::max(0.0,bound*(1-margin)-std::numeric_limits<float>::min()*q.c[mcol].d);
            visits.push_back({bound,begin,end});begin=end;++blocks_considered;
          }
          bounds_ms=elapsed(phase);phase=stamp();
          bool bound_order=std::getenv("LITE_BLOCK_ORDER")&&std::string(std::getenv("LITE_BLOCK_ORDER"))=="bound";
          if(bound_order)std::stable_sort(visits.begin(),visits.end(),[](const Visit&a,const Visit&b){return a.bound<b.bound;});
          block_sort_ms=elapsed(phase);phase=stamp();
          for(const auto&v:visits){
            if(top.size()>=ask&&v.bound>top.top().d){++blocks_pruned;block_pruned_candidates+=v.end-v.begin;continue;}
            score(v.begin,v.end);
          }
        }else if(ask)score(0,dense?q.fast.size():positions.size());
        scan_ms=elapsed(phase);
        while(!top.empty()){h.push_back(top.top());top.pop();}
      }else{
        phase=stamp();for(auto id:*scan)add_hit(id);scan_ms=elapsed(phase);
        filtered_candidates=h.size();
      }
      phase=stamp();
      if(metric&&li<t.size()&&oi+2<t.size()&&ask<h.size()&&(up(t[oi+2])=="L2_DISTANCE"||t[oi+2]==alias)&&!(oi+3<t.size()&&up(t[oi+3])=="DESC")){
        auto cmp=[&](const H&a,const H&b){
          if(a.d!=b.d)return a.d<b.d;
          return a.id<b.id;
        };
        std::nth_element(h.begin(),h.begin()+ask,h.end(),cmp);
        h.resize(ask);
      }
      if(oi<t.size()){
        bool desc=oi+3<t.size()&&up(t[oi+3])=="DESC";
        std::stable_sort(h.begin(),h.end(),[&](const H&a,const H&b){
          int cmp=0;
          if(distance_order)cmp=(a.d>b.d)-(a.d<b.d);
          else if(order_col==0)cmp=(a.id>b.id)-(a.id<b.id);
          else{
            const auto&av=q.r.at(a.id).v[order_col-1];const auto&bv=q.r.at(b.id).v[order_col-1];
            if(q.c[order_col].t=="BIGINT"){auto x=std::stoll(unq(av)),y=std::stoll(unq(bv));cmp=(x>y)-(x<y);}
            else{auto x=unq(av),y=unq(bv);cmp=(x>y)-(x<y);}
          }
          if(cmp)return desc?cmp>0:cmp<0;return a.id<b.id;
        });
      }
      size_t lim=h.size();
      if(li<t.size())lim=std::stoull(t[li+1]);
      J out=jo(),cols=ja(),rows=ja();
      rows.a.reserve(std::min(lim,h.size()));
      for(auto&p:proj)cols.a.push_back(js(p));
      if(metric)cols.a.insert(cols.a.begin()+distance_projection,js(alias));
      for(size_t n=0;n<std::min(lim,h.size());++n){
        auto&hh=h[n];
        auto&row=q.r[hh.id];
        J a=ja();
        a.a.reserve(proj.size()+size_t(metric));
        for(auto&p:proj){
          int c=ci(q,p);
          if(c==0)a.a.push_back(ji(hh.id));
          else if(c>0){
            auto&col=q.c[c];
            if(col.t=="BIGINT")a.a.push_back(ji(std::stoll(unq(row.v[c-1]))));
            else if(col.t=="VECTOR"){
              J ar=ja();
              for(float z:(row.voff.size()>=(size_t)q.c.size()&&row.voff[c]>=0?read_vec(row.voff[c],col.d):vec(row.v[c-1],col.d)))ar.a.push_back(jd(z));
              a.a.push_back(ar);
            }
            else a.a.push_back(js(unq(row.v[c-1])));
          }
        }
        if(metric)a.a.insert(a.a.begin()+distance_projection,jd(std::sqrt(hh.d)));
        rows.a.push_back(std::move(a));
      }
      out.o["columns"]=std::move(cols);
      out.o["rows"]=std::move(rows);
      J mt=jo();
      mt.o["planner_path"]=js("exact_scan");
      if(ann_mode)mt.o["ann_fallback"]=jb(true);
      mt.o["candidate_count"]=ji((long long)scan->size());
      mt.o["raw_candidates"]=ji((long long)raw_candidates);
      mt.o["probed_lists"]=ji(probes_used);
      mt.o["exact_evaluations"]=ji((long long)distance_calls);
      mt.o["distance_pruned"]=ji((long long)early_pruned);
      mt.o["distance_dimensions"]=ji((long long)early_dims);
      mt.o["exact_path"]=js(exact_topk?(block_exact?"block":"scan"):"generic");
      mt.o["filtered_candidates"]=ji(filtered_candidates);
      mt.o["full_distance_evaluations"]=ji(distance_calls-early_pruned);
      mt.o["blocks_considered"]=ji(blocks_considered);
      mt.o["blocks_pruned"]=ji(blocks_pruned);
      mt.o["block_pruned_candidates"]=ji(block_pruned_candidates);
      mt.o["heap_updates"]=ji(heap_updates);
      if(profile){
        mt.o["matrix_owned_bytes"]=ji(matrix?matrix->v.capacity()*sizeof(float):0);mt.o["matrix_mapped"]=jb(matrix&&matrix->mapped_off>=0);
        mt.o["screened_vectors"]=ji(screened_vectors);mt.o["screen_rejected"]=ji(screen_rejected);mt.o["screen_bytes"]=ji(matrix?matrix->codes.capacity()+matrix->packed.capacity()+matrix->norms.capacity()*4:0);
        mt.o["hot_postings"]=jb(direct_positions!=nullptr);mt.o["hot_postings_builds"]=ji(hot_post_builds-posting_build_start);mt.o["hot_postings_bytes"]=ji(hot_posts.bytes);
        mt.o["batched_vectors"]=ji(batched_vectors);mt.o["matrix_enabled"]=jb(matrix!=nullptr);
        mt.o["matrix_scan_vectors"]=ji(matrix_scan_vectors);
        mt.o["contiguous_candidates"]=jb(linear);
        mt.o["distance_isa"]=js(metric&&q.c[mcol].d>=64&&has_avx512()?"avx512":metric&&q.c[mcol].d>=32&&has_avx2()?"avx2":"scalar");
        J times=jo();
        times.o["prepare"]=jd(prepare_ms);times.o["matrix"]=jd(matrix_ms);
        times.o["block_build"]=jd(block_build_ms);times.o["bounds"]=jd(bounds_ms);times.o["block_sort"]=jd(block_sort_ms);
        times.o["vector_distance"]=jd(distance_ms);times.o["topk"]=jd(topk_ms);times.o["scan_total"]=jd(scan_ms);
        times.o["result"]=jd(elapsed(phase));times.o["select_total"]=jd(elapsed(query_start));mt.o["profile_ms"]=std::move(times);
      }
      out.o["metrics"]=std::move(mt);
      return out;
    }
  };
}
int main(int argc,char**argv){
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);
  std::string d="/data",maint;
  bool check=false;
  for(int i=1;i<argc;++i){
    std::string a=argv[i];
    if(a=="--data-dir"&&i+1<argc)d=argv[++i];
    else if(a=="--check")check=true;
    else if(a=="--maintenance"&&i+1<argc)maint=argv[++i];
    else{
      std::cerr<<"unknown argument\n";
      return 2;
    }
  }
  try{
    lite::Engine e(d);
    if(check){
      e.store->verify();
      std::cout<<"{\"ok\":true,\"tables\":"<<e.tabs.size()<<"}\n";
      return 0;
    }
    if(!maint.empty()){
      if(maint=="flush")e.flush_sst();
      else if(maint=="compact")e.compact();
      else if(maint=="gc")e.gc();
      else throw lite::Err("invalid_request","maintenance");
      e.checkpoint();
      return 0;
    }
    std::cout<<"{\"type\":\"ready\"}\n"<<std::flush;
    std::string line;
    while(std::getline(std::cin,line)){
      if(line.find_first_not_of(" \r\t")==std::string::npos)continue;
      lite::J req;
      try{
        req=lite::parse(line);
        bool shut=(req.o.count("type")&&req.o["type"].k==lite::J::S&&req.o["type"].s=="shutdown")||(req.o.count("op")&&req.o["op"].k==lite::J::S&&req.o["op"].s=="shutdown");
        if(shut){
          e.rollback();
          e.checkpoint();
          lite::J o=lite::jo();
          o.o["ok"]=lite::jb(true);
          if(req.o.count("id"))o.o["id"]=req.o["id"];
          std::cout<<lite::dump(o)<<"\n"<<std::flush;
          break;
        }
        if(!req.o.count("sql")||req.o["sql"].k!=lite::J::S)throw lite::Err("invalid_request","sql");
        e.ann_mode=req.o.count("search_mode")&&req.o["search_mode"].k==lite::J::S&&req.o["search_mode"].s=="ann";
        lite::J out=e.exec(req.o["sql"].s);
        out.o["ok"]=lite::jb(true);
        if(req.o.count("id"))out.o["id"]=req.o["id"];
        std::cout<<lite::dump(out)<<"\n"<<std::flush;
      }
      catch(const lite::Err&x){
        lite::J o=lite::jo();
        o.o["ok"]=lite::jb(false);
        lite::J er=lite::jo();
        er.o["code"]=lite::js(x.code);
        er.o["message"]=lite::js(x.what());
        o.o["error"]=er;
        if(req.k==lite::J::O&&req.o.count("id"))o.o["id"]=req.o["id"];
        std::cout<<lite::dump(o)<<"\n"<<std::flush;
      }
      catch(...){
        lite::J o=lite::jo(),er=lite::jo();o.o["ok"]=lite::jb(false);
        er.o["code"]=lite::js("invalid_request");er.o["message"]=lite::js("invalid statement or value");o.o["error"]=er;
        if(req.k==lite::J::O&&req.o.count("id"))o.o["id"]=req.o["id"];
        try{throw;}catch(const std::exception&x){std::cerr<<"request failed: "<<x.what()<<'\n';}catch(...){std::cerr<<"request failed: unknown exception\n";}
        std::cout<<lite::dump(o)<<"\n"<<std::flush;
      }
    }
  }
  catch(const std::exception&x){
    std::cerr<<x.what()<<"\n";
    return 1;
  }
  return 0;
}
