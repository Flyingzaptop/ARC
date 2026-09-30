#include "arc/perceptual_trial.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
arc::ProbeCapture load(const std::string& prefix){
 std::ifstream bytes(prefix+".rgba8",std::ios::binary);std::vector<unsigned char> data{std::istreambuf_iterator<char>(bytes),{}};if(data.size()!=320*180*4)throw std::runtime_error("image size");
 arc::ProbeCapture c;c.width=320;c.height=180;c.state_key=1;c.generation=1;c.readback_complete=true;c.linear_rgb=true;c.rgb.reserve(320*180*3);for(size_t i=0;i<data.size();i+=4)for(size_t k=0;k<3;++k)c.rgb.push_back(float(data[i+k])/255.f);
 std::ifstream csv(prefix+".csv");std::string line;std::getline(csv,line);while(std::getline(csv,line)){std::istringstream row(line);std::vector<std::string> v;std::string part;while(std::getline(row,part,','))v.push_back(part);if(v.size()==7&&v[1]=="0")c.gpu_ms.push_back(std::stod(v[5]));}return c;
}
int main(int argc,char**argv){try{if(argc!=4)return 2;auto a=load(argv[1]),b=load(argv[2]),c=load(argv[3]);auto verdict=arc::PerceptualCritic{}.evaluate(a,b,c);std::cout<<"{\"critic_reason\":"<<int(verdict.reason)<<",\"accepted\":"<<(verdict.accepted()?"true":"false")<<",\"reference_mean\":"<<verdict.reference_mean<<",\"modified_mean\":"<<verdict.modified_mean<<",\"modified_tile\":"<<verdict.modified_tile<<",\"gpu_gain_ms\":"<<verdict.gain_ms<<",\"rollback_exact\":"<<(a.rgb==c.rgb?"true":"false")<<",\"modified_exact\":"<<(a.rgb==b.rgb?"true":"false")<<"}\n";return (a.rgb==b.rgb&&a.rgb==c.rgb)?0:1;}catch(const std::exception&e){std::cerr<<e.what();return 2;}}
