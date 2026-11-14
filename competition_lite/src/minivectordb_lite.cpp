#include <algorithm>
#include <cmath>
#include <immintrin.h>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
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
  static J jb(bool x){
    J j;
    j.k=J::B;
    j.b=x;
    return j;
  }
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
  static void ws(const std::string&s,size_t&p){
    while(p<s.size()&&std::isspace((unsigned char)s[p]))++p;
  }
  static std::string jstr(const std::string&s,size_t&p){
    if(p>=s.size()||s[p++]!='"')throw Err("invalid_request","string");
    std::string r;
    while(p<s.size()){
      char c=s[p++];
      if(c=='"')return r;
      if(c=='\\'&&p<s.size()){
        char e=s[p++];
        r+=e=='n'?'\n':e=='r'?'\r':e=='t'?'\t':e;
      }
      else r+=c;
    }
    throw Err("invalid_request","string");
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
      if(c=='"'||c=='\\')r+='\\';
      if(c=='\n')r+="\\n";
      else if(c=='\r')r+="\\r";
      else if(c=='\t')r+="\\t";
      else r+=c;
    }
    return r+"\"";
  }
  static std::string dump(const J&x){
    if(x.k==J::N)return"null";
    if(x.k==J::B)return x.b?"true":"false";
    if(x.k==J::I)return std::to_string(x.i);
    if(x.k==J::D){
      std::ostringstream o;
      o<<std::setprecision(17)<<x.d;
      return o.str();
    }
    if(x.k==J::S)return esc(x.s);
    if(x.k==J::A){
      std::string r="[";
      for(size_t i=0;i<x.a.size();++i)r+=(i?",":"")+dump(x.a[i]);
      return r+"]";
    }
    std::string r="{";
    bool f=true;
    for(auto&v:x.o){
      if(!f)r+=',';
      f=false;
      r+=esc(v.first)+":"+dump(v.second);
    }
    return r+"}";
  }
  static std::vector<std::string> tok(const std::string&s){
    std::vector<std::string>r;
    for(size_t p=0;p<s.size();){
      while(p<s.size()&&std::isspace((unsigned char)s[p]))++p;
      if(p>=s.size())break;
      if(s[p]=='\''){
        ++p;
        std::string x;
        while(p<s.size()){
          char c=s[p++];
          if(c=='\'')break;
          if(c=='\\'&&p<s.size())x+=s[p++];
          else x+=c;
        }
        r.push_back("'"+x+"'");
      }
      else if(std::string("(),;=<>!").find(s[p])!=std::string::npos){
        if((s[p]=='<'||s[p]=='>'||s[p]=='!')&&p+1<s.size()&&s[p+1]=='='){
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
  struct Col{
    std::string n,t;
    int d=0;
  };
  struct Row{
    long long id=0;
    std::vector<std::string>v;
    std::vector<off_t>voff;
  };
  struct Table{
    std::string n;
    std::vector<Col>c;
    std::unordered_map<long long,Row>r;
    std::vector<long long>order;
  };
  struct Cond{
    int col=-1;
    std::string op,val;
  };
  struct PQ{
    int d=0,m=0,ks=0,nlist=0;
    std::vector<float>coarse,book;
    std::map<long long,std::vector<unsigned char>>code;
    std::map<long long,int>cell;
  };
  struct VBlock{
    size_t begin=0,end=0;
    std::vector<float>lo,hi;
  };
  static uint32_t crc32c(const char*p,size_t n){
    uint32_t c=0xffffffffu;
    for(size_t i=0;i<n;++i){
      c^=(unsigned char)p[i];
      for(int k=0;k<8;++k)c=(c>>1)^((c&1)?0x82f63b78u:0u);
    }
    return ~c;
  }
  static const uint32_t WAL_MAGIC=0x4d564c32u;
  #if defined(__x86_64__) || defined(__i386__)
  #if defined(__GNUC__) || defined(__clang__)
  __attribute__((target("avx2"))) static double l2_avx2(const float*a,const float*b,int d){
    __m256 sum=_mm256_setzero_ps();
    int i=0;
    for(;i+8<=d;i+=8){
      __m256 va=_mm256_loadu_ps(a+i),vb=_mm256_loadu_ps(b+i),x=_mm256_sub_ps(va,vb);
      sum=_mm256_add_ps(sum,_mm256_mul_ps(x,x));
    }
    alignas(32) float z[8];
    _mm256_store_ps(z,sum);
    double s=0;
    for(float x:z)s+=x;
    for(;i<d;++i){
      double x=a[i]-b[i];
      s+=x*x;
    }
    return s;
  }
  static bool has_avx2(){
    static const bool v=__builtin_cpu_supports("avx2");
    return v;
  }
  #else
  static bool has_avx2(){
    return false;
  }
  #endif
  #else
  static bool has_avx2(){
    return false;
  }
  #endif
  struct Engine{
    std::string dir,wal,vecfile;
    std::map<std::string,Table>tabs,save;
    std::set<std::string>indexes;
    std::vector<std::string>pending,memtable;
    bool in=false,recovering=false;
    off_t vec_mark=0,vec_end=0,vec_cursor=0;
    size_t vecbuf_mark=0;
    int vfd=-1;
    std::string vecbuf;
    off_t vecbuf_base=0;
    std::vector<float>scratch;
    std::unordered_map<std::string,std::string>vcache;
    std::vector<std::string>immutable;
    std::map<int,std::vector<std::string>>levels;
    unsigned sst_seq=0;
    uint64_t wal_txid=0;
    bool ann_mode=false;
    std::map<std::string,int>index_cols;
    std::map<std::string,std::map<int,std::vector<long long>>>buckets;
    std::map<std::string,PQ>pqs;
    std::map<std::string,std::vector<VBlock>>vblocks;
    std::map<std::string,std::map<std::string,std::vector<long long>>>sindex;
    std::map<std::string,J>qcache;
    mutable void*vmap=nullptr;
    mutable size_t mapsz=0;
    explicit Engine(std::string d):dir(std::move(d)),wal(dir+"/wal.log"),vecfile(dir+"/vectors.f32"){
      for(size_t p=1;p<dir.size();++p)if(dir[p]==char(47))mkdir(dir.substr(0,p).c_str(),0755);
      mkdir(dir.c_str(),0755);
      vfd=open(vecfile.c_str(),O_RDWR|O_CREAT|O_APPEND,0644);
      vec_end=fsize(vecfile);
      load_manifest();
      recover();
    }
    void load_manifest(){
      std::ifstream f(dir+"/MANIFEST");
      std::string x;
      while(std::getline(f,x)){
        if(x.empty())continue;
        int lev=0;
        std::string p=x;
        size_t sp=x.find(' ');
        if(sp!=std::string::npos){
          try{
            lev=std::stoi(x.substr(0,sp));
            p=x.substr(sp+1);
          }
          catch(...){
            lev=0;
            p=x;
          }
        }
        if(p.find('/')==std::string::npos)p=dir+"/"+p;
        levels[lev].push_back(p);
      }
      DIR*d=opendir(dir.c_str());
      if(!d)return;
      while(auto*e=readdir(d)){
        std::string n=e->d_name;
        if(n.size()<6||n.find(".sst")==std::string::npos)continue;
        size_t p=n.find_last_of('-');
        if(p==std::string::npos)continue;
        unsigned v=0;
        bool ok=false;
        for(size_t i=p+1;i<n.size()&&std::isdigit((unsigned char)n[i]);++i){
          v=v*10+(unsigned)(n[i]-'0');
          ok=true;
        }
        if(ok&&v>sst_seq)sst_seq=v;
      }
      closedir(d);
    }
    static off_t fsize(const std::string&p){
      struct stat st{
      };
      return stat(p.c_str(),&st)==0?st.st_size:0;
    }
    void trunc_vec(off_t n){
      vec_end=n;
      if(vfd>=0)ftruncate(vfd,n);
      else{
        int fd=open(vecfile.c_str(),O_WRONLY|O_CREAT,0644);
        if(fd>=0){
          ftruncate(fd,n);
          close(fd);
        }
      }
    }
    ~Engine(){
      flush_sst();
      if(vmap&&mapsz)munmap(vmap,mapsz);
      if(vfd>=0){
        fsync(vfd);
        close(vfd);
      }
    }
    void snapshot(){
      J a=ja();
      for(auto&t:tabs){
        J x=jo();
        x.o["name"]=js(t.second.n);
        J c=ja();
        for(auto&z:t.second.c){
          J y=jo();
          y.o["n"]=js(z.n);
          y.o["t"]=js(z.t);
          y.o["d"]=ji(z.d);
          c.a.push_back(y);
        }
        x.o["c"]=c;
        J r=ja();
        for(auto&kv:t.second.r){
          J y=jo();
          y.o["id"]=ji(kv.second.id);
          J v=ja(),o=ja();
          for(size_t i=1;i<t.second.c.size();++i){
            v.a.push_back(js(kv.second.v[i-1]));
            o.a.push_back(ji(i<kv.second.voff.size()?kv.second.voff[i]:-1));
          }
          y.o["v"]=v;
          y.o["o"]=o;
          r.a.push_back(y);
        }
        x.o["r"]=r;
        a.a.push_back(x);
      }
      J z=jo(),ix=ja(),ic=jo();
      for(auto&s:indexes)ix.a.push_back(js(s));
      for(auto&v:index_cols)ic.o[v.first]=ji(v.second);
      z.o["t"]=a;
      z.o["i"]=ix;
      z.o["c"]=ic;
      z.o["w"]=ji((long long)wal_txid);
      std::string p=dir+"/STATE.tmp";
      std::ofstream f(p,std::ios::trunc);
      f<<dump(z)<<"\n";
      f.flush();
      int fd=open(p.c_str(),O_RDONLY);
      if(fd>=0){
        fsync(fd);
        close(fd);
      }
      rename(p.c_str(),(dir+"/STATE").c_str());
    }
    template<class T> static void wr(std::ofstream&f,const T&v){
      f.write((const char*)&v,sizeof(v));
    }
    template<class T> static bool rd(std::ifstream&f,T&v){
      return bool(f.read((char*)&v,sizeof(v)));
    }
    std::string index_path(const Table&q)const{
      return dir+"/INDEX-"+q.n+".bin";
    }
    void save_index(const Table&q){
      auto z=pqs.find(q.n);
      if(z==pqs.end())return;
      std::ofstream f(index_path(q)+".tmp",std::ios::binary|std::ios::trunc);
      uint32_t magic=0x4d564950,ver=1,d=z->second.d,m=z->second.m,ks=z->second.ks,nl=z->second.nlist,n=(uint32_t)z->second.code.size();
      wr(f,magic);
      wr(f,ver);
      wr(f,wal_txid);
      wr(f,d);
      wr(f,m);
      wr(f,ks);
      wr(f,nl);
      wr(f,n);
      for(float v:z->second.coarse)wr(f,v);
      for(float v:z->second.book)wr(f,v);
      for(auto&kv:z->second.code){
        uint64_t id=kv.first;
        uint32_t len=(uint32_t)kv.second.size();
        uint32_t cell=z->second.cell.count((long long)id)?(uint32_t)z->second.cell.at((long long)id):0;
        wr(f,id);
        wr(f,cell);
        wr(f,len);
        f.write((char*)kv.second.data(),len);
      }
      f.flush();
      int fd=open((index_path(q)+".tmp").c_str(),O_RDONLY);
      if(fd>=0){
        fsync(fd);
        close(fd);
      }
      rename((index_path(q)+".tmp").c_str(),index_path(q).c_str());
    }
    bool load_index(Table&q){
      auto it=index_cols.find(q.n);
      if(it==index_cols.end())return false;
      std::ifstream f(index_path(q),std::ios::binary);
      if(!f)return false;
      uint32_t magic=0,ver=0,d=0,m=0,ks=0,nl=0,n=0;
      uint64_t seq=0;
      if(!rd(f,magic)||!rd(f,ver)||!rd(f,seq)||!rd(f,d)||!rd(f,m)||!rd(f,ks)||!rd(f,nl)||!rd(f,n)||magic!=0x4d564950||ver!=1||seq!=wal_txid||n>q.r.size())return false;
      PQ z;
      z.d=(int)d;
      z.m=(int)m;
      z.ks=(int)ks;
      z.nlist=(int)nl;
      if(z.d!=q.c[it->second].d||!z.d||!z.m||!z.ks||!z.nlist)return false;
      z.coarse.resize((size_t)z.nlist*z.d);
      z.book.resize((size_t)z.m*z.ks*((z.d+z.m-1)/z.m));
      for(float&v:z.coarse)if(!rd(f,v))return false;
      for(float&v:z.book)if(!rd(f,v))return false;
      buckets[q.n].clear();
      for(uint32_t i=0;i<n;++i){
        uint64_t id=0;
        uint32_t cell=0,len=0;
        if(!rd(f,id)||!rd(f,cell)||!rd(f,len)||cell>=(uint32_t)z.nlist||len!=(uint32_t)z.m)return false;
        std::vector<unsigned char>code(len);
        if(!f.read((char*)code.data(),len)||!q.r.count((long long)id))return false;
        auto&row=q.r[(long long)id];
        if(it->second>=(int)row.voff.size()||row.voff[it->second]<0)return false;
        buckets[q.n][cell].push_back((long long)id);
        z.cell[(long long)id]=(int)cell;
        z.code[(long long)id]=std::move(code);
      }
      pqs[q.n]=std::move(z);
      return true;
    }
    void gc(){
      flush_vecbuf();
      std::string tmp=vecfile+".gc";
      int out=open(tmp.c_str(),O_WRONLY|O_CREAT|O_TRUNC,0644);
      if(out<0)throw Err("io_error","gc output");
      off_t n=0;
      for(auto&tk:tabs)for(auto&rk:tk.second.r)for(size_t c=1;c<tk.second.c.size();++c){
        if(tk.second.c[c].t!="VECTOR"||c>=rk.second.voff.size()||rk.second.voff[c]<0)continue;
        auto v=read_vec(rk.second.voff[c],tk.second.c[c].d);
        size_t bytes=v.size()*sizeof(float);
        if(write(out,v.data(),bytes)!=(ssize_t)bytes){
          close(out);
          throw Err("io_error","gc write");
        }
        rk.second.voff[c]=n;
        n+=(off_t)bytes;
      }
      fsync(out);
      close(out);
      if(vmap&&mapsz){
        munmap(vmap,mapsz);
        vmap=nullptr;
        mapsz=0;
      }
      if(vfd>=0)close(vfd);
      if(rename(tmp.c_str(),vecfile.c_str())!=0)throw Err("io_error","gc publish");
      vfd=open(vecfile.c_str(),O_RDWR|O_APPEND);
      if(vfd<0)throw Err("io_error","gc reopen");
      vec_end=n;
      vblocks.clear();
      ensure_map();
    }
    void checkpoint(){
      flush_sst();
      snapshot();
      std::ofstream z(wal,std::ios::trunc);
      z.flush();
      z.close();
      int fd=open(wal.c_str(),O_RDONLY);
      if(fd>=0){
        fdatasync(fd);
        close(fd);
      }
    }
    void load_state(){
      std::ifstream f(dir+"/STATE");
      std::string line;
      if(!std::getline(f,line)||line.empty())return;
      try{
        J z=parse(line);
        if(z.o.count("w"))wal_txid=(uint64_t)z.o["w"].i;
        for(auto&x:z.o["t"].a){
          Table q;
          q.n=x.o["name"].s;
          for(auto&c:x.o["c"].a){
            Col y;
            y.n=c.o["n"].s;
            y.t=c.o["t"].s;
            y.d=(int)c.o["d"].i;
            q.c.push_back(y);
          }
          for(auto&y:x.o["r"].a){
            Row r;
            r.id=y.o["id"].i;
            for(auto&v:y.o["v"].a)r.v.push_back(v.s);
            r.voff.push_back(-1);
            for(auto&o:y.o["o"].a)r.voff.push_back((off_t)o.i);
            q.r[r.id]=r;
            q.order.push_back(r.id);
          }
          tabs[q.n]=q;
        }
        indexes.clear();
        for(auto&i:z.o["i"].a)indexes.insert(i.s);
        index_cols.clear();
        for(auto&i:z.o["c"].o)index_cols[i.first]=(int)i.second.i;
        vec_end=fsize(vecfile);
        ensure_map();
        sindex.clear();
        for(auto&t:tabs){
          rebuild_scalar(t.second);
          if(!load_index(t.second))rebuild_index(t.second);
        }
      }
      catch(...){
        tabs.clear();
        indexes.clear();
        index_cols.clear();
        buckets.clear();
        pqs.clear();
        vblocks.clear();
        sindex.clear();
      }
    }
    static void put32(std::string&o,uint32_t v){
      for(int i=0;i<4;++i)o.push_back((char)(v>>(i*8)));
    }
    static bool get32(const std::string&s,size_t&p,uint32_t&v){
      if(p+4>s.size())return false;
      v=(unsigned char)s[p]|((uint32_t)(unsigned char)s[p+1]<<8)|((uint32_t)(unsigned char)s[p+2]<<16)|((uint32_t)(unsigned char)s[p+3]<<24);
      p+=4;
      return true;
    }
    std::string encode_tokens(const std::string&s)const{
      auto t=tok(s);
      if(t.empty())return {
      };
      std::string k=up(t[0]);
      uint8_t op=0;
      if(k=="CREATE"&&t.size()>1&&up(t[1])=="TABLE")op=1;
      else if(k=="CREATE"&&t.size()>2&&up(t[1])=="VECTOR")op=2;
      else if(k=="INSERT")op=3;
      else if(k=="UPDATE")op=4;
      else if(k=="DELETE")op=5;
      if(!op)return {
      };
      std::string o(1,(char)op);
      put32(o,(uint32_t)t.size());
      for(auto&x:t){
        put32(o,(uint32_t)x.size());
        o+=x;
      }
      return o;
    }
    bool decode_tokens(const std::string&s,std::vector<std::string>&t)const{
      if(s.size()<5)return false;
      uint8_t op=(uint8_t)s[0];
      uint32_t n=0;
      size_t p=1;
      if(op<1||op>5||!get32(s,p,n)||n==0||n>4096)return false;
      t.clear();
      for(uint32_t i=0;i<n;++i){
        uint32_t z=0;
        if(!get32(s,p,z)||z>(1u<<20)||p+z>s.size())return false;
        t.emplace_back(s.data()+p,z);
        p+=z;
      }
      if(p!=s.size()||t.empty())return false;
      std::string k=up(t[0]);
      if((op==1||op==2)&&k!="CREATE")return false;
      if(op==3&&k!="INSERT")return false;
      if(op==4&&k!="UPDATE")return false;
      if(op==5&&k!="DELETE")return false;
      return true;
    }
    void frame(std::ofstream&f,const std::string&s,uint64_t tx,uint16_t kind){
      uint32_t n=(uint32_t)s.size(),magic=WAL_MAGIC,crc=crc32c(s.data(),s.size());
      uint16_t flags=0;
      f.write((char*)&magic,4).write((char*)&n,4).write((char*)&tx,8).write((char*)&kind,2).write((char*)&flags,2).write((char*)&crc,4).write(s.data(),n);
    }
    void flush_vecbuf(){
      if(vfd>=0&&!vecbuf.empty()){
        write(vfd,vecbuf.data(),vecbuf.size());
        vecbuf.clear();
        vecbuf_base=vec_end;
        ensure_map();
      }
    }
    void persist(){
      if(pending.empty())return;
      uint64_t tx=++wal_txid;
      std::ofstream f(wal,std::ios::app|std::ios::binary);
      frame(f,"",tx,1);
      for(auto&s:pending){
        auto x=encode_tokens(s);
        if(!x.empty())frame(f,x,tx,2);
      }
      frame(f,"",tx,3);
      f.flush();
      flush_vecbuf();
      if(vfd>=0)fdatasync(vfd);
      int fd=open(wal.c_str(),O_RDONLY);
      if(fd>=0){
        fdatasync(fd);
        close(fd);
      }
      memtable.insert(memtable.end(),pending.begin(),pending.end());
      pending.clear();
      if(memtable.size()>=16)flush_sst();
      for(auto&kv:index_cols){
        auto it=tabs.find(kv.first);
        if(it!=tabs.end())save_index(it->second);
      }
    }
    void recover(){
      recovering=true;
      load_state();
      uint64_t base=wal_txid;
      vec_cursor=vec_end;
      std::ifstream f(wal,std::ios::binary);
      uint32_t magic,n,crc;
      uint64_t tx=0,active_tx=0;
      uint16_t kind=0,flags=0;
      std::string x;
      std::vector<std::vector<std::string>>b;
      bool active=false,any=false;
      while(f.read((char*)&magic,4)){
        if(!f.read((char*)&n,4)||!f.read((char*)&tx,8)||!f.read((char*)&kind,2)||!f.read((char*)&flags,2)||!f.read((char*)&crc,4)||magic!=WAL_MAGIC||n>(1u<<20))break;
        x.assign(n,0);
        if(!f.read(x.data(),n)||crc32c(x.data(),x.size())!=crc)break;
        wal_txid=std::max(wal_txid,tx);
        bool current=tx>base;
        if(kind==1){
          b.clear();
          active=current;
          active_tx=tx;
        }
        else if(kind==2&&active&&tx==active_tx){
          std::vector<std::string>t;
          if(!decode_tokens(x,t))break;
          b.push_back(std::move(t));
        }
        else if(kind==3&&active&&tx==active_tx){
          for(auto&t:b)apply_tokens(t);
          any=true;
          b.clear();
          active=false;
        }
      }
      if(any){
        trunc_vec(vec_cursor);
        vec_end=vec_cursor;
      }
      recovering=false;
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
      while(p<e){
        while(p<e&&(std::isspace((unsigned char)*p)||*p==','))++p;
        if(p>=e)break;
        bool neg=false;
        if(*p=='-'||*p=='+'){
          neg=*p=='-';
          ++p;
        }
        double val=0;
        int nd=0;
        while(p<e&&std::isdigit((unsigned char)*p)){
          val=val*10+(*p-'0');
          ++p;
          ++nd;
        }
        if(p<e&&*p=='.'){
          ++p;
          double scale=.1;
          while(p<e&&std::isdigit((unsigned char)*p)){
            val+=(*p-'0')*scale;
            scale*=.1;
            ++p;
            ++nd;
          }
        }
        int ex=0;
        if(p<e&&(*p=='e'||*p=='E')){
          ++p;
          bool en=false;
          if(*p=='-'||*p=='+'){
            en=*p=='-';
            ++p;
          }
          int ne=0;
          while(p<e&&std::isdigit((unsigned char)*p)){
            ex=ex*10+(*p-'0');
            ++p;
            ++ne;
          }
          if(!ne)throw Err("invalid_value","vector");
          if(en)ex=-ex;
        }
        if(!nd)throw Err("invalid_value","vector");
        if(ex)val*=std::pow(10.0,ex);
        float x=(float)(neg?-val:val);
        if(!std::isfinite(x))throw Err("invalid_value","nonfinite vector");
        v.push_back(x);
      }
      if(want>=0&&(int)v.size()!=want)throw Err("dimension_mismatch","vector dimension");
    }
    std::vector<float> vec(const std::string&s,int want=-1)const{
      std::vector<float>v;
      parse_into(s,want,v);
      return v;
    }
    off_t append_vec(const std::string&s,int d){
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
      if(vcache.size()<2048)vcache.emplace(s,raw);
      vecbuf.append(raw);
      vec_end+=raw.size();
      return off;
    }
    void ensure_map()const{
      if(mapsz==size_t(vec_end)&&vmap)return;
      if(vmap&&mapsz)munmap(vmap,mapsz);
      vmap=nullptr;
      mapsz=0;
      if(vec_end){
        void*p=mmap(nullptr,vec_end,PROT_READ,MAP_SHARED,vfd,0);
        if(p!=MAP_FAILED){
          vmap=p;
          mapsz=vec_end;
        }
      }
    }
    double l2(off_t off,int d,const std::vector<float>&q)const{
      if(off<0)return 0;
      const float*p=nullptr;
      if(off>=vecbuf_base&&size_t(off-vecbuf_base+d*4)<=vecbuf.size())p=(const float*)(vecbuf.data()+off-vecbuf_base);
      else if(vfd>=0&&vmap&&size_t(off+d*4)<=mapsz)p=(const float*)((const char*)vmap+off);
      if(p){
        if(d>=32&&has_avx2())return l2_avx2(p,q.data(),d);
        double z=0;
        for(int i=0;i<d;++i){
          double x=p[i]-q[i];
          z+=x*x;
        }
        return z;
      }
      if(vfd>=0){
        std::vector<float>v(d);
        pread(vfd,v.data(),d*4,off);
        if(d>=32&&has_avx2())return l2_avx2(v.data(),q.data(),d);
        double z=0;
        for(int i=0;i<d;++i){
          double x=v[i]-q[i];
          z+=x*x;
        }
        return z;
      }
      return 0;
    }
    std::vector<float> read_vec(off_t off,int d)const{
      std::vector<float>v(d);
      if(off<0)return v;
      if(off>=vecbuf_base&&size_t(off-vecbuf_base+d*4)<=vecbuf.size()){
        std::memcpy(v.data(),vecbuf.data()+off-vecbuf_base,d*4);
        return v;
      }
      if(vfd<0)return v;
      if(vmap&&size_t(off+d*4)<=mapsz)std::memcpy(v.data(),(char*)vmap+off,d*4);
      else pread(vfd,v.data(),d*4,off);
      return v;
    }
    std::string canon_insert(const Table&q,const std::vector<std::vector<std::string>>&rows)const{
      std::string s="INSERT INTO "+q.n+" VALUES ";
      for(size_t i=0;i<rows.size();++i){
        if(i)s+=",";
        s+='(';
        for(size_t j=0;j<rows[i].size();++j){
          if(j)s+=',';
          s+=(j>0&&q.c[j].t=="VECTOR"?"@":rows[i][j]);
        }
        s+=')';
      }
      return s+";";
    }
    bool match(const Table&q,const Row&r,const std::vector<Cond>&cs)const{
      for(auto&c:cs){
        std::string a=c.col==0?std::to_string(r.id):unq(r.v[c.col-1]),b=unq(c.val);
        bool num=q.c[c.col].t=="BIGINT";
        if(num){
          long long x=std::stoll(a),y=std::stoll(b);
          if(c.op=="="&&x!=y)return false;
          if(c.op==">"&&x<=y)return false;
          if(c.op=="<"&&x>=y)return false;
          if(c.op==">="&&x<y)return false;
          if(c.op=="<="&&x>y)return false;
        }
        else{
          if(c.op=="="&&a!=b)return false;
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
        r.push_back(c);
      }
      return r;
    }
    void apply_tokens(const std::vector<std::string>&t){
      if(t.empty())return;
      std::string k=up(t[0]);
      if(k=="CREATE"&&t.size()>1&&up(t[1])=="TABLE"){
        create(t,false);
        return;
      }
      if(k=="CREATE"&&t.size()>2&&up(t[1])=="VECTOR"){
        auto&q=tabs.at(t[5]);
        int c=ci(q,t[7]);
        indexes.insert(t[3]);
        index_cols[q.n]=c;
        rebuild_index(q);
        return;
      }
      if(k=="INSERT"){
        insert(t,false);
        return;
      }
      if(k=="UPDATE"){
        update(t,false);
        return;
      }
      if(k=="DELETE"){
        erase(t,false);
        return;
      }
    }
    void apply(const std::string&s){
      apply_tokens(tok(s));
    }
    void begin(){
      if(in)throw Err("transaction_active","already active");
      save=tabs;
      pending.clear();
      vec_mark=vec_end;
      vecbuf_mark=vecbuf.size();
      in=true;
    }
    void rollback(){
      qcache.clear();
      if(!in)return;
      tabs=save;
      pending.clear();
      vecbuf.resize(vecbuf_mark);
      trunc_vec(vec_mark);
      in=false;
      for(auto&kv:tabs){
        rebuild_scalar(kv.second);
        rebuild_index(kv.second);
      }
    }
    void commit(){
      if(!in)return;
      persist();
      in=false;
    }
    void record(const std::string&s){
      qcache.clear();
      pending.push_back(s);
    }
    void write_manifest(){
      std::ofstream m(dir+"/MANIFEST",std::ios::trunc);
      for(auto&kv:levels)for(auto&x:kv.second)m<<kv.first<<" "<<x<<'\n';
      m.flush();
    }
    void compact(){
      for(int lev=0;lev<3;++lev){
        auto&src=levels[lev];
        if(src.size()<4)continue;
        std::vector<std::string>in=src;
        for(auto&x:levels[lev+1])in.push_back(x);
        std::vector<std::ifstream>fs;
        std::vector<std::string>cur;
        for(auto&x:in){
          fs.emplace_back(x);
          std::string y;
          std::getline(fs.back(),y);
          cur.push_back(y);
        }
        std::string p=dir+"/L"+std::to_string(lev+1)+"-"+std::to_string(++sst_seq)+".sst";
        std::ofstream out(p,std::ios::binary);
        for(;;){
          int k=-1;
          for(size_t i=0;i<cur.size();++i)if(!cur[i].empty()&&(k<0||cur[i]<cur[k]))k=(int)i;
          if(k<0)break;
          out<<cur[k]<<'\n';
          if(!std::getline(fs[k],cur[k]))cur[k].clear();
        }
        out.flush();
        int fd=open(p.c_str(),O_RDONLY);
        if(fd>=0){
          fsync(fd);
          close(fd);
        }
        for(auto&x:in)std::remove(x.c_str());
        src.clear();
        levels[lev+1].clear();
        levels[lev+1].push_back(p);
        write_manifest();
      }
    }
    void flush_sst(){
      if(memtable.empty())return;
      immutable=memtable;
      std::vector<std::string>rec;
      for(auto&s:immutable){
        auto t=tok(s);
        rec.push_back((!t.empty()&&up(t[0])=="DELETE"?"T ":"P ")+s);
      }
      std::sort(rec.begin(),rec.end());
      std::string p=dir+"/L0-"+std::to_string(++sst_seq)+".sst";
      std::ofstream f(p,std::ios::binary);
      for(auto&s:rec)f<<s<<'\n';
      f.flush();
      int fd=open(p.c_str(),O_RDONLY);
      if(fd>=0){
        fsync(fd);
        close(fd);
      }
      levels[0].push_back(p);
      write_manifest();
      compact();
      immutable.clear();
      memtable.clear();
    }
    J exec(const std::string&sql){
      auto t=tok(sql);
      if(t.empty())throw Err("invalid_request","empty");
      std::string k=up(t[0]);
      if(k=="BEGIN"){
        begin();
        return jo();
      }
      if(k=="ROLLBACK"){
        rollback();
        return jo();
      }
      if(k=="COMMIT"){
        commit();
        return jo();
      }
      bool readonly=k=="SELECT";
      bool auto_tx=!in&&!readonly;
      if(auto_tx)begin();
      std::map<std::string,Table>before;
      if(!readonly)before=tabs;
      size_t np=pending.size();
      off_t vm=vec_end;
      size_t vb=vecbuf.size();
      try{
        J r;
        if(k=="CREATE"&&t.size()>1&&up(t[1])=="VECTOR")r=create_index(t);
        else if(k=="CREATE")r=create(t,true);
        else if(k=="INSERT")r=insert(t,true);
        else if(k=="UPDATE")r=update(t,true);
        else if(k=="DELETE")r=erase(t,true);
        else if(k=="SELECT"){
          std::string ck=(ann_mode?"A:":"E:")+sql;
          auto qi=qcache.find(ck);
          if(qi!=qcache.end())r=qi->second;
          else{
            flush_vecbuf();
            r=select(t);
            if(qcache.size()<128)qcache.emplace(std::move(ck),r);
          }
        }
        else throw Err("unsupported_sql","statement");
        if(auto_tx)commit();
        return r;
      }
      catch(...){
        if(!readonly){
          tabs=before;
          pending.resize(np);
          vecbuf.resize(vb);
          trunc_vec(vm);
          sindex.clear();
          buckets.clear();
          for(auto&kv:tabs){
            rebuild_scalar(kv.second);
            rebuild_index(kv.second);
          }
        }
        if(auto_tx)rollback();
        throw;
      }
    }
    int bucket_code(const std::vector<float>&v)const{
      if(v.empty())return 0;
      int b=(int)std::floor((v[0]+2.0f)*8.0f);
      return std::max(0,std::min(31,b));
    }
    double coarse_dist(const PQ&z,const std::vector<float>&v,int k)const{
      int cp=std::min(128,z.d);
      double s=0;
      for(int j=0;j<cp;++j){
        int x=(j*z.d)/cp;
        float a=v[x]-z.coarse[k*z.d+x];
        s+=a*a;
      }
      return s;
    }
    int coarse_pick(const PQ&z,const std::vector<float>&v)const{
      int b=0;
      double best=coarse_dist(z,v,0);
      for(int k=1;k<z.nlist;++k){
        double d=coarse_dist(z,v,k);
        if(d<best){
          best=d;
          b=k;
        }
      }
      return b;
    }
    void pq_encode(const PQ&z,const std::vector<float>&v,int&b,std::vector<unsigned char>&out)const{
      b=coarse_pick(z,v);
      int sub=(z.d+z.m-1)/z.m;
      out.assign(z.m,0);
      for(int mi=0;mi<z.m;++mi){
        int best=0;
        double bd=1e300;
        for(int k=0;k<z.ks;++k){
          double d=0;
          for(int j=0;j<sub;++j){
            int x=mi*sub+j;
            if(x<z.d){
              float a=v[x]-z.book[(mi*z.ks+k)*sub+j];
              d+=a*a;
            }
          }
          if(d<bd){
            bd=d;
            best=k;
          }
        }
        out[mi]=(unsigned char)best;
      }
    }
    double pq_adc(const PQ&z,long long id,const std::vector<float>&v)const{
      auto it=z.code.find(id);
      if(it==z.code.end())return 1e300;
      int sub=(z.d+z.m-1)/z.m;
      double d=0;
      for(int mi=0;mi<z.m;++mi){
        int k=it->second[mi];
        for(int j=0;j<sub;++j){
          int x=mi*sub+j;
          if(x<z.d){
            float a=v[x]-z.book[(mi*z.ks+k)*sub+j];
            d+=a*a;
          }
        }
      }
      return d;
    }
    void index_add(Table&q,long long id){
      vblocks.erase(q.n);
      auto it=index_cols.find(q.n);
      if(it==index_cols.end()||!q.r.count(id))return;
      int c=it->second;
      if(q.r[id].voff[c]<0)return;
      auto z=pqs.find(q.n);
      if(z==pqs.end())return;
      auto v=read_vec(q.r[id].voff[c],q.c[c].d);
      int b;
      std::vector<unsigned char>code;
      pq_encode(z->second,v,b,code);
      buckets[q.n][b].push_back(id);
      z->second.code[id]=std::move(code);
      z->second.cell[id]=b;
    }
    void index_remove(Table&q,long long id){
      vblocks.erase(q.n);
      auto it=index_cols.find(q.n);
      if(it==index_cols.end())return;
      for(auto&kv:buckets[q.n]){
        auto&v=kv.second;
        v.erase(std::remove(v.begin(),v.end(),id),v.end());
      }
      auto z=pqs.find(q.n);
      if(z!=pqs.end()){
        z->second.code.erase(id);
        z->second.cell.erase(id);
      }
    }
    void scalar_add(Table&q,long long id){
      auto&row=q.r[id];
      for(size_t c=1;c<q.c.size();++c)if(q.c[c].t!="VECTOR")sindex[q.n+"."+q.c[c].n][unq(row.v[c-1])].push_back(id);
    }
    void scalar_remove(Table&q,long long id){
      if(!q.r.count(id))return;
      auto&row=q.r[id];
      for(size_t c=1;c<q.c.size();++c)if(q.c[c].t!="VECTOR"){
        auto&v=sindex[q.n+"."+q.c[c].n][unq(row.v[c-1])];
        v.erase(std::remove(v.begin(),v.end(),id),v.end());
      }
    }
    void rebuild_scalar(Table&q){
      for(size_t c=1;c<q.c.size();++c)if(q.c[c].t!="VECTOR")sindex.erase(q.n+"."+q.c[c].n);
      for(auto&kv:q.r)scalar_add(q,kv.first);
    }
    void rebuild_index(Table&q){
      ensure_map();
      buckets[q.n].clear();
      pqs.erase(q.n);
      auto it=index_cols.find(q.n);
      if(it==index_cols.end())return;
      int c=it->second;
      if(c<0||c>=(int)q.c.size()||q.c[c].t!="VECTOR")return;
      std::vector<long long>ids;
      std::vector<std::vector<float>>sample;
      size_t stride=std::max<size_t>(1,q.r.size()/4096),seen=0;
      for(auto id:q.order){
        auto ri=q.r.find(id);
        if(ri==q.r.end())continue;
        auto&row=ri->second;
        if(row.voff.size()>=(size_t)q.c.size()&&row.voff[c]>=0){
          ids.push_back(id);
          if(sample.size()<4096&&seen%stride==0)sample.push_back(read_vec(row.voff[c],q.c[c].d));
          ++seen;
        }
      }
      if(ids.empty())return;
      PQ z;
      z.d=q.c[c].d;
      z.nlist=std::min(32,std::max(1,(int)std::sqrt((double)ids.size())));
      z.m=std::min(16,z.d);
      z.ks=std::min(16,std::max(1,(int)sample.size()));
      int sub=(z.d+z.m-1)/z.m;
      z.coarse.resize(z.nlist*z.d);
      for(int k=0;k<z.nlist;++k)z.coarse[k*z.d]=sample[k%sample.size()][0];
      for(int k=0;k<z.nlist;++k)for(int j=0;j<z.d;++j)z.coarse[k*z.d+j]=sample[k%sample.size()][j];
      for(int itn=0;itn<3;++itn){
        std::vector<float>sum(z.nlist*z.d);
        std::vector<int>cnt(z.nlist);
        for(auto&v:sample){
          int b=coarse_pick(z,v);
          ++cnt[b];
          for(int j=0;j<z.d;++j)sum[b*z.d+j]+=v[j];
        }
        for(int k=0;k<z.nlist;++k)if(cnt[k])for(int j=0;j<z.d;++j)z.coarse[k*z.d+j]=sum[k*z.d+j]/cnt[k];
      }
      z.book.resize(z.m*z.ks*sub);
      for(int mi=0;mi<z.m;++mi){
        for(int k=0;k<z.ks;++k)for(int j=0;j<sub;++j){
          int x=mi*sub+j;
          z.book[(mi*z.ks+k)*sub+j]=x<z.d?sample[k%sample.size()][x]:0;
        }
        for(int itn=0;itn<3;++itn){
          std::vector<float>sum(z.ks*sub);
          std::vector<int>cnt(z.ks);
          for(auto&v:sample){
            int best=0;
            double bd=1e300;
            for(int k=0;k<z.ks;++k){
              double d0=0;
              for(int j=0;j<sub;++j){
                int x=mi*sub+j;
                if(x<z.d){
                  float a=v[x]-z.book[(mi*z.ks+k)*sub+j];
                  d0+=a*a;
                }
              }
              if(d0<bd){
                bd=d0;
                best=k;
              }
            }
            ++cnt[best];
            for(int j=0;j<sub;++j){
              int x=mi*sub+j;
              if(x<z.d)sum[best*sub+j]+=v[x];
            }
          }
          for(int k=0;k<z.ks;++k)if(cnt[k])for(int j=0;j<sub;++j)z.book[(mi*z.ks+k)*sub+j]=sum[k*sub+j]/cnt[k];
        }
      }
      pqs[q.n]=std::move(z);
      auto&pq=pqs[q.n];
      for(auto id:ids){
        auto v=read_vec(q.r[id].voff[c],q.c[c].d);
        int b;
        std::vector<unsigned char>code;
        pq_encode(pq,v,b,code);
        buckets[q.n][b].push_back(id);
        pq.code[id]=std::move(code);
        pq.cell[id]=b;
      }
      save_index(q);
    }
    J create_index(const std::vector<std::string>&t){
      if(t.size()<7||up(t[1])!="VECTOR"||up(t[2])!="INDEX"||up(t[4])!="ON")throw Err("unsupported_sql","index");
      if(indexes.count(t[3]))throw Err("duplicate_index","index");
      auto&q=tabs.at(t[5]);
      if(t[6]!="(")throw Err("invalid_request","index");
      int c=ci(q,t[7]);
      if(c<0||q.c[c].t!="VECTOR")throw Err("unsupported_type","index column");
      indexes.insert(t[3]);
      index_cols[q.n]=c;
      rebuild_index(q);
      record("CREATE VECTOR INDEX "+t[3]+" ON "+t[5]+"("+t[7]+");");
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
          c.d=std::stoi(t[i+1]);
          i+=3;
        }
        else if(c.t!="BIGINT")throw Err("unsupported_type","type");
        q.c.push_back(c);
      }
      if(q.c.empty())throw Err("invalid_request","columns");
      tabs[name]=q;
      if(rec){
        std::string s="CREATE TABLE "+name+" (";
        for(size_t i=0;i<q.c.size();++i){
          if(i)s+=",";
          s+=q.c[i].n+" "+q.c[i].t;
          if(q.c[i].d)s+="("+std::to_string(q.c[i].d)+")";
        }
        record(s+");");
      }
      return jo();
    }
    J insert(const std::vector<std::string>&t,bool rec){
      if(t.size()<6||up(t[1])!="INTO"||up(t[3])!="VALUES")throw Err("unsupported_sql","insert");
      auto&q=tabs.at(t[2]);
      std::vector<std::vector<std::string>> rows;
      for(size_t p=4;p<t.size();){
        if(t[p]!="(")throw Err("invalid_request","values");
        ++p;
        std::vector<std::string>v;
        while(p<t.size()&&t[p]!=")"){
          if(t[p]==","){
            ++p;
            continue;
          }
          v.push_back(t[p++]);
        }
        if(p>=t.size())throw Err("invalid_request","values");
        ++p;
        if(v.size()!=q.c.size())throw Err("column_count","values");
        rows.push_back(v);
        while(p<t.size()&&t[p]!="(")++p;
      }
      std::set<long long>seen;
      for(auto&v:rows){
        long long id=std::stoll(v[0]);
        if(q.r.count(id)||!seen.insert(id).second)throw Err("duplicate_key","key");
        for(size_t i=1;i<v.size();++i){
          if(q.c[i].t=="BIGINT")std::stoll(unq(v[i]));
        }
      }
      for(auto&v:rows){
        long long id=std::stoll(v[0]);
        Row r;
        r.id=id;
        r.v.assign(v.begin()+1,v.end());
        r.voff.assign(q.c.size(),-1);
        for(size_t i=1;i<v.size();++i)if(q.c[i].t=="VECTOR"){
          if(recovering&&v[i]=="@"){
            r.voff[i]=vec_cursor;
            vec_cursor+=q.c[i].d*4;
          }
          else r.voff[i]=append_vec(v[i],q.c[i].d);
          r.v[i-1].clear();
        }
        q.r[id]=r;
        q.order.push_back(id);
        scalar_add(q,id);
        index_add(q,id);
      }
      if(rec)record(canon_insert(q,rows));
      J x=jo();
      x.o["affected_rows"]=ji((long long)rows.size());
      return x;
    }
    J update(const std::vector<std::string>&t,bool rec){
      if(t.size()<6||up(t[0])!="UPDATE"||up(t[2])!="SET")throw Err("unsupported_sql","update");
      auto&q=tabs.at(t[1]);
      size_t p=3;
      std::vector<std::pair<int,std::string>>as;
      while(p<t.size()&&up(t[p])!="WHERE"){
        if(t[p]==","){
          ++p;
          continue;
        }
        int c=ci(q,t[p++]);
        if(c<0)throw Err("unknown_column","set");
        if(p>=t.size()||t[p++]!="=")throw Err("invalid_request","set");
        as.push_back({
          c,t[p++]
        });
      }
      std::vector<Cond>cs;
      if(p<t.size())cs=conditions(q,t,p+1,t.size());
      std::vector<long long>ids;
      for(auto id:q.order)if(q.r.count(id)&&match(q,q.r[id],cs))ids.push_back(id);
      for(auto id:ids){
        Row&r=q.r[id];
        scalar_remove(q,id);
        for(auto&a:as){
          if(a.first==0)throw Err("immutable_key","id");
          auto&c=q.c[a.first];
          if(c.t=="BIGINT")std::stoll(unq(a.second));
          if(c.t=="VECTOR"){
            index_remove(q,id);
            r.v[a.first-1].clear();
            if(r.voff.size()<q.c.size())r.voff.assign(q.c.size(),-1);
            if(recovering&&a.second=="@"){
              r.voff[a.first]=vec_cursor;
              vec_cursor+=c.d*4;
            }
            else r.voff[a.first]=append_vec(a.second,c.d);
            index_add(q,id);
          }
          else r.v[a.first-1]=a.second;
        }
        scalar_add(q,id);
      }
      if(rec){
        std::string s="UPDATE "+q.n+" SET ";
        for(size_t i=0;i<as.size();++i){
          if(i)s+=",";
          s+=q.c[as[i].first].n+"="+(q.c[as[i].first].t=="VECTOR"?"@":as[i].second);
        }
        if(!cs.empty()){
          s+=" WHERE ";
          for(size_t i=0;i<cs.size();++i){
            if(i)s+=" AND ";
            s+=q.c[cs[i].col].n+cs[i].op+cs[i].val;
          }
        }
        record(s+";");
      }
      J x=jo();
      x.o["affected_rows"]=ji((long long)ids.size());
      return x;
    }
    J erase(const std::vector<std::string>&t,bool rec){
      if(t.size()<3||up(t[0])!="DELETE"||up(t[1])!="FROM")throw Err("unsupported_sql","delete");
      auto&q=tabs.at(t[2]);
      size_t p=3;
      std::vector<Cond>cs;
      if(p<t.size()&&up(t[p])=="WHERE")cs=conditions(q,t,p+1,t.size());
      std::vector<long long>ids;
      for(auto id:q.order)if(q.r.count(id)&&match(q,q.r[id],cs))ids.push_back(id);
      for(auto id:ids){
        scalar_remove(q,id);
        index_remove(q,id);
        q.r.erase(id);
        q.order.erase(std::remove(q.order.begin(),q.order.end(),id),q.order.end());
      }
      if(rec){
        std::string s="DELETE FROM "+q.n;
        if(!cs.empty()){
          s+=" WHERE ";
          for(size_t i=0;i<cs.size();++i){
            if(i)s+=" AND ";
            s+=q.c[cs[i].col].n+cs[i].op+cs[i].val;
          }
        }
        record(s+";");
      }
      J x=jo();
      x.o["affected_rows"]=ji((long long)ids.size());
      return x;
    }
    std::vector<float> query_vec(const std::vector<std::string>&t)const{
      for(auto&x:t)if(x.find("[")!=std::string::npos)return vec(x);
      throw Err("invalid_request","distance vector");
    }
    J select(const std::vector<std::string>&t){
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
      std::vector<Cond>cs;
      if(wi<t.size())cs=conditions(q,t,wi+1,e);
      bool metric=false;
      std::string alias="distance";
      int mcol=-1;
      std::vector<std::string>proj;
      for(size_t i=1;i<from;){
        if(t[i]==","){
          ++i;
          continue;
        }
        if(up(t[i])=="L2_DISTANCE"){
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
      std::vector<float>qv;
      if(metric){
        qv=query_vec(t);
        if(mcol<=0||mcol>=(int)q.c.size()||(int)qv.size()!=q.c[mcol].d)throw Err("dimension_mismatch","query");
      }
      struct H{
        long long id;
        double d;
        double ord;
      };
      if(metric)ensure_map();
      std::vector<long long>scan=q.order;
      for(auto&c:cs)if(c.op=="="&&c.col>0&&q.c[c.col].t!="VECTOR"){
        auto it=sindex.find(q.n+"."+q.c[c.col].n);
        if(it!=sindex.end()){
          auto jt=it->second.find(unq(c.val));
          if(jt==it->second.end())scan.clear();
          else if(jt->second.size()<scan.size())scan=jt->second;
        }
      }
      size_t ask=li<t.size()?std::stoull(t[li+1]):10;
      if(metric&&ann_mode&&index_cols.count(q.n)&&cs.empty()&&pqs.count(q.n)){
        auto&z=pqs.at(q.n);
        std::vector<std::pair<double,int>>co;
        for(int k=0;k<z.nlist;++k)co.push_back({
          coarse_dist(z,qv,k),k
        });
        std::sort(co.begin(),co.end());
        int probes=std::min(8,z.nlist);
        scan.clear();
        for(int k=0;k<probes;++k){
          auto itb=buckets[q.n].find(co[k].second);
          if(itb!=buckets[q.n].end())scan.insert(scan.end(),itb->second.begin(),itb->second.end());
        }
        std::sort(scan.begin(),scan.end());
        scan.erase(std::unique(scan.begin(),scan.end()),scan.end());
        if(scan.size()>=ask){
          int sub=(z.d+z.m-1)/z.m;
          std::vector<double>ad(z.m*z.ks);
          for(int mi=0;mi<z.m;++mi)for(int k=0;k<z.ks;++k){
            double d0=0;
            for(int j=0;j<sub;++j){
              int x=mi*sub+j;
              if(x<z.d){
                float a=qv[x]-z.book[(mi*z.ks+k)*sub+j];
                d0+=a*a;
              }
            }
            ad[mi*z.ks+k]=d0;
          }
          std::vector<std::pair<double,long long>>ap;
          for(auto id:scan){
            auto ci=z.code.find(id);
            double d0=0;
            if(ci!=z.code.end())for(int mi=0;mi<z.m;++mi)d0+=ad[mi*z.ks+ci->second[mi]];
            ap.push_back({
              d0,id
            });
          }
          std::sort(ap.begin(),ap.end());
          size_t cap=std::min(ap.size(),std::max<size_t>(ask*32,ask));
          scan.clear();
          for(size_t k=0;k<cap;++k)scan.push_back(ap[k].second);
        }
        else scan=q.order;
      }
      bool block_exact=metric&&!ann_mode&&cs.empty()&&oi<t.size()&&li<t.size()&&oi+2<t.size()&&
        (up(t[oi+2])=="L2_DISTANCE"||t[oi+2]==alias)&&
        !(oi+3<t.size()&&up(t[oi+3])=="DESC")&&ask<q.order.size();
      std::vector<H>h;
      auto worse=[](const H&a,const H&b){
        if(a.d!=b.d)return a.d<b.d;
        return a.id<b.id;
      };
      std::priority_queue<H,std::vector<H>,decltype(worse)>top(worse);
      auto add_hit=[&](long long id){
        auto it=q.r.find(id);
        if(it==q.r.end()||!match(q,it->second,cs))return;
        double d=0;
        if(metric){
          if(it->second.voff.size()>=(size_t)q.c.size()&&it->second.voff[mcol]>=0)d=l2(it->second.voff[mcol],q.c[mcol].d,qv);
          else{
            auto v=vec(it->second.v[mcol-1],q.c[mcol].d);
            for(size_t k=0;k<v.size();++k){
              double z=v[k]-qv[k];
              d+=z*z;
            }
          }
        }
        double ov=0;
        if(oi<t.size()){
          std::string on=oi+2<t.size()?t[oi+2]:"";
          if(up(on)=="L2_DISTANCE"||on==alias)ov=d;
          else{
            int c=ci(q,on);
            if(c==0)ov=id;
            else if(c>0&&q.c[c].t=="BIGINT")ov=std::stod(unq(it->second.v[c-1]));
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
      if(block_exact){
        int bc=mcol;
        auto&bs=vblocks[q.n];
        if(bs.empty()){
          int d=q.c[bc].d,sd=std::min(32,d);
          for(size_t b=0;b<q.order.size();b+=64){
            VBlock w;
            w.begin=b;
            w.end=std::min(q.order.size(),b+64);
            w.lo.assign(sd,1e30f);
            w.hi.assign(sd,-1e30f);
            for(size_t p=w.begin;p<w.end;++p){
              auto it=q.r.find(q.order[p]);
              if(it==q.r.end())continue;
              auto v=read_vec(it->second.voff[bc],d);
              for(int j=0;j<sd;++j){
                int x=(j*d)/sd;
                w.lo[j]=std::min(w.lo[j],v[x]);
                w.hi[j]=std::max(w.hi[j],v[x]);
              }
            }
            bs.push_back(std::move(w));
          }
        }
        std::vector<std::pair<double,size_t>>bo;
        int d=q.c[bc].d,sd=std::min(32,d);
        for(size_t bi=0;bi<bs.size();++bi){
          double z=0;
          for(int j=0;j<sd;++j){
            int x=(j*d)/sd;
            double a=qv[x];
            if(a<bs[bi].lo[j]){
              double e=bs[bi].lo[j]-a;
              z+=e*e;
            }
            else if(a>bs[bi].hi[j]){
              double e=a-bs[bi].hi[j];
              z+=e*e;
            }
          }
          bo.push_back({
            z,bi
          });
        }
        std::sort(bo.begin(),bo.end());
        for(auto&kv:bo){
          if(ask&&top.size()>=ask){
            double worst=top.top().d;
            if(kv.first>worst)break;
          }
          for(size_t p=bs[kv.second].begin;p<bs[kv.second].end;++p)add_hit(q.order[p]);
        }
      }
      else for(auto id:scan)add_hit(id);
      if(block_exact){
        while(!top.empty()){
          h.push_back(top.top());
          top.pop();
        }
      }
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
          if(a.ord!=b.ord)return desc?a.ord>b.ord:a.ord<b.ord;return a.id<b.id;
        });
      }
      size_t lim=h.size();
      if(li<t.size())lim=std::stoull(t[li+1]);
      J out=jo(),cols=ja(),rows=ja();
      for(auto&p:proj)cols.a.push_back(js(p));
      if(metric)cols.a.push_back(js(alias));
      for(size_t n=0;n<std::min(lim,h.size());++n){
        auto&hh=h[n];
        auto&row=q.r[hh.id];
        J a=ja();
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
        if(metric)a.a.push_back(jd(std::sqrt(hh.d)));
        rows.a.push_back(a);
      }
      out.o["columns"]=cols;
      out.o["rows"]=rows;
      J mt=jo();
      mt.o["planner_path"]=js(metric&&ann_mode&&index_cols.count(q.n)&&cs.empty()&&pqs.count(q.n)?"ivf_pq":"exact_scan");
      mt.o["candidate_count"]=ji((long long)scan.size());
      mt.o["exact_evaluations"]=ji((long long)h.size());
      out.o["metrics"]=mt;
      return out;
    }
  };
}
int main(int argc,char**argv){
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
        std::cout<<"{\"ok\":false,\"error\":{\"code\":\"invalid_request\",\"message\":\"failure\"}}\n"<<std::flush;
      }
    }
  }
  catch(const std::exception&x){
    std::cerr<<x.what()<<"\n";
    return 1;
  }
}
