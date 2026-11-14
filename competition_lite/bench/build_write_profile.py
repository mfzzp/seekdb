#!/usr/bin/env python3
"""Generate a diagnostic-only source with phase clocks, leaving runtime untouched.

Supports edde4ac and this write iteration. Compile generated source with the same
release flags, then use write_pairs.py for one diagnostic run. Never use these
instrumented timings as final performance numbers. Fails on changed anchors.
"""
import argparse
from pathlib import Path

ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--source',type=Path,required=True);ap.add_argument('--output',type=Path,required=True)
a=ap.parse_args();s=a.source.read_text(encoding='utf-8')
intro='''
namespace write_diag {
using Clock=std::chrono::steady_clock;
struct Stats{double ms[7]={};unsigned long long calls[7]={};
~Stats(){const char*names[]={"sql_parse","prepare","apply","wal","vector_write_map","sync","lsm"};
for(int i=0;i<7;++i)std::cerr<<"WRITE_PROFILE "<<names[i]<<" "<<ms[i]<<" "<<calls[i]<<"\\n";}};
static Stats stats;
static auto stamp(){return Clock::now();}
static void add(int i,Clock::time_point t){stats.ms[i]+=std::chrono::duration<double,std::milli>(Clock::now()-t).count();++stats.calls[i];}
}
'''
def replace(old,new):
    global s
    assert s.count(old)==1,(old,s.count(old))
    s=s.replace(old,new)
replace('namespace lite {',intro+'\nnamespace lite {')
replace('      auto t=tok(sql);','      auto diag_t=write_diag::stamp();auto t=tok(sql);write_diag::add(0,diag_t);diag_t=write_diag::stamp();')
replace('        begin();','        begin();write_diag::add(1,diag_t);')
replace('        J r;','        write_diag::add(1,diag_t);diag_t=write_diag::stamp();\n        J r;')
replace('        if(auto_tx)commit();','        write_diag::add(2,diag_t);\n        if(auto_tx)commit();')
replace('      uint64_t tx=++wal_txid;','      auto diag_t=write_diag::stamp();\n      uint64_t tx=++wal_txid;')
if 'sync_commit(wfd);' in s:
    replace('      // Persist vector bytes', '      write_diag::add(3,diag_t);diag_t=write_diag::stamp();\n      // Persist vector bytes')
    replace('      flush_vecbuf();\n      sync_commit(vfd);','      flush_vecbuf();write_diag::add(4,diag_t);diag_t=write_diag::stamp();\n      sync_commit(vfd);write_diag::add(5,diag_t);diag_t=write_diag::stamp();')
    replace('      write_all(wfd,f.data(),f.size());','      write_all(wfd,f.data(),f.size());write_diag::add(3,diag_t);diag_t=write_diag::stamp();')
    replace('      sync_commit(wfd);','      sync_commit(wfd);write_diag::add(5,diag_t);diag_t=write_diag::stamp();')
    replace('      }catch(...){failed_io=true;throw;}','      }catch(...){failed_io=true;throw;}\n      write_diag::add(6,diag_t);')
else:
    replace('      f.flush();\n      flush_vecbuf();','      f.flush();write_diag::add(3,diag_t);diag_t=write_diag::stamp();\n      flush_vecbuf();write_diag::add(4,diag_t);diag_t=write_diag::stamp();')
    replace('      for(auto&r:pending_lsm){r.seq=tx;memtable.push_back(std::move(r));}','      write_diag::add(5,diag_t);diag_t=write_diag::stamp();\n      for(auto&r:pending_lsm){r.seq=tx;memtable.push_back(std::move(r));}')
    replace('      if(membytes>=(size_t)envint("LITE_MEMTABLE_BYTES",4<<20)||fsize(wal)>=(32u<<20))flush_sst();','      if(membytes>=(size_t)envint("LITE_MEMTABLE_BYTES",4<<20)||fsize(wal)>=(32u<<20))flush_sst();\n      write_diag::add(6,diag_t);')
a.output.write_text(s,encoding='utf-8')
