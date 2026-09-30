#include "arc/arc2/runtime.hpp"
#include <chrono>
#include <array>
#include <iostream>
using namespace arc::arc2;
int main(){Runtime r(4096,4096);auto q=r.create_object(ObjectKind::Queue,1);auto a=r.create_object(ObjectKind::Allocator,2);auto c=r.create_object(ObjectKind::CommandList,3);auto b=r.create_object(ObjectKind::Resource,4);auto f=r.create_object(ObjectKind::Fence,5);auto s=r.create_object(ObjectKind::Swapchain,6);r.describe_resource(b,64);double early=0,steady=0;constexpr unsigned n=12000;for(unsigned i=0;i<n;++i){auto start=std::chrono::steady_clock::now();r.reset_command_list(c,a);for(int k=0;k<4;++k)r.barrier(c,b,k,k+1);r.close_command_list(c);r.submit(q,std::array<ObjectId,1>{c});r.signal(q,f,i+1);r.present(s,q,b,0);double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();if(i<500)early+=ms;if(i>=8000)steady+=ms;}std::cout<<"{\"iterations\":"<<n<<",\"history_capacity\":4096,\"early_mean_ms\":"<<early/500<<",\"saturated_mean_ms\":"<<steady/4000<<"}\n";}
