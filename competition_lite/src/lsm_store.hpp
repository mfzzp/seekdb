#pragma once

// Included inside namespace lite. SSTs and the manifest, rather than a second
// snapshot of the row cache, are authoritative. All publication is single writer.
namespace lsm {
inline void u32(std::string& s,uint32_t x){for(int i=0;i<4;++i)s+=char(x>>(8*i));}
inline void u64(std::string& s,uint64_t x){for(int i=0;i<8;++i)s+=char(x>>(8*i));}
inline void str(std::string& s,const std::string& x){u32(s,(uint32_t)x.size());s+=x;}
struct Decode {
  const std::string& s;size_t p=0;
  void need(size_t n){if(n>s.size()-p)throw Err("corruption","truncated storage record");}
  uint32_t u32(){need(4);uint32_t x=0;for(int i=0;i<4;++i)x|=uint32_t((unsigned char)s[p++])<<(8*i);return x;}
  uint64_t u64(){need(8);uint64_t x=0;for(int i=0;i<8;++i)x|=uint64_t((unsigned char)s[p++])<<(8*i);return x;}
  std::string str(){size_t n=u32();need(n);std::string x=s.substr(p,n);p+=n;return x;}
  bool bit(){need(1);auto c=(unsigned char)s[p++];if(c>1)throw Err("corruption","invalid tombstone");return c;}
  void end(){if(p!=s.size())throw Err("corruption","storage record trailing bytes");}
};
inline void io(bool good,const char* what){if(!good)throw Err("io_error",what);}
inline void write_all(int fd,const char* p,size_t n){while(n){ssize_t z=::write(fd,p,n);if(z<0&&errno==EINTR)continue;io(z>0,"storage write");p+=z;n-=z;}}
inline void sync(int fd,bool data=false){int z;do{z=data?fdatasync(fd):fsync(fd);}while(z<0&&errno==EINTR);io(z==0,"storage sync");}
struct FD {
  int n=-1;
  explicit FD(int x=-1):n(x){}
  ~FD(){if(n>=0)::close(n);}
  FD(const FD&)=delete;FD& operator=(const FD&)=delete;
};
inline void sync_dir(const std::string& dir){FD f(open(dir.c_str(),O_RDONLY|O_DIRECTORY));io(f.n>=0,"open storage directory");sync(f.n);}
inline std::string read_at(int fd,uint64_t off,size_t n){
  std::string s(n,'\0');size_t p=0;
  while(p<n){ssize_t z=pread(fd,s.data()+p,n-p,off+p);if(z<0&&errno==EINTR)continue;io(z>=0,"storage read");if(!z)throw Err("corruption","short storage file");p+=z;}return s;
}
inline uint64_t file_size(int fd){struct stat s{};io(fstat(fd,&s)==0&&s.st_size>=0,"storage stat");return s.st_size;}
inline bool exists(const std::string& p){struct stat s{};return stat(p.c_str(),&s)==0;}
inline std::string checked(const std::string& s){std::string x;u32(x,crc32c(s.data(),s.size()));return x+s;}
inline std::string uncheck(const std::string& s){Decode d{s};uint32_t crc=d.u32();std::string x=s.substr(4);if(crc32c(x.data(),x.size())!=crc)throw Err("corruption","storage checksum");return x;}
inline std::string encode(const SRec& r){std::string s;str(s,r.key);str(s,r.value);u64(s,r.seq);s+=char(r.tomb);return s;}
inline SRec decode(Decode& d){SRec r;r.key=d.str();r.value=d.str();r.seq=d.u64();r.tomb=d.bit();return r;}
inline std::string upper(std::string s){for(size_t p=s.size();p;){--p;if((unsigned char)s[p]!=255){++s[p];s.resize(p+1);return s;}}return {};}
inline std::string part(const std::string& s){std::string z;for(char c:s){z+=c;if(!c)z+=char(255);}z+=char(0);z+=char(0);return z;}
inline std::string number(uint64_t x){std::string s;for(int i=7;i>=0;--i)s+=char(x>>(8*i));return s;}
inline uint64_t number_value(const std::string& s,size_t p){if(p+8>s.size())throw Err("corruption","storage key");uint64_t x=0;for(int i=0;i<8;++i)x=(x<<8)|(unsigned char)s[p+i];return x;}

struct File {
  std::string path,lo,hi;uint64_t bytes=0,maxseq=0;int level=0;bool retired=false;
  ~File(){if(retired)unlink(path.c_str());}
};
using F=std::shared_ptr<File>;
struct Index {std::string key;uint64_t off=0;uint32_t bytes=0;};
struct Writer {
  F file;FD fd;std::string block;uint32_t count=0;std::vector<Index> index;std::string first,last;
  explicit Writer(F f):file(std::move(f)),fd(open(file->path.c_str(),O_WRONLY|O_CREAT|O_EXCL,0644)){
    io(fd.n>=0,"create SST");write_all(fd.n,"LITESST3",8);file->bytes=8;
  }
  void flush(){if(!count)return;std::string p;u32(p,count);p+=block;std::string b=checked(p),h;u32(h,(uint32_t)b.size());
    index.push_back({first,file->bytes,(uint32_t)(h.size()+b.size())});write_all(fd.n,h.data(),h.size());write_all(fd.n,b.data(),b.size());file->bytes+=h.size()+b.size();block.clear();count=0;}
  void add(const SRec& r){
    if(!last.empty()&&r.key<=last)throw Err("corruption","SST input is not strictly ordered");
    if(!count)first=r.key;if(file->lo.empty())file->lo=r.key;file->hi=r.key;last=r.key;
    file->maxseq=std::max(file->maxseq,r.seq);block+=encode(r);++count;if(block.size()>=32768)flush();
  }
  F finish(bool durable=true){flush();std::string x;u32(x,(uint32_t)index.size());for(auto&i:index){str(x,i.key);u64(x,i.off);u32(x,i.bytes);}x=checked(x);
    write_all(fd.n,x.data(),x.size());std::string foot;u64(foot,x.size());foot+="LSMEND03";write_all(fd.n,foot.data(),foot.size());file->bytes+=x.size()+foot.size();if(durable)sync(fd.n);return file;}
};
struct Cursor {SRec row;bool valid=false;virtual void next()=0;virtual ~Cursor()=default;};
struct DiskCursor:Cursor {
  F file;FD fd;std::vector<Index> index;size_t bi=0,p=0;uint32_t left=0;std::string block,high;
  DiskCursor(F f,const std::string& low={},std::string hi={}):file(std::move(f)),fd(open(file->path.c_str(),O_RDONLY)),high(std::move(hi)){
    io(fd.n>=0,"open referenced SST");uint64_t sz=file_size(fd.n);if(sz<24||read_at(fd.n,0,8)!="LITESST3")throw Err("corruption","SST header");
    std::string foot=read_at(fd.n,sz-16,16);Decode z{foot};uint64_t n=z.u64();
    if(foot.substr(8)!="LSMEND03"||n>sz-24||n>(64u<<20))throw Err("corruption","SST footer");
    std::string ix=uncheck(read_at(fd.n,sz-16-n,(size_t)n));Decode d{ix};uint32_t ni=d.u32();if(ni>ix.size()/16)throw Err("corruption","SST index count");
    uint64_t end=8;for(uint32_t i=0;i<ni;++i){Index a{d.str(),d.u64(),d.u32()};if(a.off!=end||a.bytes<12||a.off+a.bytes>sz-16-n||(!index.empty()&&a.key<=index.back().key))throw Err("corruption","SST index");end=a.off+a.bytes;index.push_back(std::move(a));}d.end();
    if(end!=sz-16-n)throw Err("corruption","SST block boundary");
    if(!low.empty()){auto it=std::upper_bound(index.begin(),index.end(),low,[](const std::string& k,const Index& i){return k<i.key;});bi=it==index.begin()?0:size_t(it-index.begin()-1);}
    next();while(valid&&row.key<low)next();
  }
  void next()override{
    if(!left){if(bi==index.size()){valid=false;return;}auto&i=index[bi++];std::string b=read_at(fd.n,i.off,i.bytes);Decode h{b};if(h.u32()!=b.size()-4)throw Err("corruption","SST block length");block=uncheck(b.substr(4));Decode c{block};left=c.u32();p=c.p;if(!left)throw Err("corruption","empty SST block");}
    Decode d{block,p};row=decode(d);p=d.p;--left;if(!left)d.end();valid=high.empty()||row.key<high;
  }
};
struct MemCursor:Cursor {
  std::map<std::string,SRec> rows;std::map<std::string,SRec>::iterator it;std::string high;
  MemCursor(std::map<std::string,SRec> m,const std::string& lo,std::string hi):rows(std::move(m)),it(rows.lower_bound(lo)),high(std::move(hi)){next();}
  void next()override{valid=it!=rows.end()&&(high.empty()||it->first<high);if(valid)row=(it++)->second;}
};
struct ConcatCursor:Cursor {
  std::vector<F> files;size_t pos=0;std::unique_ptr<DiskCursor> c;std::string low,high;
  ConcatCursor(std::vector<F> fs,std::string lo,std::string hi):files(std::move(fs)),low(std::move(lo)),high(std::move(hi)){next();}
  void next()override{if(c)c->next();for(;;){if(c&&c->valid){row=c->row;valid=true;return;}if(pos==files.size()){valid=false;return;}c=std::make_unique<DiskCursor>(files[pos++],low,high);}}
};
struct Merge:Cursor {
  std::vector<std::unique_ptr<Cursor>> cs;
  struct Item {std::string key;size_t i;bool operator>(const Item& x)const{return key>x.key;}};
  std::priority_queue<Item,std::vector<Item>,std::greater<Item>> heap;bool live;
  explicit Merge(std::vector<std::unique_ptr<Cursor>> v,bool live_only):cs(std::move(v)),live(live_only){for(size_t i=0;i<cs.size();++i)if(cs[i]->valid)heap.push({cs[i]->row.key,i});next();}
  void next()override{while(!heap.empty()){std::string key=heap.top().key;bool chosen=false;SRec best;
      do{size_t i=heap.top().i;heap.pop();const auto&r=cs[i]->row;if(!chosen||r.seq>best.seq){best=r;chosen=true;}else if(r.seq==best.seq&&(r.tomb!=best.tomb||r.value!=best.value))throw Err("corruption","conflicting record versions");cs[i]->next();if(cs[i]->valid)heap.push({cs[i]->row.key,i});}while(!heap.empty()&&heap.top().key==key);
      if(live&&best.tomb)continue;row=std::move(best);valid=true;return;}valid=false;}
};
struct Overlay:Cursor {
  std::unique_ptr<Cursor> c;
  explicit Overlay(std::unique_ptr<Cursor> x):c(std::move(x)){copy();}
  void copy(){valid=c->valid;if(valid){row=c->row;row.seq=UINT64_MAX;}}
  void next()override{c->next();copy();}
};
inline void add_files(std::vector<std::unique_ptr<Cursor>>& cs,const std::vector<F>& files,const std::string& lo,const std::string& hi){
  std::map<int,std::vector<F>> levels;
  for(auto&f:files)if((lo.empty()||f->hi>=lo)&&(hi.empty()||f->lo<hi)){
    if(f->level==0)cs.push_back(std::make_unique<DiskCursor>(f,lo,hi));else levels[f->level].push_back(f);
  }
  for(auto&l:levels){auto&v=l.second;std::sort(v.begin(),v.end(),[](const F&a,const F&b){return a->lo<b->lo;});cs.push_back(std::make_unique<ConcatCursor>(std::move(v),lo,hi));}
}

// Private, spillable write sets. Sequence numbers here are private ordering only;
// committed versions are assigned once, in the checksummed WAL batch.
struct Batch {
  std::map<std::string,SRec> mem;std::vector<F> runs;size_t bytes=0;uint64_t serial=0;
  size_t limit;std::function<F()> allocate;
  Batch(size_t lim,std::function<F()> f):limit(lim),allocate(std::move(f)){}
  bool empty()const{return mem.empty()&&runs.empty();}
  std::unique_ptr<Merge> scan(const std::string& lo={},const std::string& hi={})const{
    std::vector<std::unique_ptr<Cursor>> cs;if(!mem.empty())cs.push_back(std::make_unique<MemCursor>(mem,lo,hi));add_files(cs,runs,lo,hi);return std::make_unique<Merge>(std::move(cs),false);
  }
  void spill(){if(mem.empty())return;F f=allocate();f->retired=true;Writer w(f);for(auto&x:mem)w.add(x.second);runs.push_back(w.finish(false));mem.clear();bytes=0;
    // Size-tiered private runs avoid rewriting the entire transaction after
    // every few statements, while keeping only O(log(transaction size)) runs.
    for(;;){std::map<int,std::vector<F>> tiers;for(auto&r:runs){int t=0;for(uint64_t n=r->bytes/std::max<size_t>(1,limit);n>=8;n/=8)++t;tiers[t].push_back(r);}std::vector<F> selected;for(auto&t:tiers)if(t.second.size()>=8){selected.assign(t.second.begin(),t.second.begin()+8);break;}if(selected.empty())break;
      std::vector<std::unique_ptr<Cursor>> cs;add_files(cs,selected,{},{});Merge c(std::move(cs),false);F out=allocate();out->retired=true;Writer z(out);while(c.valid){z.add(c.row);c.next();}auto f2=z.finish(false);runs.erase(std::remove_if(runs.begin(),runs.end(),[&](const F&r){return std::find(selected.begin(),selected.end(),r)!=selected.end();}),runs.end());runs.push_back(std::move(f2));}}
  void put(std::string key,std::string value,bool tomb=false){auto it=mem.find(key);if(it!=mem.end())bytes-=it->second.key.size()+it->second.value.size()+96;
    SRec r{std::move(key),std::move(value),++serial,tomb};bytes+=r.key.size()+r.value.size()+96;mem[r.key]=std::move(r);if(bytes>=limit)spill();}
  bool get(const std::string& key,SRec& r)const{auto it=mem.find(key);if(it!=mem.end()){r=it->second;return true;}bool found=false;for(auto&f:runs)if(key>=f->lo&&key<=f->hi&&(!found||f->maxseq>r.seq)){DiskCursor c(f,key);if(c.valid&&c.row.key==key&&(!found||c.row.seq>r.seq)){r=c.row;found=true;}}return found;}
  void absorb(Batch& b){auto c=b.scan();while(c->valid){auto&r=c->row;put(r.key,r.value,r.tomb);c->next();}}
};

struct Store {
  std::string dir,manifest,vector_name="vectors.f32",wal_name;FD lock;int wfd=-1;
  std::map<std::string,SRec> mem;std::vector<F> files;size_t mem_bytes=0,limit;
  uint64_t seq=0,checkpoint_seq=0,vector_end=0,next_file=0,wal_bytes=0,bytes_in=0,bytes_out=0;
  std::string generation;bool failed=false,migrating=false;
  explicit Store(std::string root,bool migration=false):dir(std::move(root)),manifest(migration?"LSM-MIGRATION":"LSM-MANIFEST"),lock(open((dir+"/LOCK").c_str(),O_RDWR|O_CREAT,0644)),limit((size_t)envint("LITE_MEMTABLE_BYTES",4<<20)),migrating(migration){
    io(lock.n>=0&&flock(lock.n,LOCK_EX|LOCK_NB)==0,"database already open or lock unavailable");
    generation=std::to_string((uint64_t)std::chrono::system_clock::now().time_since_epoch().count())+"-"+std::to_string(getpid());
    if(!migration&&exists(dir+"/LSM-MANIFEST")){load();open_wal();replay();}
    else{wal_name=fresh("wal");FD f(open((dir+"/"+wal_name).c_str(),O_WRONLY|O_CREAT|O_EXCL,0644));io(f.n>=0,"create WAL");sync(f.n);publish(files,0);open_wal();}
  }
  ~Store(){if(wfd>=0)close(wfd);}
  std::string fresh(const std::string& ext){return "lsm-"+generation+"-"+std::to_string(++next_file)+"."+ext;}
  F allocate(int level=0,bool temporary=false){auto f=std::make_shared<File>();f->path=dir+"/"+fresh(temporary?"txn":"sst");f->level=level;return f;}
  std::unique_ptr<Batch> batch(){return std::make_unique<Batch>(limit,[this](){return allocate(0,true);});}
  void open_wal(){wfd=open((dir+"/"+wal_name).c_str(),O_RDWR|O_APPEND);io(wfd>=0,"open referenced WAL");wal_bytes=file_size(wfd);}
  void publish(const std::vector<F>& next,uint64_t high){
    std::string body="LITEMAN3";u64(body,high);u64(body,vector_end);str(body,vector_name);str(body,wal_name);u32(body,(uint32_t)next.size());
    for(auto&f:next){u32(body,(uint32_t)f->level);str(body,f->path.substr(dir.size()+1));str(body,f->lo);str(body,f->hi);u64(body,f->bytes);u64(body,f->maxseq);}
    body=checked(body);std::string tmp=dir+"/"+fresh("manifest");FD f(open(tmp.c_str(),O_WRONLY|O_CREAT|O_EXCL,0644));io(f.n>=0,"create manifest");write_all(f.n,body.data(),body.size());sync(f.n);sync_dir(dir);
    io(rename(tmp.c_str(),(dir+"/"+manifest).c_str())==0,"publish manifest");
    // Do not retire either version if directory sync fails after rename.
    sync_dir(dir);files=next;checkpoint_seq=high;
  }
  static bool safe_name(const std::string& s){return !s.empty()&&s.find('/')==std::string::npos&&s.find("..") ==std::string::npos;}
  void load(){FD f(open((dir+"/"+manifest).c_str(),O_RDONLY));io(f.n>=0,"open manifest");uint64_t n=file_size(f.n);if(n>(64u<<20))throw Err("corruption","manifest size");std::string b=uncheck(read_at(f.n,0,n));Decode d{b};d.need(8);if(b.substr(0,8)!="LITEMAN3")throw Err("corruption","manifest format");d.p=8;
    seq=checkpoint_seq=d.u64();vector_end=d.u64();vector_name=d.str();wal_name=d.str();if(!safe_name(vector_name)||!safe_name(wal_name))throw Err("corruption","manifest filename");uint32_t count=d.u32();if(count>b.size()/32)throw Err("corruption","manifest files");
    std::map<int,std::vector<F>> levels;std::set<std::string> names;
    for(uint32_t i=0;i<count;++i){auto x=std::make_shared<File>();x->level=(int)d.u32();std::string name=d.str();if(!safe_name(name)||!names.insert(name).second||x->level<0||x->level>30)throw Err("corruption","manifest SST");x->path=dir+"/"+name;x->lo=d.str();x->hi=d.str();x->bytes=d.u64();x->maxseq=d.u64();if(x->lo>x->hi||x->maxseq>seq)throw Err("corruption","manifest key range");FD sf(open(x->path.c_str(),O_RDONLY));io(sf.n>=0,"missing referenced SST");if(file_size(sf.n)!=x->bytes)throw Err("corruption","SST size mismatch");files.push_back(x);levels[x->level].push_back(x);}
    d.end();for(auto&l:levels)if(l.first){auto&v=l.second;std::sort(v.begin(),v.end(),[](const F&a,const F&b){return a->lo<b->lo;});for(size_t i=1;i<v.size();++i)if(v[i-1]->hi>=v[i]->lo)throw Err("corruption","overlapping leveled SSTs");}
  }
  std::unique_ptr<Merge> scan(const std::string& lo={},const std::string& hi={},bool live=true)const{
    std::vector<std::unique_ptr<Cursor>> cs;if(!mem.empty())cs.push_back(std::make_unique<MemCursor>(mem,lo,hi));add_files(cs,files,lo,hi);return std::make_unique<Merge>(std::move(cs),live);
  }
  bool get(const std::string& key,SRec& best)const{bool found=false;auto it=mem.find(key);if(it!=mem.end()){best=it->second;found=true;}
    for(auto&f:files)if(key>=f->lo&&key<=f->hi&&(!found||f->maxseq>best.seq)){DiskCursor c(f,key);if(c.valid&&c.row.key==key&&(!found||c.row.seq>best.seq)){best=c.row;found=true;}}return found;
  }
  void apply(const SRec& r){seq=std::max(seq,r.seq);auto it=mem.find(r.key);if(it!=mem.end()){if(it->second.seq>r.seq)return;mem_bytes-=it->second.key.size()+it->second.value.size()+96;}mem[r.key]=r;mem_bytes+=r.key.size()+r.value.size()+96;bytes_in+=r.key.size()+r.value.size()+17;if(mem_bytes>=limit)flush();}
  std::vector<F> write_stream(Cursor& c,int level,bool drop_tombs=false){std::vector<F> out;std::unique_ptr<Writer> w;size_t target=(size_t)envint("LITE_SST_BYTES",8<<20);
    while(c.valid){if(!drop_tombs||!c.row.tomb){if(!w)w=std::make_unique<Writer>(allocate(level));w->add(c.row);if(w->file->bytes+w->block.size()>=target){out.push_back(w->finish());w.reset();}}c.next();}if(w)out.push_back(w->finish());return out;
  }
  void retire(const std::vector<F>& old){for(auto&f:old)f->retired=true;}
  void flush(){if(mem.empty())return;auto frozen=std::move(mem);mem.clear();mem_bytes=0;
    try{MemCursor c(std::move(frozen),{},{});auto out=write_stream(c,0);auto next=files;next.insert(next.end(),out.begin(),out.end());publish(next,seq);for(auto&f:out)bytes_out+=f->bytes;compact(false);}catch(...){failed=true;throw;}
  }
  void compact(bool full=false){
    if(full){flush();if(files.empty())return;auto old=files;auto c=scan({}, {},false);auto out=write_stream(*c,1,true);publish(out,seq);retire(old);for(auto&f:out)bytes_out+=f->bytes;return;}
    for(int level=0;level<30;++level){std::vector<F> src;uint64_t bytes=0,budget=uint64_t(envint("LITE_SST_BYTES",8<<20))*4;for(int l=1;l<level&&budget<(1ull<<60);++l)budget*=8;
      for(auto&f:files)if(f->level==level){src.push_back(f);bytes+=f->bytes;}
      if((level==0&&src.size()<4)||(level>0&&bytes<=budget))continue;if(src.empty())continue;
      // Compact one bounded source file at higher levels and every overlapping
      // target file. The merge holds one block per sorted run, not all rows.
      if(level>0){std::sort(src.begin(),src.end(),[](const F&a,const F&b){return a->maxseq<b->maxseq;});src.resize(1);}
      std::string lo=src.front()->lo,hi=src.front()->hi;for(auto&f:src){lo=std::min(lo,f->lo);hi=std::max(hi,f->hi);}for(auto&f:files)if(f->level==level+1&&f->hi>=lo&&f->lo<=hi)src.push_back(f);
      std::vector<std::unique_ptr<Cursor>> cs;add_files(cs,src,{},{});Merge c(std::move(cs),false);auto out=write_stream(c,level+1);std::vector<F> next;for(auto&f:files)if(std::find(src.begin(),src.end(),f)==src.end())next.push_back(f);next.insert(next.end(),out.begin(),out.end());publish(next,checkpoint_seq);retire(src);for(auto&f:out)bytes_out+=f->bytes;
      if(level>0)--level;
    }
  }
  void frame(unsigned char kind,const std::string& data){std::string p(1,char(kind));p+=data;std::string b=checked(p),h;u32(h,(uint32_t)b.size());write_all(wfd,h.data(),h.size());write_all(wfd,b.data(),b.size());wal_bytes+=h.size()+b.size();}
  bool read_frame(uint64_t& at,uint64_t end,unsigned char& kind,std::string& data){if(end-at<4)return false;std::string h=read_at(wfd,at,4);Decode d{h};uint32_t n=d.u32();if(n<5||n>(64u<<20))throw Err("corruption","WAL frame length");if(n>end-at-4)return false;std::string b=uncheck(read_at(wfd,at+4,n));kind=(unsigned char)b[0];data=b.substr(1);at+=4+n;return true;}
  void commit(Batch& batch,uint64_t vend){if(batch.empty())return;try{uint64_t count=0;auto c=batch.scan();while(c->valid){++count;c->next();}if(count>UINT64_MAX-seq)throw Err("io_error","version overflow");
      std::string begin;u64(begin,seq+1);u64(begin,count);u64(begin,vend);frame(1,begin);uint64_t next=seq;std::string block;c=batch.scan();
      while(c->valid){SRec r=c->row;r.seq=++next;block+=encode(r);if(block.size()>=65536){frame(2,block);block.clear();}c->next();}if(!block.empty())frame(2,block);frame(3,begin);sync(wfd,true);
      vector_end=vend;uint64_t n=seq;c=batch.scan();while(c->valid){SRec r=c->row;r.seq=++n;apply(r);c->next();}
      if(wal_bytes>=(32u<<20))checkpoint();
    }catch(...){failed=true;throw;}}
  void replay(){uint64_t end=file_size(wfd),pos=0,good=0,start=0,first=0,count=0,vend=vector_end,seen=0;std::string begin;bool active=false;
    unsigned char kind=0;std::string data;
    while(read_frame(pos,end,kind,data)){
      if(kind==1){if(active)throw Err("corruption","nested WAL batch");Decode d{data};first=d.u64();count=d.u64();vend=d.u64();d.end();if(!first||!count||count>UINT64_MAX-first)throw Err("corruption","WAL versions");begin=data;start=pos;seen=0;active=true;}
      else if(kind==2&&active){Decode d{data};while(d.p<data.size()){auto r=decode(d);if(r.seq!=first+seen++)throw Err("corruption","WAL record order");}}
      else if(kind==3&&active&&data==begin&&seen==count){vector_end=std::max(vector_end,vend);uint64_t p=start;while(p<pos){unsigned char k;std::string b;if(!read_frame(p,pos,k,b))throw Err("corruption","WAL replay");if(k==3)break;Decode d{b};while(d.p<b.size()){auto r=decode(d);if(r.seq>checkpoint_seq)apply(r);}}seq=std::max(seq,first+count-1);good=pos;active=false;}
      else throw Err("corruption","WAL batch framing");
    }
    io(ftruncate(wfd,(off_t)good)==0,"truncate incomplete WAL tail");wal_bytes=good;
  }
  void checkpoint(){try{flush();std::string old=wal_name;std::string next=fresh("wal");FD fd(open((dir+"/"+next).c_str(),O_RDWR|O_APPEND|O_CREAT|O_EXCL,0644));io(fd.n>=0,"create checkpoint WAL");sync(fd.n);wal_name=next;
      publish(files,seq);if(wfd>=0)close(wfd);wfd=fd.n;fd.n=-1;wal_bytes=0;unlink((dir+"/"+old).c_str());
    }catch(...){failed=true;throw;}}
  void finish_migration(){checkpoint();io(rename((dir+"/"+manifest).c_str(),(dir+"/LSM-MANIFEST").c_str())==0,"publish migration");sync_dir(dir);manifest="LSM-MANIFEST";migrating=false;}
  void cleanup(){
    std::set<std::string> live{wal_name,vector_name};for(auto&f:files)live.insert(f->path.substr(dir.size()+1));
    DIR*d=opendir(dir.c_str());io(d!=nullptr,"list orphan files");
    while(auto*e=readdir(d)){std::string n=e->d_name;if(n.compare(0,4,"lsm-")||live.count(n))continue;auto dot=n.find_last_of('.');if(dot==std::string::npos)continue;auto ext=n.substr(dot);if(ext!=".sst"&&ext!=".txn"&&ext!=".manifest"&&ext!=".wal"&&ext!=".vectors")continue;std::string p=dir+"/"+n;struct stat st{};if(lstat(p.c_str(),&st)==0&&S_ISREG(st.st_mode))unlink(p.c_str());}closedir(d);
  }
  void verify()const{for(auto&f:files){DiskCursor c(f);std::string prev;uint64_t maxseq=0;while(c.valid){if(!prev.empty()&&c.row.key<=prev)throw Err("corruption","SST key order");if(prev.empty()&&c.row.key!=f->lo)throw Err("corruption","SST lower key");prev=c.row.key;maxseq=std::max(maxseq,c.row.seq);c.next();}if(prev!=f->hi||maxseq!=f->maxseq)throw Err("corruption","SST upper key or sequence");}}
  void replace(std::vector<F> next,const std::string& vector,uint64_t vend){auto old=files;auto prev=vector_name;vector_name=vector;vector_end=vend;
    try{publish(next,seq);}catch(...){failed=true;throw;}retire(old);if(prev!=vector)unlink((dir+"/"+prev).c_str());
  }
};
} // namespace lsm
