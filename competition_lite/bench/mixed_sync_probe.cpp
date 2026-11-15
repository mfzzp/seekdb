// Diagnostic-only link wrapper. Counts direct fsync/fdatasync calls by phase.
// Shared counters are read by the host driver after a response; no per-vector clock.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <cerrno>
extern "C" int __real_fsync(int);
extern "C" int __real_fdatasync(int);
static uint64_t* counters(){
  static uint64_t*p=[](){const char*path=std::getenv("LITE_SYNC_PROBE");if(!path)return (uint64_t*)nullptr;int fd=::open(path,O_CREAT|O_RDWR,0600);if(fd<0)return (uint64_t*)nullptr;
    if(ftruncate(fd,4096)){close(fd);return (uint64_t*)nullptr;}void*m=mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);close(fd);return m==MAP_FAILED?nullptr:(uint64_t*)m;}();return p;
}
static uint64_t nanos(){timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return uint64_t(t.tv_sec)*1000000000+t.tv_nsec;}
static int group(int fd){char name[64],path[1024];std::snprintf(name,sizeof(name),"/proc/self/fd/%d",fd);ssize_t n=readlink(name,path,sizeof(path)-1);if(n<0)return 5;path[n]=0;
  if(strstr(path,"vector")||strstr(path,".vec"))return 0;
  if(strstr(path,"wal")||strstr(path,"WAL")||strstr(path,"journal"))return 1;
  if(strstr(path,".sst")||strstr(path,".SST"))return 2;
  if(strstr(path,"MANIFEST")||strstr(path,"manifest")||strstr(path,"CURRENT"))return 3;
  struct stat st;if(!fstat(fd,&st)&&S_ISDIR(st.st_mode))return 4;return 5;
}
static int measure(int fd,bool data){uint64_t*p=counters();if(!p)return data?__real_fdatasync(fd):__real_fsync(fd);int g=group(fd);uint64_t phase=__atomic_load_n(p,__ATOMIC_RELAXED);if(phase>9)phase=9;
  uint64_t start=nanos();int result=data?__real_fdatasync(fd):__real_fsync(fd),err=errno;uint64_t elapsed=nanos()-start;
  size_t index=8+((phase*6+g)*2+int(data))*3;__atomic_fetch_add(p+index,1,__ATOMIC_RELAXED);__atomic_fetch_add(p+index+1,elapsed,__ATOMIC_RELAXED);__atomic_fetch_add(p+index+2,result!=0,__ATOMIC_RELAXED);errno=err;return result;
}
extern "C" int __wrap_fsync(int fd){return measure(fd,false);}
extern "C" int __wrap_fdatasync(int fd){return measure(fd,true);}
