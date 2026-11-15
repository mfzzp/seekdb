// Independent deterministic mixed-dimension input generator, not engine code.
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>
int main(int argc,char**argv)try{
  if(argc!=3)throw std::runtime_error("ROWS OUT_DIR");
  size_t total=std::stoull(argv[1]),n=total/2;
  if(total%200||!total)throw std::runtime_error("rows must be divisible by 200");
  std::filesystem::create_directories(argv[2]);std::string root=argv[2];
  std::ofstream requests(root+"/insert.jsonl",std::ios::binary);
  std::ofstream raw[2];raw[0].open(root+"/vectors128.f32",std::ios::binary);raw[1].open(root+"/vectors1024.f32",std::ios::binary);
  std::mt19937 gen[2]={std::mt19937(20261115),std::mt19937(20261243)};
  std::normal_distribution<float> norm[2]={std::normal_distribution<float>(0,.5f),std::normal_distribution<float>(0,.5f)};
  size_t ident=1000;
  for(size_t begin=0;begin<n;begin+=100)for(int table=0;table<2;++table){
    int d=table?1024:128;std::string dim=std::to_string(d),sql="INSERT INTO docs"+dim+" VALUES ";sql.reserve(d*1200);
    std::vector<float>v(d);
    for(size_t id=begin+1;id<=begin+100;++id){
      if(id!=begin+1)sql+=',';
      sql+='('+std::to_string(id)+",'cat"+std::to_string(id%20)+"','doc"+dim+"-"+std::to_string(id)+"','[";
      for(int j=0;j<d;++j){v[j]=norm[table](gen[table]);char b[64];auto r=std::to_chars(b,b+sizeof(b),v[j],std::chars_format::general,9);if(r.ec!=std::errc())throw std::runtime_error("format float");if(j)sql+=',';sql.append(b,r.ptr);}
      sql+="]')";raw[table].write(reinterpret_cast<char*>(v.data()),d*4);
    }
    requests<<"{\"id\":"<<ident++<<",\"op\":\"execute\",\"sql\":\""<<sql<<";\",\"search_mode\":\"exact\"}\n";
    if(ident%200==0)std::cerr<<"prepared rows "<<(ident-1000)*100<<'\n';
  }
  if(!requests||!raw[0]||!raw[1])throw std::runtime_error("write input");
  std::cout<<"prepared "<<total<<" rows, 50% 128D and 50% 1024D\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
