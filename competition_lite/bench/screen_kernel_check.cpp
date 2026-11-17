#define main lite_runtime_main
#include "../src/minivectordb_lite.cpp"
#undef main
#include <random>
int main(){try{
  if(!lite::has_avx512()||!__builtin_cpu_supports("avx512bw")){std::cout<<"SKIP AVX512BW unavailable\n";return 0;}
  std::mt19937 rng(894721);std::vector<float>matrix(128*257+16),query(144);float*a=matrix.data()+3,*q=query.data()+5;
  std::vector<int8_t>codes(128*257+7);int8_t*data=codes.data()+7;int8_t qc[128];
  std::vector<size_t>positions(257);for(size_t i=0;i<positions.size();++i)positions[i]=positions.size()-1-i;
  size_t checked=0,cuts=0,clipped=0;int ds[257],selected[257];
  for(int trial=0;trial<512;++trial){
    int mode=trial%8,exponent=(trial*43)%270-145;
    auto value=[&](){return std::ldexp(float(int(rng()%20001)-10000)/10000.f,mode==0?0:exponent);};
    for(int j=0;j<128;++j)q[j]=mode==6?std::numeric_limits<float>::max()*(j%2?1:-1):value();
    double maximum=0;
    for(size_t i=0;i<257;++i)for(int j=0;j<128;++j){float v=value();
      if(mode==2)v=std::nextafter(q[j],i%2?INFINITY:-INFINITY);
      if(mode==3)v=q[j];if(mode==4)v=j<2?value():0;
      if(mode==5){uint32_t bits=rng();std::memcpy(&v,&bits,4);if(!std::isfinite(v))v=0;}
      if(mode==6)v=-q[j];if(mode==7)v=j%2?-q[j]:q[j];a[i*128+j]=v;maximum=std::max(maximum,std::abs(double(v)));
    }
    double scale=maximum?maximum/127:1;for(size_t i=0;i<257*128;++i)data[i]=(int8_t)std::llround(double(a[i])/scale);
    if(trial%11==0)for(int j=0;j<128;++j)q[j]=j%2?std::numeric_limits<float>::max():-std::numeric_limits<float>::max();
    double error=lite::screen_query128(q,scale,qc);for(int j=0;j<128;++j)clipped+=std::abs(double(q[j])/scale)>127;
    lite::l2_screen128(data,qc,nullptr,257,ds);lite::l2_screen128(data,qc,positions.data(),257,selected);
    if(__builtin_cpu_supports("avx512vnni")){lite::Matrix m;m.codes.assign(data,data+257*128);lite::screen_pack128(m);int dot[257];
      lite::l2_screen_dot128(m.packed.data(),m.norms.data(),qc,257,dot);if(std::memcmp(dot,ds,sizeof(ds)))throw std::runtime_error("dot differs");
      uint16_t masks[17];
      const int cutoff=ds[trial%257];
      for(size_t start:{size_t(0),size_t(16)}){
        const size_t count=257-start;
        lite::l2_screen_dot128(m.packed.data()+start*128,m.norms.data()+start,qc,count,nullptr,masks,cutoff);
        for(size_t i=0;i<count;i+=16){
          unsigned expected=0;
          for(size_t j=0;j<16&&i+j<count;++j)
            if(ds[start+i+j]<=cutoff)expected|=1u<<j;
          if(masks[i/16]!=expected)throw std::runtime_error("screen survivor mask differs");
        }
      }
      lite::l2_screen_dot128(m.packed.data()+16*128,m.norms.data()+16,qc,241,dot);if(std::memcmp(dot,ds+16,241*sizeof(int)))throw std::runtime_error("dot offset/tail differs");}
    for(size_t i=0;i<257;++i){
      if(selected[i]!=ds[positions[i]])throw std::runtime_error("position mismatch");
      int integer=0;for(int j=0;j<128;++j){int d=int(data[i*128+j])-qc[j];integer+=d*d;}
      if(ds[i]!=integer)throw std::runtime_error("integer arithmetic mismatch");
      double exact=lite::l2_avx512(a+i*128,q,128);
      long double truth=0;for(int j=0;j<128;++j){long double d=(long double)a[i*128+j]-q[j];truth+=d*d;}
      for(double limit:{exact,std::nextafter(exact,0.0),std::nextafter(exact,INFINITY),exact*.99,exact*.5,0.0,double(INFINITY)}){
        double threshold=lite::screen_threshold128(limit,scale,error);
        if(integer>threshold){if(!(exact>limit&&truth>limit))throw std::runtime_error("unsafe rejection trial="+std::to_string(trial));++cuts;}
        ++checked;
      }
    }
  }
  if(cuts<10000||clipped==0)throw std::runtime_error("coverage missing");
  std::cout<<"PASS thresholds="<<checked<<" safe rejections="<<cuts<<" clipped query coordinates="<<clipped<<"\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
