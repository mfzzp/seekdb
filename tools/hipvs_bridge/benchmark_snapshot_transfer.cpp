// Diagnostic paired snapshot transfer benchmark. Not SQL/contest performance.
#include "lib/vector/cuvs_snapshot_codec.h"
#include "lib/vector/cuvs_snapshot_io.h"
#include <chrono>
#include <fstream>
#include <string>
#include <stdexcept>
#include <memory>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
using namespace oceanbase::common::obvsag;
using Clock = std::chrono::steady_clock;
using File = std::unique_ptr<FILE, int(*)(FILE*)>;
static void check(bool ok) { if (!ok) throw std::runtime_error("I/O or byte verification failed"); }
static double elapsed(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now()-start).count(); }
static void equal(FILE* a, FILE* b, uint64_t bytes) {
  rewind(a); rewind(b); char x[65536], y[65536];
  while(bytes) { const size_t n=std::min<uint64_t>(bytes,sizeof(x)); check(fread(x,1,n,a)==n && fread(y,1,n,b)==n && memcmp(x,y,n)==0); bytes-=n; }
  check(fgetc(b)==EOF);
}
int main(int argc,char**argv) {
  if(argc!=3) return 2;
  try {
    File input(fopen(argv[1],"rb"),fclose); check(bool(input));
    struct stat st{}; check(fstat(fileno(input.get()),&st)==0 && st.st_size>0);
    const uint64_t bytes=st.st_size;
    for(int sample=-1;sample<5;++sample) {
      for(int order=0;order<2;++order) {
        const bool compressed=((sample+order+2)%2)==0;
        const std::string path=std::string(argv[2])+(compressed?".v4":".v3");
        CuvsSnapshotCodec::Stats stats; bool mapped=false;
        auto start=Clock::now();
        {
          std::ofstream out(path,std::ios::binary|std::ios::trunc); check(out.good());
          check(compressed?CuvsSnapshotCodec::encode(input.get(),bytes,out,stats):cuvs_copy_snapshot(input.get(),bytes,out,mapped,false));
          out.close(); check(out.good());
        }
        const double save_ms=elapsed(start);
        struct stat stored{}; check(stat(path.c_str(),&stored)==0);
        File restored(tmpfile(),fclose); check(bool(restored));
        start=Clock::now();
        {
          std::ifstream in(path,std::ios::binary); check(in.good());
          if(compressed) check(CuvsSnapshotCodec::decode(in,bytes,restored.get(),stats));
          else { std::vector<char> chunk(1024*1024); uint64_t left=bytes; while(left) { const size_t n=std::min<uint64_t>(left,chunk.size()); check(cuvs_snapshot_read(in,chunk.data(),n)); check(fwrite(chunk.data(),1,n,restored.get())==n); left-=n; } check(fflush(restored.get())==0); }
        }
        const double read_ms=elapsed(start);
        equal(input.get(),restored.get(),bytes); check(unlink(path.c_str())==0);
        if(sample>=0) printf("sample=%d format=%s raw_bytes=%llu stored_bytes=%lld save_ms=%.3f restore_ms=%.3f verified=1\n",sample,compressed?"v4":"v3",(unsigned long long)bytes,(long long)stored.st_size,save_ms,read_ms);
        fflush(stdout);
      }
    }
  } catch(const std::exception&e) { fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
