// Compile only against geo_experiment.py's isolated generated source tree.
#define main lite_database_main
#include "../src/minivectordb_lite.cpp"
#undef main
#include <random>
static double scalar(const std::vector<float>&a,const std::vector<float>&b){double s=0;for(size_t i=0;i<a.size();++i){double z=double(a[i])-double(b[i]);s+=z*z;}return s;}
int main(){
  std::mt19937 gen(442221);size_t checks=0,positive=0;
  for(int d:{1,3,7,16,31,32,33,64,65,127,128,129,256,1024,4096})for(int trial=0;trial<100;++trial){
    std::vector<float>x(d),q(d),p(d);
    auto value=[&](){return std::ldexp(float(int(gen()%2001)-1000),trial%4?int(gen()%250)-149:-4);};
    for(int j=0;j<d;++j){x[j]=value();q[j]=trial%5==0?std::nextafter(x[j],0.0f):value();p[j]=trial%3?value():0;}
    if(trial%11==0)q=p;if(trial%13==0)x=p;if(trial%17==0)x=q;
    auto a=lite::norm_interval(x.data(),p.data(),d),b=lite::norm_interval(q.data(),p.data(),d);
    for(int side=0;side<2;++side){const auto&v=side?q:x;auto r=side?b:a;long double s=0;
      for(int j=0;j<d;++j){long double z=(long double)v[j]-(long double)p[j];s+=z*z;}
      long double radius=std::sqrt(s);if(!(r.lo<=radius&&radius<=r.hi))throw std::runtime_error("radius interval");++checks;
    }
    double bound=lite::radial_bound(a,b,d);if(bound>0)++positive;
    for(int isa:{0,2,512}){
      if(isa==2&&!lite::has_avx2())continue;if(isa==512&&!lite::has_avx512())continue;
      double full=isa==2?lite::l2_avx2(x.data(),q.data(),d):isa==512?lite::l2_avx512(x.data(),q.data(),d):scalar(x,q);
      if(!(bound<=full))throw std::runtime_error("unsafe rounded distance bound");
      for(double tau:{full,std::nextafter(full,0.0),std::nextafter(full,INFINITY)})if(bound>tau&&!(full>tau))throw std::runtime_error("unsafe tie prune");
      ++checks;
    }
  }
  if(!positive)throw std::runtime_error("bounds never positive");
  std::cout<<"PASS geo interval/bound checks="<<checks<<" positive="<<positive<<"\n";
}
