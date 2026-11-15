// Compare ISA kernels bitwise and with independent long-double distances.
#define main lite_database_main
#include "../src/minivectordb_lite.cpp"
#undef main
#include <random>
static void require(bool ok,const char*what,int dim){if(!ok)throw std::runtime_error(std::string(what)+" dim="+std::to_string(dim));}
int main(){
  std::mt19937 gen(711243);uint64_t checks=0,cuts=0;
  std::vector<int>dims={1,3,4,7,8,15,16,17,31,32,33,63,64,65,127,128,129,255,256,257,511,512,1023,1024,4096};
  for(int trial=0;trial<300;++trial)for(int dim:dims){
    std::vector<float>q(dim+32),storage[4];const float*ptrs[4];float*b=q.data()+trial%16;
    auto value=[&](){return std::ldexp(float(int(gen()%20001)-10000),trial%5==0?int(gen()%200)-113:-14);};
    for(int i=0;i<dim;++i)b[i]=value();
    for(int k=0;k<4;++k){storage[k].resize(dim+32);float*p=storage[k].data()+(trial+k)%16;ptrs[k]=p;
      for(int i=0;i<dim;++i)p[i]=trial%13==0?b[i]:value();}
    for(int isa:{2,512}){
      if(isa==2?!lite::has_avx2():!lite::has_avx512())continue;
      double pair[2],quad[4],scanned[4];std::vector<float>matrix(dim*4);
      for(int k=0;k<4;++k)std::copy(ptrs[k],ptrs[k]+dim,matrix.data()+k*dim);
      if(isa==2)lite::l2_avx2_scan(matrix.data(),b,dim,4,scanned);else lite::l2_avx512_scan(matrix.data(),b,dim,4,scanned);
      if(isa==512&&dim==128){double wide[4];lite::l2_avx512_query128(matrix.data(),b,dim,4,wide);
        require(std::memcmp(wide,scanned,sizeof(wide))==0,"cached query scan differs",dim);}
      if(isa==2){lite::l2_avx2_many<2>(ptrs,b,dim,pair);lite::l2_avx2_many<4>(ptrs,b,dim,quad);}
      else{lite::l2_avx512_many<2>(ptrs,b,dim,pair);lite::l2_avx512_many<4>(ptrs,b,dim,quad);}
      for(int k=0;k<4;++k){
        double full=isa==2?lite::l2_avx2(ptrs[k],b,dim):lite::l2_avx512(ptrs[k],b,dim);
        require(std::memcmp(&full,&quad[k],sizeof(double))==0,"quad differs from single",dim);
        require(std::memcmp(&full,&scanned[k],sizeof(double))==0,"matrix scan differs from single",dim);
        if(k<2)require(std::memcmp(&full,&pair[k],sizeof(double))==0,"pair differs from single",dim);
        long double truth=0;for(int i=0;i<dim;++i){long double x=(long double)ptrs[k][i]-(long double)b[i];truth+=x*x;}
        require(std::abs((long double)full-truth)<=4*(dim+1)*std::numeric_limits<double>::epsilon()*truth,"oracle distance",dim);
        for(double limit:{full,full*.25,full*.75,std::nextafter(full,0.0),std::nextafter(full,INFINITY)}){
          int used=0;double d=isa==2?lite::l2_avx2<true>(ptrs[k],b,dim,limit,&used):lite::l2_avx512<true>(ptrs[k],b,dim,limit,&used);
          require(used>0&&used<=dim,"dimension count",dim);
          if(used<dim){require(full>limit&&d>limit,"unsafe partial cutoff",dim);++cuts;}
          else require(std::memcmp(&full,&d,sizeof(double))==0,"early full result differs",dim);
          ++checks;
        }
      }
    }
  }
  require(cuts>0,"no early cutoff exercised",0);
  std::cout<<"PASS kernel comparisons="<<checks<<" partial cutoffs="<<cuts<<"\n";
}
