#pragma once
#include "generic_shader_transform.hpp"
#include <sstream>
namespace arc::dx12::shader {
// Separate small PSO: all pixels contribute to tile features, no application UAVs.
inline std::string compact_spatial_source(const Transform& contract){
    if(!contract.execution_marker||!contract.edge_input_mask)return {};
    std::ostringstream out;out<<"cbuffer Control:register(b0,space"<<contract.control_space<<"){uint4 control[9];}\n"
        <<"RWByteAddressBuffer map:register(u0,space"<<contract.control_space<<");\n";
    for(const auto& r:contract.resources)if(r.resource_class==0&&r.range_id<31&&(contract.edge_input_mask&(1u<<r.range_id)))
        out<<"Texture2D<float4> input"<<r.range_id<<":register(t"<<r.shader_register<<",space"<<r.space<<");\n";
    out<<R"(
groupshared float4 minimums[64],maximums[64],sums[64];
groupshared uint counts[64],bad[64];
[numthreads(64,1,1)]void MainCS(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    uint width=control[0].z,height=control[0].w,capacity=control[5].x,frame=control[5].y;
    uint2 key=control[5].zw,tile=control[6].xy;uint flags=control[6].z,mask=control[1].z;
    if(!(flags&16)||!(mask&0x80000000)||capacity!=8192||!frame||!(key.x|key.y)||!width||!height||!tile.x||!tile.y||tile.x>16384||tile.y>16384||tile.x*tile.y>65536)return;
    uint rx=control[0].x==2||control[0].x==4?control[0].x:1,ry=control[0].y==2||control[0].y==4?control[0].y:1;
)"
       <<"if(tile.x%(rx*"<<contract.threads[0]<<")||tile.y%(ry*"<<contract.threads[1]<<"))return;\n"
       <<R"(
    uint2 tiles=(uint2(width,height)+tile-1)/tile;
    uint index=group.y*tiles.x+group.x;
    if(group.x>=tiles.x||group.y>=tiles.y||index>=capacity)return;
    float error=0,feature=0;bool valid=true;uint sources=0;
)";
    for(const auto& r:contract.resources)if(r.resource_class==0&&r.range_id<31&&(contract.edge_input_mask&(1u<<r.range_id))){
        out<<"if(mask & "<<(1u<<r.range_id)<<"u){\n"
          <<"float4 lo=3.402823466e+38,hi=-3.402823466e+38,sum=0;uint count=0,invalid=0;\n"
          <<"for(uint i=lane;i<tile.x*tile.y;i+=64){uint2 p=group.xy*tile+uint2(i%tile.x,i/tile.x);if(p.x<width&&p.y<height){float4 v=input"<<r.range_id<<".Load(int3(p,0));if(!all(isfinite(v)))invalid=1;lo=min(lo,v);hi=max(hi,v);sum+=v;++count;}}\n"
          <<R"(minimums[lane]=lo;maximums[lane]=hi;sums[lane]=sum;counts[lane]=count;bad[lane]=invalid;
GroupMemoryBarrierWithGroupSync();
for(uint stride=32;stride;stride>>=1){if(lane<stride){minimums[lane]=min(minimums[lane],minimums[lane+stride]);maximums[lane]=max(maximums[lane],maximums[lane+stride]);sums[lane]+=sums[lane+stride];counts[lane]+=counts[lane+stride];bad[lane]|=bad[lane+stride];}GroupMemoryBarrierWithGroupSync();}
if(lane==0){float4 scale=max(.01,max(abs(minimums[0]),abs(maximums[0])));float4 range=(maximums[0]-minimums[0])/scale;
error=max(error,max(max(range.x,range.y),max(range.z,range.w)));feature+=dot(sums[0]/max(1,counts[0]),float4(.23,.27,.31,.19));valid=valid&&!bad[0]&&counts[0]>0;++sources;}
GroupMemoryBarrierWithGroupSync();
}
)";
    }
    out<<R"(
if(lane)return;
uint page=flags&1,base=32+index*32,previous=base+(page^1)*262144;
uint4 header=map.Load4(previous),data=map.Load4(previous+16);
uint age=frame-header.z;
uint current=base+page*262144;uint4 alternate=map.Load4(current),alternate_data=map.Load4(current+16);
uint alternate_age=frame-alternate.z;
bool previous_valid=all(header.xy==key)&&age>=1&&age<=4;
bool alternate_valid=all(alternate.xy==key)&&alternate_age>=1&&alternate_age<=4;
if(alternate_valid&&(!previous_valid||alternate_age<age)){header=alternate;data=alternate_data;age=alternate_age;}
float old=asfloat(data.y),motion=abs(feature-old)/max(.01,max(abs(feature),abs(old)));
bool confident=valid&&sources>0&&all(header.xy==key)&&age>=1&&age<=4&&isfinite(motion);
error=max(error,motion);
float2 center=(float2(group.xy)+.5)/float2(tiles);
bool central=asfloat(control[6].w)>0&&all(center>=.25)&&all(center<=.75);
uint packed=0;
if(flags&32){
    bool feature_known=confident;
    float mean=abs(feature);uint a=min(7,(uint)(saturate(error/2)*8)),b=min(7,(uint)(saturate(mean/(1+mean))*8)),d=motion<.001?0:motion<.01?1:motion<.1?2:3;
    uint bin=a+8*b+64*d;
    uint4 model=map.Load4(524320),entry=map.Load4(524336+bin*16);
    float bound=asfloat(entry.x),limit=asfloat(control[8].z);uint model_age=frame-entry.z;
    bool certified=any(control[8].xy)&&all(model.xy==control[8].xy)&&model.z==1&&entry.y>=3&&!entry.w&&model_age<=600&&isfinite(bound)&&bound>=0&&bound<=limit;
    bool allowed=feature_known&&certified&&!central;
    uint stable=allowed?min(8,((header.w>>8)&255)==bin?((header.w>>16)&255)+1:1):0;
    confident=allowed&&stable>=8;
    error=certified?bound:1000;
    packed=(bin<<8)|(stable<<16)|(feature_known?0x80000000:0);
}else{if(central||!confident)error=1000;}
uint mode=(rx>1?2:0)|(ry>1?1:0)|(control[1].x==9?4:0)|((control[2].x>0&&control[2].x<=8)?8:0);
mode=confident&&error<=asfloat(control[1].w)?mode:0;
uint address=base+page*262144;
map.Store4(address,uint4(key,frame,mode|packed));
map.Store4(address+16,uint4(asuint(error),asuint(feature),asuint(confident?1.:0.),asuint(motion)));
uint ignored;map.InterlockedAdd(16,mode?1:0,ignored);map.InterlockedAdd(20,1,ignored);
}
)";
    return out.str();
}
}
