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
    for old, new in edits:
        assert src.count(old) == 1, ('instrumentation anchor changed', old, src.count(old))
        src = src.replace(old, new, 1)
    a.out.write_text(src)
    a.out.with_suffix('.json').write_text(json.dumps({'production_source_sha256': original_hash,
        'diagnostic_source_sha256': hashlib.sha256(a.out.read_bytes()).hexdigest(),
        'caveats': ['Diagnostic timings include instrumentation overhead.',
                    'insert_inclusive includes vector_parse and any buffered writes; do not sum nested stages.',
                    'copy logical_bytes excludes map-node allocation, spare capacity and allocator overhead.']}, indent=2)+'\n')


if __name__ == '__main__':
    main()
