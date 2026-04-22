#include <fstream>
#include <iostream>
int main(){
  std::fstream fs(drive_1.txt, std::ios::binary|std::ios::in|std::ios::out);
  if(!fs.is_open()){ std::cout<<open
