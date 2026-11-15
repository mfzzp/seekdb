#!/usr/bin/env python3
"""Create a separate instrumented source. Never use its binary for final timing.

Each edit requires one exact source match. Only clocks/counters are inserted;
the production source, sync calls and query answers are unchanged.
"""
import argparse
import hashlib
import json
from pathlib import Path

HEADER = r'''
namespace diagnostic {
  using Clock=std::chrono::steady_clock;
  struct Stat { uint64_t count=0,rows=0,logical_bytes=0; double ms=0; };
  static std::map<std::string,Stat> stats;
  struct Timer {
    Stat& stat; Clock::time_point start;
    explicit Timer(const char*name):stat(stats[name]),start(Clock::now()){++stat.count;}
    ~Timer(){stat.ms+=std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
  };
  template<class Tables> void copy_size(const char*name,const Tables&tabs){
    auto&s=stats[name];
    for(const auto&t:tabs){
      const auto&q=t.second;s.rows+=q.r.size();
      s.logical_bytes+=sizeof(q)+q.order.size()*sizeof(long long)+q.fast.size()*sizeof(void*);
      for(const auto&v:q.r){const auto&r=v.second;
        s.logical_bytes+=sizeof(r)+r.v.size()*sizeof(std::string)+r.voff.size()*sizeof(off_t);
        for(const auto&x:r.v)s.logical_bytes+=x.size();
      }
    }
  }
  static int sync(int fd){Timer t("fsync_other");return ::fsync(fd);}
  static std::map<int,std::string> paths;
  static const char* io_scope="flush";
  struct IoScope {const char* prev;explicit IoScope(const char*s):prev(io_scope){io_scope=s;}~IoScope(){io_scope=prev;}};
  static bool suffix(const std::string&s,const std::string&t){return s.size()>=t.size()&&s.compare(s.size()-t.size(),t.size(),t)==0;}
  static const char* io_name(int fd){
    auto it=paths.find(fd);if(it==paths.end()){char b[4096];std::string link="/proc/self/fd/"+std::to_string(fd);ssize_t n=readlink(link.c_str(),b,sizeof(b));it=paths.emplace(fd,n<0?"?":std::string(b,n)).first;}
    const auto&p=it->second;
    if(suffix(p,".wal"))return "io_wal";
    if(suffix(p,".manifest"))return "io_manifest";
    if(suffix(p,".f32")||suffix(p,".vectors"))return std::string(io_scope)=="gc"?"io_gc_vector":"io_vector";
    if(suffix(p,".txn"))return std::string(io_scope)=="txn_merge"?"io_txn_merge":"io_txn_spill";
    if(suffix(p,".sst"))return std::string(io_scope)=="compact"?"io_sst_compact":std::string(io_scope)=="gc"?"io_sst_gc":"io_sst_flush";
    return "io_other";
  }
  static ssize_t write(int fd,const void*p,size_t n){const char*name=io_name(fd);Timer t(name);ssize_t z=::write(fd,p,n);if(z>0)stats[name].logical_bytes+=(uint64_t)z;return z;}
  static ssize_t pwrite(int fd,const void*p,size_t n,off_t off){const char*name=io_name(fd);Timer t(name);ssize_t z=::pwrite(fd,p,n,off);if(z>0)stats[name].logical_bytes+=(uint64_t)z;return z;}
  static int close(int fd){paths.erase(fd);return ::close(fd);}
  struct Request {
    Clock::time_point start;
    Request(){stats.clear();start=Clock::now();}
    ~Request(){
      double ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
      std::cerr<<std::setprecision(12)<<"{\"diagnostic\":true,\"request_ms\":"<<ms<<",\"stats\":{";
      bool first=true;for(const auto&kv:stats){if(!first)std::cerr<<',';first=false;const auto&s=kv.second;
        std::cerr<<'"'<<kv.first<<"\":{\"count\":"<<s.count<<",\"ms\":"<<s.ms
                 <<",\"rows\":"<<s.rows<<",\"logical_bytes\":"<<s.logical_bytes<<'}';}
      std::cerr<<"}}\n"<<std::flush;
    }
  };
}
#define fsync diagnostic::sync
#define DIAG_EXPR(name,expr) ([&](){diagnostic::Timer timer(name);return (expr);}())
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    src = a.source.read_text()
    original_hash = hashlib.sha256(a.source.read_bytes()).hexdigest()
    edits = [
        ('namespace lite {', HEADER+'\nnamespace lite {'),
        ('      lite::J req;', '      diagnostic::Request request_timer;\n      lite::J req;'),
        ('req=lite::parse(line);', 'req=DIAG_EXPR("json_parse",lite::parse(line));'),
        ('auto t=tok(sql);', 'auto t=DIAG_EXPR("sql_lex",tok(sql));'),
        ('lite::dump(out)', 'DIAG_EXPR("json_encode",lite::dump(out))'),
        ('if(snapshot)save=tabs;', 'if(snapshot){diagnostic::copy_size("begin_copy",tabs);diagnostic::Timer t("begin_copy");save=tabs;}'),
        ('if(!readonly&&!lean_insert)before=tabs;', 'if(!readonly&&!lean_insert){diagnostic::copy_size("statement_copy",tabs);diagnostic::Timer t("statement_copy");before=tabs;}'),
        ('    void sync_commit(int fd){', '    void sync_commit(int fd){\n      diagnostic::Timer t(fd==vfd?"vector_sync":"wal_sync");'),
        ('    void write_all(int fd,const char*p,size_t n){', '    void write_all(int fd,const char*p,size_t n){\n      const char*name=fd==vfd?"vector_write":"wal_write";diagnostic::Timer t(name);diagnostic::stats[name].logical_bytes+=n;'),
        ('      std::string f;\n      frame(f,"",tx,1);', '      std::string f;\n      {diagnostic::Timer t("wal_encode");\n      frame(f,"",tx,1);'),
        ('      frame(f,"",tx,3);', '      frame(f,"",tx,3);}\n'),
        ('    J insert(const std::vector<std::string>&t,bool rec){', '    J insert(const std::vector<std::string>&t,bool rec){\n      diagnostic::Timer timing("insert_inclusive");'),
        ('    static void parse_into(const std::string&s,int want,std::vector<float>&v){', '    static void parse_into(const std::string&s,int want,std::vector<float>&v){\n      diagnostic::Timer timing("vector_parse");'),
    ]
    header = a.source.with_name('lsm_store.hpp')
    if '#include "lsm_store.hpp"' in src:
        # LSM write sets replace the old copies. Instrument an independent copy
        # of both runtime sources; no clocks are added to the submission binary.
        edits = edits[:5] + [
            ('    void sync_commit(int fd){', '    void sync_commit(int fd){diagnostic::Timer timing("vector_sync");'),
            ('    void write_all(int fd,const char*p,size_t n){', '    void write_all(int fd,const char*p,size_t n){diagnostic::Timer timing("vector_write");diagnostic::stats["vector_write"].logical_bytes+=n;'),
            ('    J insert(const std::vector<std::string>&t,bool){', '    J insert(const std::vector<std::string>&t,bool){diagnostic::Timer timing("insert_inclusive");'),
            ('    static void parse_into(const std::string&s,int want,std::vector<float>&v){', '    static void parse_into(const std::string&s,int want,std::vector<float>&v){diagnostic::Timer timing("vector_parse");'),
            ('    void begin(bool=true){', '    void begin(bool=true){diagnostic::Timer timing("begin_write_set");'),
            ('try{tx->absorb(*statement);}catch', 'try{diagnostic::Timer timing("statement_merge");tx->absorb(*statement);}catch'),
        ]
        edits[0]=(edits[0][0],edits[0][1].replace('stats.clear();start=', 'stats.clear();stats["begin_copy"];stats["statement_copy"];start='))
        h=header.read_text()
        header_edits=[
            ('sync(wfd,true);', 'DIAG_EXPR("wal_sync",sync(wfd,true));'),
            ('  void frame(unsigned char kind,const std::string& data){', '  void frame(unsigned char kind,const std::string& data){diagnostic::Timer timing("wal_encode_write");diagnostic::stats["wal_encode_write"].logical_bytes+=data.size()+9;'),
            ('  void flush(){if(mem.empty())return;', '  void flush(){if(mem.empty())return;diagnostic::Timer timing("memtable_flush_inclusive");'),
            ('  void compact(bool full=false){', '  void compact(bool full=false){diagnostic::Timer timing("compaction_inclusive");'),
            ('  void commit(Batch& batch,uint64_t vend){', '  void commit(Batch& batch,uint64_t vend){diagnostic::Timer timing("lsm_commit_inclusive");'),
        ]
        if '  void sync_vectors(){' in h:
            header_edits += [('sync(vfd,true);', 'DIAG_EXPR("vector_sync",sync(vfd,true));')]
        header_edits += [
            ('  void flush(){if(mem.empty())return;', '  void flush(){if(mem.empty())return;diagnostic::IoScope scope("flush");'),
            ('  void compact(bool full=false){', '  void compact(bool full=false){diagnostic::IoScope scope("compact");'),
            ('  void spill(){if(mem.empty())return;', '  void spill(){if(mem.empty())return;diagnostic::IoScope scope("txn_spill");'),
            ('Writer z(out);', 'diagnostic::IoScope merge_scope("txn_merge");Writer z(out);'),
        ]
        edits += [('    void gc(){', '    void gc(){diagnostic::IoScope scope("gc");')]
        for before,after in header_edits:
            assert h.count(before)==1,('LSM instrumentation anchor changed',before,h.count(before))
            h=h.replace(before,after,1)
        h=h.replace('::write(fd,p,n)', 'diagnostic::write(fd,p,n)')
        h=h.replace('::pwrite(fd,p,n,(off_t)off)', 'diagnostic::pwrite(fd,p,n,(off_t)off)')
        h=h.replace('::close(n)', 'diagnostic::close(n)').replace('close(wfd)', 'diagnostic::close(wfd)')
        src=src.replace('close(vfd)', 'diagnostic::close(vfd)')
        a.out.parent.mkdir(parents=True,exist_ok=True)
        a.out.with_name('lsm_store.hpp').write_text(h)
    for old, new in edits:
        assert src.count(old) == 1, ('instrumentation anchor changed', old, src.count(old))
        src = src.replace(old, new, 1)
    a.out.write_text(src)
    a.out.with_suffix('.json').write_text(json.dumps({'production_source_sha256': original_hash,
        'production_lsm_sha256': hashlib.sha256(header.read_bytes()).hexdigest() if header.exists() else None,
        'diagnostic_source_sha256': hashlib.sha256(a.out.read_bytes()).hexdigest(),
        'caveats': ['Diagnostic timings include instrumentation overhead.',
                    'io_* bytes count successful write/pwrite return values; they are application bytes, not device I/O.',
                    'io_* timing includes wrappers and nested logical counters; do not sum inclusive stages.',
                    'insert_inclusive includes vector_parse and any buffered writes; do not sum nested stages.',
                    'copy logical_bytes excludes map-node allocation, spare capacity and allocator overhead.']}, indent=2)+'\n')


if __name__ == '__main__':
    main()
