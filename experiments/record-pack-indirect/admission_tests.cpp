#include "admission.hpp"
#include <cassert>
#include <iostream>
using namespace arc_indirect_contract;
int main(){
 // A completed compute packet does not retire its later draw consumers.
 assert(!reusable(17,17));assert(reusable(17,18));assert(reusable(~0ull,0));
 Extra side[2]={{0,2,0,256|3,10},{0,2,0,256|3,10}};
 uint32_t records[8]={7,0,0x10000,255,7,1,0x30000,255};Group g{};
 assert(admit(records,2,side,2,10,g));assert(g.mesh==7&&g.lod==2&&g.stencil==3);
 side[1].generation=9;assert(!admit(records,2,side,2,10,g));side[1].generation=10;
 side[1].flags=3;assert(!admit(records,2,side,2,10,g));side[1].flags=256|4;assert(!admit(records,2,side,2,10,g));side[1].flags=256|3;
 side[1].lod=4;assert(!admit(records,2,side,2,10,g));records[7]=2;assert(admit(records,2,side,2,10,g));records[7]=255;side[1].lod=2;
 records[4]=8;assert(!admit(records,2,side,2,10,g));records[4]=7;
 records[5]=2;assert(!admit(records,2,side,2,10,g));records[5]=1;
 records[6]=0x17c00;assert(!admit(records,2,side,2,10,g));records[6]=0;assert(admit(records,2,side,2,10,g));
 assert(!admit(records,0,side,2,10,g));assert(!admit(records,65537,side,2,10,g));assert(!admit(records,2,side,0,10,g));
 std::cout<<"admission and consumer-retirement checks passed\n";
}
