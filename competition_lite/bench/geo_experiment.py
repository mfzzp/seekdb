#!/usr/bin/env python3
"""Generate an isolated geometric-block runtime; never edits production sources.

The generated path is opt-in (LITE_EXACT_PATH=geo). Keep the patch reproducible
without putting unproven geometry/preparation into the submitted executable.
"""
import argparse, hashlib, json, shutil
from pathlib import Path

ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--source',type=Path,required=True)
ap.add_argument('--out',type=Path,required=True)
a=ap.parse_args();a.out.mkdir(parents=True,exist_ok=False)
source=a.source.read_text(encoding='utf-8');s=source
def replace(old,new):
 global s
 assert s.count(old)==1,(old[:100],s.count(old))
 s=s.replace(old,new)

replace('  struct Matrix{',r'''  // Intervals enclose the real Euclidean norm of finite float inputs.
  struct Radius{double lo=0,hi=0;};
  static Radius norm_interval(const float*a,const float*b,int d){
    Radius r;
    for(int i=0;i<d;++i){
      double z=double(a[i])-double(b[i]);
      double lo=std::nextafter(z,-INFINITY),hi=std::nextafter(z,INFINITY);
      double mn=lo<=0&&hi>=0?0:std::min(std::abs(lo),std::abs(hi));
      double mx=std::max(std::abs(lo),std::abs(hi));
      double l=std::nextafter(mn*mn,0.0),h=std::nextafter(mx*mx,INFINITY);
      r.lo=std::nextafter(r.lo+l,0.0);r.hi=std::nextafter(r.hi+h,INFINITY);
    }
    return {std::nextafter(std::sqrt(r.lo),0.0),std::nextafter(std::sqrt(r.hi),INFINITY)};
  }
  static double radial_bound(Radius block,Radius query,int d){
    double delta=std::max({0.0,std::nextafter(block.lo-query.hi,-INFINITY),std::nextafter(query.lo-block.hi,-INFINITY)});
    double sq=std::nextafter(delta*delta,0.0);
    // Compare against the rounded SIMD/scalar squared sum, not the real norm.
    double margin=std::min(1.0,8.0*(double(d)+1)*std::numeric_limits<double>::epsilon());
    return std::max(0.0,std::nextafter(sq*(1-margin),0.0));
  }
  struct Geo{
    int col=-1;std::vector<float>pivot;std::vector<size_t>order,rank;std::vector<Radius>blocks;
  };
  struct Matrix{''')
replace('    std::map<std::string,int>vblock_col;','    std::map<std::string,int>vblock_col;\n    std::map<std::string,Geo>geos;')
replace('vblocks.clear();vblock_col.clear();','vblocks.clear();vblock_col.clear();geos.clear();')
# All view changes invalidate both physical and geometry caches. This includes
# reload/rollback/GC through clear_caches, and per-table hot/index rebuilding.
s=s.replace('vblocks.erase(q.n);','vblocks.erase(q.n);geos.erase(q.n);')
replace('    void build_vblocks(Table&q,int bc){',r'''    Geo& build_geo(Table&q,int col){
      auto&g=geos[q.n];if(g.col==col&&!g.order.empty())return g;
      g=Geo{};g.col=col;int d=q.c[col].d;
      for(auto*r:q.fast)if(r){g.pivot=read_vec(r->voff[col],d);break;}
      struct Item{size_t pos;Radius radius;};std::vector<Item>items;items.reserve(q.fast.size());
      for(size_t p=0;p<q.fast.size();++p)if(q.fast[p]){
        auto v=read_vec(q.fast[p]->voff[col],d);items.push_back({p,norm_interval(v.data(),g.pivot.data(),d)});
      }
      std::sort(items.begin(),items.end(),[&](const Item&a,const Item&b){return a.radius.lo!=b.radius.lo?a.radius.lo<b.radius.lo:q.fast[a.pos]->id<q.fast[b.pos]->id;});
      g.rank.resize(q.fast.size());g.order.reserve(items.size());
      for(size_t i=0;i<items.size();++i){
        if(i%64==0)g.blocks.push_back({INFINITY,0});auto&r=g.blocks.back();
        r.lo=std::min(r.lo,items[i].radius.lo);r.hi=std::max(r.hi,items[i].radius.hi);
        g.order.push_back(items[i].pos);g.rank[items[i].pos]=i;
      }
      return g;
    }
    void build_vblocks(Table&q,int bc){''')
replace('exact_path!="scan"&&exact_path!="block"','exact_path!="scan"&&exact_path!="block"&&exact_path!="geo"')
replace('LITE_EXACT_PATH must be auto, scan or block','LITE_EXACT_PATH must be auto, scan, block or geo')
replace('      std::vector<size_t>positions;',r'''      bool geo_exact=exact_topk&&exact_path=="geo"&&ask&&scan->size()>=(size_t)envint("LITE_GEO_MIN",2048)&&q.fast.size()*48<=(size_t)envint("LITE_GEO_MB",16)*1024*1024;
      block_exact=block_exact||geo_exact;
      std::vector<size_t>positions;''')
replace('        if(block_exact&&ask&&!positions.empty()){',r'''        if(geo_exact&&ask&&!positions.empty()){
          auto&g=build_geo(q,mcol);block_build_ms=elapsed(phase);phase=stamp();
          if(cs.empty())positions=g.order;
          else if(positions.size()>g.order.size()/4){
            std::vector<unsigned char>selected(q.fast.size());for(auto p:positions)selected[p]=1;
            positions.clear();for(auto p:g.order)if(selected[p])positions.push_back(p);
          }else std::sort(positions.begin(),positions.end(),[&](size_t x,size_t y){return g.rank[x]<g.rank[y];});
          block_sort_ms=elapsed(phase);phase=stamp();
          auto qr=norm_interval(qv.data(),g.pivot.data(),dim);
          struct Visit{double bound;size_t begin,end;};std::vector<Visit>visits;visits.reserve(g.blocks.size());
          for(size_t begin=0;begin<positions.size();){size_t bi=g.rank[positions[begin]]/64,end=begin+1;
            while(end<positions.size()&&g.rank[positions[end]]/64==bi)++end;
            visits.push_back({radial_bound(g.blocks[bi],qr,dim),begin,end});begin=end;++blocks_considered;
          }
          bounds_ms=elapsed(phase);phase=stamp();
          std::stable_sort(visits.begin(),visits.end(),[](const Visit&a,const Visit&b){return a.bound<b.bound;});
          block_sort_ms+=elapsed(phase);phase=stamp();
          for(const auto&v:visits){if(top.size()>=ask&&v.bound>top.top().d){++blocks_pruned;block_pruned_candidates+=v.end-v.begin;continue;}score(v.begin,v.end);}
        }else if(block_exact&&ask&&!positions.empty()){''')
replace('      prepare_ms=elapsed(phase);phase=stamp();','      if(geo_exact&&filtered_candidates<(size_t)envint("LITE_GEO_MIN",2048)){geo_exact=false;block_exact=false;}\n      prepare_ms=elapsed(phase);phase=stamp();')
replace('block_exact?"block":"scan"','geo_exact?"geo":block_exact?"block":"scan"')
(a.out/a.source.name).write_bytes(s.encode('utf-8'))
shutil.copyfile(a.source.with_name('lsm_store.hpp'),a.out/'lsm_store.hpp')
(a.out/'geo-input.json').write_bytes((json.dumps({'base_sha256':hashlib.sha256(source.encode()).hexdigest(),'generated_sha256':hashlib.sha256(s.encode()).hexdigest(),'production_default_changed':False},indent=2)+'\n').encode())
print((a.out/'geo-input.json').read_text())
