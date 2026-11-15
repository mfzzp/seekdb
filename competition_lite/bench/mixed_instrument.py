#!/usr/bin/env python3
"""Copy frozen source and insert coarse diagnostic boundaries, never edit it."""
import argparse,hashlib,json,shutil
from pathlib import Path
ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--source',type=Path,required=True);ap.add_argument('--out',type=Path,required=True);a=ap.parse_args();a.out.mkdir(parents=True,exist_ok=False)
s=a.source.read_text(encoding='utf-8');changes=[]
def replace(old,new):
 global s
 assert s.count(old)==1,(old,s.count(old));s=s.replace(old,new);changes.append(old)
def enter(anchor,index):replace(anchor,anchor+f'MixedStage mixed_stage({index});')
header='''#pragma once
#include <chrono>
#include <cstdint>
extern "C" void mixed_stage_record(unsigned,uint64_t);
extern "C" void mixed_counter(unsigned,uint64_t);
struct MixedStage { unsigned index;std::chrono::steady_clock::time_point start;
 explicit MixedStage(unsigned i):index(i),start(std::chrono::steady_clock::now()){}
 ~MixedStage(){mixed_stage_record(index,std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count());}};
#define MIXED_EXPR(i,e) ([&](){MixedStage mixed_stage(i);return (e);}())
'''
(a.out/'mixed_stages.hpp').write_text(header)
if 'namespace mv {' in s:
 replace('req = mv::json_parse(line);','req = MIXED_EXPR(0,mv::json_parse(line));')
 replace('auto tok = mv::lex_sql(sql);','auto tok = MIXED_EXPR(1,mv::lex_sql(sql));')
 enter('static std::vector<float> parse_vector(const std::string &x, int dim) {',2)
 enter('int64_t apply_insert_rows(Table &tab, std::vector<Row> pending) {',3)
 enter('Json select(std::vector<Token> &t, size_t &p, bool ann_mode = false) {',4)
 enter('static void ensure_vector_matrix(Table &t) {',5)
 enter('static void ensure_equality_index(Table &t, int col) {',6)
 enter('static void ensure_blocks(Table &t) {',7)
 replace('tx = st;','{MixedStage copy_stage(8);tx = st;}')
 enter('void append_journal(const std::vector<JournalEntry> &entries) {',9)
 enter('void checkpoint() {',10)
 replace('mv::json_dump(body)','MIXED_EXPR(11,mv::json_dump(body))')
 replace('    auto distance_for = [&](size_t r, size_t qi) {','    uint64_t mixed_distance_calls=0;\n    auto distance_for = [&](size_t r, size_t qi) {')
 replace('      if (std::isnan(distance_cache[qi][r])) {','      if (std::isnan(distance_cache[qi][r])) {++mixed_distance_calls;')
 if 'if (cached != cache.end()) return cached->second;' in s:
  replace('if (cached != cache.end()) return cached->second;','if (cached != cache.end()) return cached->second;++mixed_distance_calls;')
 replace('    return Json::object({{"columns", Json::array(std::move(cols))}, {"rows", Json::array(std::move(out))}});','    return Json::object({{"columns", Json::array(std::move(cols))}, {"rows", Json::array(std::move(out))}, {"metrics", Json::object({{"full_distance_evaluations",Json::integer(mixed_distance_calls)},{"distance_dimensions",Json::integer(mixed_distance_calls*(tab.vector_col>=0?tab.cols[tab.vector_col].width:0))}})}});')
 if (a.source.parent/'v2').exists():shutil.copytree(a.source.parent/'v2',a.out/'v2')
else:
 replace('req=lite::parse(line);','req=MIXED_EXPR(0,lite::parse(line));')
 replace('auto t=tok(sql);','auto t=MIXED_EXPR(1,tok(sql));')
 enter('static void parse_into(const std::string&s,int want,std::vector<float>&v){',2)
 enter('J insert(const std::vector<std::string>&t,bool){',3)
 enter('J select(const std::vector<std::string>&t){',4)
 enter('void ensure_matrix(Table&q,int c){',5)
 enter('void rebuild_scalar(Table&q){',6)
 enter('void build_vblocks(Table&q,int bc){',7)
 enter('void checkpoint(){',10)
 enter('bool ensure_hot(Table&q){',12)
 replace('bool row_get(const Table&q,long long id,Row&r)const{','bool row_get(const Table&q,long long id,Row&r)const{mixed_counter(0,1);')
 replace('Row decode_row(const Table&q,const std::string&s)const{','Row decode_row(const Table&q,const std::string&s)const{mixed_counter(1,1);')
 replace('write_all(vfd,vecbuf.data(),vecbuf.size());vecbuf.clear();','write_all(vfd,vecbuf.data(),vecbuf.size());mixed_counter(9,vecbuf.size());vecbuf.clear();')
 replace('lite::dump(out)','MIXED_EXPR(11,lite::dump(out))')
 h=a.source.with_name('lsm_store.hpp').read_text(encoding='utf-8')
 def hreplace(old,new,count=1):
  global h
  assert h.count(old)==count,(old,h.count(old));h=h.replace(old,new);changes.append('lsm:'+old)
 hreplace('  void commit(Batch& batch,uint64_t vend){','  void commit(Batch& batch,uint64_t vend){MixedStage mixed_stage(9);')
 hreplace('io(fd.n>=0,"open referenced SST");','mixed_counter(2,1);io(fd.n>=0,"open referenced SST");')
 hreplace('std::string b=read_at(fd.n,i.off,i.bytes);','std::string b=read_at(fd.n,i.off,i.bytes);mixed_counter(3,1);mixed_counter(4,i.bytes);')
 if 'struct TableReader:Cached' in h:
  hreplace('auto r=decode(d);++stats->decodes;','auto r=decode(d);mixed_counter(5,1);++stats->decodes;')
 else:
  hreplace('row=decode(d);p=d.p;','row=decode(d);mixed_counter(5,1);p=d.p;')
 hreplace('for(auto&f:out)bytes_out+=f->bytes;compact(false);','for(auto&f:out){bytes_out+=f->bytes;mixed_counter(6,f->bytes);}compact(false);')
 hreplace('for(auto&f:out)bytes_out+=f->bytes;','for(auto&f:out){bytes_out+=f->bytes;mixed_counter(7,f->bytes);}',2)
 hreplace('wal_bytes+=h.size()+b.size();','wal_bytes+=h.size()+b.size();mixed_counter(8,h.size()+b.size());')
 hreplace('write_all(f.n,body.data(),body.size());sync(f.n);','write_all(f.n,body.data(),body.size());mixed_counter(10,body.size());sync(f.n);')
 hreplace('bool get(const std::string& key,SRec& best)const{','bool get(const std::string& key,SRec& best)const{mixed_counter(11,1);')
 (a.out/'lsm_store.hpp').write_text(h,encoding='utf-8')
s='#include "mixed_stages.hpp"\n'+s;(a.out/a.source.name).write_text(s)
probe=Path(__file__).with_name('mixed_sync_probe.cpp').read_text().replace('4096','8192')
probe+='''
extern "C" void mixed_stage_record(unsigned kind,uint64_t ns){
 uint64_t*p=counters();if(!p||kind>=13)return;uint64_t phase=__atomic_load_n(p,__ATOMIC_RELAXED);if(phase>9)phase=9;
 size_t index=400+(phase*13+kind)*2;__atomic_fetch_add(p+index,1,__ATOMIC_RELAXED);__atomic_fetch_add(p+index+1,ns,__ATOMIC_RELAXED);
}
extern "C" void mixed_counter(unsigned kind,uint64_t amount){
 uint64_t*p=counters();if(!p||kind>=12)return;uint64_t phase=__atomic_load_n(p,__ATOMIC_RELAXED);if(phase>9)phase=9;
 __atomic_fetch_add(p+700+phase*12+kind,amount,__ATOMIC_RELAXED);
}
''';(a.out/'probe.cpp').write_text(probe)
(a.out/'instrumentation.json').write_text(json.dumps({'base_sha256':hashlib.sha256(a.source.read_bytes()).hexdigest(),'diagnostic_sha256':hashlib.sha256((a.out/a.source.name).read_bytes()).hexdigest(),'generated_files':{p.relative_to(a.out).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in a.out.rglob('*') if p.is_file()},'anchors':changes,'per_distance_clocks':False,'caveat':'Function timings are inclusive; do not sum nested stages. Separate diagnostic run only. LSM counters exist only for the new engine. Byte counters describe encoded logical bytes, not physical device I/O.'},indent=2)+'\n')
