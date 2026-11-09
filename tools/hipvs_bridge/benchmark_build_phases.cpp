// Real bridge phase diagnostics over deterministic synthetic inputs.
// No SQL, official dataset, official Recall or contest score is measured here.
#include "seekdb_cuvs_bridge.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <memory>
#include <set>
#include <stdexcept>
#include <vector>
#include <sys/stat.h>
using Clock=std::chrono::steady_clock;
using Index=std::unique_ptr<void,decltype(&seekdb_cuvs_free)>;
static void check(bool ok,const char*message) { if(!ok) throw std::runtime_error(message); }
static double ms(Clock::time_point t) { return std::chrono::duration<double,std::milli>(Clock::now()-t).count(); }
int main(int argc,char**argv) {
  if(argc!=4) return 2;
  try {
    const long n=std::stol(argv[1]),dim=std::stol(argv[2]);
    check(n>=256 && n<=1000000 && dim>0 && dim<=1024,"invalid dimensions");
    std::vector<float> data(static_cast<size_t>(n)*dim); uint32_t state=0x521bc7d1;
    for(auto& v:data) { state^=state<<13; state^=state>>17; state^=state<<5; v=float(state>>8)/8388608.0f-1.0f; }
    auto t=Clock::now(); Index index(seekdb_cuvs_build(data.data(),n,dim),seekdb_cuvs_free);
    check(bool(index),"build failed"); const double build_ms=ms(t);
    t=Clock::now(); check(seekdb_cuvs_serialize(index.get(),argv[3])==0,"save failed"); const double serialize_ms=ms(t);
    struct stat st{}; check(stat(argv[3],&st)==0,"snapshot stat failed");
    index.reset();
    t=Clock::now(); index.reset(seekdb_cuvs_deserialize(argv[3],n,dim)); check(bool(index),"load failed"); const double reload_ms=ms(t);
    std::vector<float> restored(data.size()); t=Clock::now();
    check(seekdb_cuvs_export_vectors(index.get(),restored.data(),n,dim)==0,"export failed"); const double export_ms=ms(t);
    check(memcmp(data.data(),restored.data(),data.size()*sizeof(float))==0,"FP32 dataset changed");
    std::vector<unsigned> ids(100); std::vector<float> distances(100); t=Clock::now();
    check(seekdb_cuvs_search(index.get(),data.data(),1,100,ids.data(),distances.data())==0,"search failed"); const double first_query_ms=ms(t);
    std::set<unsigned> unique;
    for(size_t i=0;i<ids.size();++i) {
      check(ids[i]<static_cast<unsigned>(n) && unique.insert(ids[i]).second,"invalid neighbor");
      double exact=0; for(long d=0;d<dim;++d) { double delta=double(data[d])-data[ids[i]*dim+d]; exact+=delta*delta; }
      check(std::isfinite(distances[i]) && std::abs(distances[i]-exact)<=0.001*(1+exact),"incorrect distance");
    }
    check(ids[0]==0 && std::abs(distances[0])<=0.0001,"missing self neighbor");
    printf("fixture=synthetic_uniform n=%ld dim=%ld build_ms=%.3f serialize_ms=%.3f reload_ms=%.3f export_ms=%.3f first_query_ms=%.3f snapshot_bytes=%lld verified=1\n",n,dim,build_ms,serialize_ms,reload_ms,export_ms,first_query_ms,(long long)st.st_size);
  } catch(const std::exception&e) { fprintf(stderr,"FAIL %s\n",e.what()); return 1; }
}
