#pragma once
inline constexpr char dynamic_city_shader[]=R"HLSL(
cbuffer Frame:register(b0){
 row_major float4x4 ViewProjection,InverseViewProjection,LightViewProjection;
 float4 CameraTime,Extent;
 float4 LightPosition[24],LightColor[24];
};
Texture2DArray<float4> Materials:register(t0);
Texture2D<float> Shadow:register(t1);
Texture2D<float4> Scene:register(t2);
Texture2D<float> Depth:register(t3);
Texture2D<float4> Fog:register(t4);
RWTexture2D<float4> FogOut:register(u0);
SamplerState LinearWrap:register(s0);SamplerComparisonState ShadowSampler:register(s1);SamplerState LinearClamp:register(s2);
struct Input {float3 p:POSITION;float3 n:NORMAL;float2 uv:TEXCOORD;
 float4 w0:WORLD0;float4 w1:WORLD1;float4 w2:WORLD2;float4 w3:WORLD3;float4 tint:COLOR0;float4 material:COLOR1;};
struct Varying {float4 p:SV_Position;float3 world:TEXCOORD0;float3 normal:TEXCOORD1;nointerpolation float4 tint:TEXCOORD2;nointerpolation float4 material:TEXCOORD3;};
Varying geometry_vs(Input i){Varying o;float4x4 world=float4x4(i.w0,i.w1,i.w2,i.w3);float4 p=mul(float4(i.p,1),world);
 o.world=p.xyz;o.normal=normalize(mul(float4(i.n,0),world).xyz);o.p=mul(p,ViewProjection);o.tint=i.tint;o.material=i.material;return o;}
float4 shadow_vs(Input i):SV_Position {return mul(mul(float4(i.p,1),float4x4(i.w0,i.w1,i.w2,i.w3)),LightViewProjection);}
float shadow_at(float3 world,bool filtered){float4 s=mul(float4(world,1),LightViewProjection);s.xyz/=s.w;float2 uv=s.xy*float2(.5,-.5)+.5;
 if(any(uv<0)||any(uv>1)||s.z<=0||s.z>=1)return 1;
 if(!filtered)return Shadow.SampleCmpLevelZero(ShadowSampler,uv,s.z-.001);
 float result=0;[unroll]for(int y=-1;y<=1;y++)[unroll]for(int x=-1;x<=1;x++)result+=Shadow.SampleCmpLevelZero(ShadowSampler,uv+float2(x,y)/2048.0,s.z-.001);
 return result/9;}
float4 geometry_ps(Varying i):SV_Target {
 float3 n=normalize(i.normal),v=normalize(CameraTime.xyz-i.world);
 float2 uv=abs(n.y)>.8?i.world.xz:(abs(n.x)>.5?i.world.zy:i.world.xy);
 float3 tex=Materials.Sample(LinearWrap,float3(uv*.25*i.material.y,i.material.x)).rgb;
 float3 albedo=tex*i.tint.rgb;float3 sun=normalize(float3(-.4,.8,-.25));
 float3 lit=albedo*(float3(.018,.024,.045)+float3(.19,.23,.34)*max(0,dot(n,sun))*shadow_at(i.world,true));
 float rough=max(.08,i.material.w);float exponent=lerp(150,8,rough);float3 f0=lerp(.04,albedo,i.material.z);
 [loop]for(uint j=0;j<24;j++){float3 delta=LightPosition[j].xyz-i.world;float d2=dot(delta,delta);float radius=LightPosition[j].w;
  if(d2<radius*radius){float3 l=delta*rsqrt(max(.01,d2));float fall=pow(saturate(1-d2/(radius*radius)),2)/(1+.18*d2);
   float diffuse=max(0,dot(n,l));float spec=pow(max(0,dot(n,normalize(l+v))),exponent)*(exponent+2)*.045;
   lit+=LightColor[j].rgb*fall*(albedo*diffuse+f0*spec);}}
 lit+=albedo*i.tint.w;
 return float4(lit,1);
}
[numthreads(8,8,1)]void fog_cs(uint3 id:SV_DispatchThreadID){uint2 size=(uint2(Extent.xy)+1)/2;if(any(id.xy>=size))return;
 float2 uv=(id.xy+.5)/float2(size);float d=Depth.Load(int3(min(uint2(uv*Extent.xy),uint2(Extent.xy)-1),0));
 float4 h=mul(float4(uv*float2(2,-2)+float2(-1,1),d,1),InverseViewProjection);float3 endpoint=h.xyz/h.w;
 float3 delta=endpoint-CameraTime.xyz;float distance=min(length(delta),75);float3 direction=normalize(delta);
 float trans=1;float3 scattering=0;const uint steps=12;float stepLength=distance/steps;
 [loop]for(uint k=0;k<steps;k++){float3 p=CameraTime.xyz+direction*((k+.5)*stepLength);
  float density=.012*(.8+.2*sin(p.x*.21+p.z*.13+CameraTime.w*.17))*exp(-max(0,p.y-1)*.19);
  float visibility=shadow_at(p,false);float3 light=float3(.024,.037,.075)*(.3+.7*visibility);
  [loop]for(uint j=0;j<24;j++){float3 d=LightPosition[j].xyz-p;float d2=dot(d,d);if(d2<100)light+=LightColor[j].rgb*.065*pow(saturate(1-d2/100),2)/(1+d2*.15);}
  float extinction=exp(-density*stepLength);scattering+=trans*(1-extinction)*light;trans*=extinction;
 }
 FogOut[id.xy]=float4(scattering,trans);
}
struct Fullscreen {float4 p:SV_Position;float2 uv:TEXCOORD0;};
Fullscreen fullscreen_vs(uint id:SV_VertexID){Fullscreen o;o.uv=float2((id<<1)&2,id&2);o.p=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o;}
float3 tone(float3 x){return saturate((x*(2.51*x+.03))/(x*(2.43*x+.59)+.14));}
float4 composite_ps(Fullscreen i):SV_Target {
 float3 color=Scene.SampleLevel(LinearClamp,i.uv,0).rgb;float4 fog=Fog.SampleLevel(LinearClamp,i.uv,0);color=color*fog.a+fog.rgb;
 float3 bloom=0;[unroll]for(int y=-1;y<=1;y++)[unroll]for(int x=-1;x<=1;x++)bloom+=max(0,Scene.SampleLevel(LinearClamp,i.uv+float2(x,y)*Extent.zw*5,0).rgb-1);
 color+=bloom*.025;color=tone(color*1.7);color*=1-.15*dot(i.uv-.5,i.uv-.5);
 return float4(pow(max(color,0),1/2.2),1);
}
)HLSL";
