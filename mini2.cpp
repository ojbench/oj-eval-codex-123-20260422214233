#include src.hpp
#include <cstdio>
#include <iostream>
#include <memory>
#include <vector>
#include <string>

void prepare(const std::string& fn, int blocks, int bs){
  FILE* f=fopen(fn.c_str(),wb); std::vector<char> z(bs,0); for(int i=0;i<blocks;i++) fwrite(z.data(),1,bs,f); fclose(f);
}

int main(){
  int num=4, bs=4, bpd=8;
  std::vector<std::string> names; std::vector<std::unique_ptr<sjtu::fstream>> files;
  for(int i=0;i<num;i++){ std::string n=d+std::to_string(i)+.bin; names.push_back(n); prepare(n,bpd,bs); files.emplace_back(new sjtu::fstream(n, std::ios::binary|std::ios::in|std::ios::out)); }
  std::vector<sjtu::fstream*> ptrs; for(auto& u:files) ptrs.push_back(u.get());
  RAID5Controller r(ptrs, bpd, bs);
  r.Start(EventType::NORMAL,0);
  std::string data=AAA0;
  r.WriteBlock(0, data.c_str());
  std::vector<char> buf(bs);
  r.ReadBlock(0, buf.data());
  std::cout << std::string(buf.data(), bs) << n;
  r.Shutdown();
  return 0;
}
