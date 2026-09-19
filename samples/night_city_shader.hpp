#pragma once
// Procedural test renderer: analytic ray/box intersections, not Cyberpunk assets
// and not DXR. Scene identity/camera values never enter ARC policy.
static constexpr const char* night_city_shader=R"HLSL(
Texture2D<float4> source:register(t0); RWTexture2D<float4> output_image:register(u0);
RWTexture2D<float4> light_image:register(u1); RWTexture2D<float4> shadow_image:register(u2);
SamplerState linear_sampler:register(s0);
cbuffer FrameInput:register(b0){uint stage;uint material_samples;uint light_steps;uint detailed;uint shadow_rays;uint shadow_scale;uint camera_tick;};
float hash(float n){return frac(sin(n*127.1)*43758.5453);}
void building(uint id,out float3 lo,out float3 hi){
 uint row=id/2;float side=(id%2)==0?-1:1;
 float x=side*(7.5+hash(id+3)*1.5);float z=row*8-8;
 float height=8+hash(id+9)*24;
 lo=float3(x-2.7,0,z-3.2);hi=float3(x+2.7,height,z+3.2);
}
float hitBox(float3 origin,float3 direction,float3 lo,float3 hi,out float3 normal){
 float3 inv=rcp(direction+1e-8);float3 a=(lo-origin)*inv,b=(hi-origin)*inv;
 float3 near=min(a,b),far=max(a,b);float t=max(near.x,max(near.y,near.z));float end=min(far.x,min(far.y,far.z));
 normal=0;
 if(t<.001||end<t)return 1e8;
 if(near.x>=near.y&&near.x>=near.z)normal.x=-sign(direction.x);
 else if(near.y>=near.z)normal.y=-sign(direction.y);else normal.z=-sign(direction.z);
 return t;
}
void trace(float3 origin,float3 direction,out float distance,out float3 normal,out uint object){
 distance=1e6;normal=float3(0,1,0);object=100;
 if(direction.y<-.001){float ground=-origin.y/direction.y;if(ground>0){distance=ground;object=99;}}
 [loop]for(uint i=0;i<28;++i){float3 lo,hi,n;building(i,lo,hi);float t=hitBox(origin,direction,lo,hi,n);if(t<distance){distance=t;normal=n;object=i;}}
}
void camera(float2 uv,out float3 origin,out float3 direction){
 float t=camera_tick/60.0;
 origin=float3(sin(t*2.4)*1.1,2.4+.45*sin(t*3.5),t*30);
 float3 forward=normalize(float3(.12*sin(t*2.4),.06,1));
 float3 right=normalize(cross(float3(0,1,0),forward));float3 up=cross(forward,right);
 float2 screen=(uv-.5)*float2(1.7777778,-1)*1.35;direction=normalize(forward+right*screen.x+up*screen.y);
}
float3 neon(float seed){return lerp(float3(.08,.65,1.0),float3(1.0,.04,.35),step(.5,hash(seed)));}
float3 emission(float3 p,float3 n,uint object){
 if(object>=28)return 0;
 float2 uv=float2(abs(n.x)>.5?p.z:p.x,p.y);
 float2 cell=frac(uv*float2(.65,.42));
 float window=step(.13,cell.x)*step(cell.x,.79)*step(.17,cell.y)*step(cell.y,.73);
 float lit=step(.35,hash(floor(uv.x*.65)+floor(uv.y*.42)*91+object*11));
 float sign_band=step(2.0,p.y)*step(p.y,3.6)*step(.12,frac(uv.x*.38));
 return neon(object+1)*(window*lit*.9+sign_band*1.8);
}
[numthreads(8,8,1)]void main(uint3 tid:SV_DispatchThreadID){
 if(tid.x>=1920||tid.y>=1080)return;
 uint2 extent=uint2(1920,1080);
 if(stage==2){extent=uint2((1920+shadow_scale-1)/shadow_scale,(1080+shadow_scale-1)/shadow_scale);if(any(tid.xy>=extent))return;}
 float2 uv=(float2(tid.xy)+.5)/float2(extent);float3 origin,direction;camera(uv,origin,direction);
 float distance;float3 normal;uint object;trace(origin,direction,distance,normal,object);
 float3 surface_position=origin+direction*distance;
 if(stage==0){
  if(object==100){light_image[tid.xy]=float4(.012,.016,.045,1);return;}
  float3 light=float3(.025,.03,.06);
  // Deterministic integration over a row of area lights. Count changes sample
  // density of the same illumination field, not the presence of scene objects.
  [loop]for(uint j=0;j<light_steps;++j){
   float sample=(j+.5)/light_steps;uint lamp=(uint)(sample*12);float local=frac(sample*12);
   float3 lp=float3((lamp%2)?-4.8:4.8,3.2,origin.z-4+(lamp/2)*9+local*1.4);
   float3 delta=lp-surface_position;float attenuation=5.5/(1+dot(delta,delta));
   light+=neon(lamp)*attenuation*max(.08,dot(normal,normalize(delta)))*12/light_steps;
  }
  light_image[tid.xy]=float4(light,1);return;
 }
 if(stage==1){
  if(object==100){float glow=pow(saturate(1-abs(direction.y)),8);output_image[tid.xy]=float4(float3(.012,.016,.045)+glow*float3(.04,.01,.06),1);return;}
  float2 base=object==99?surface_position.xz*.08:float2(abs(normal.x)>.5?surface_position.z:surface_position.x,surface_position.y)*.08;
  float3 material=0;
  [loop]for(uint i=0;i<material_samples;++i){float2 offset=frac(float2(i*.6180339,i*.4142135));material+=source.SampleLevel(linear_sampler,frac(base+offset*.07),0).rgb;}
  material/=material_samples;
  float3 color=material*light_image[tid.xy].rgb+emission(surface_position,normal,object);
  if(object==99){
   float road=step(abs(surface_position.x),4.0);color*=lerp(.65,.28,road);
   float lane=step(abs(surface_position.x),.065)*step(.5,frac(surface_position.z*.18));color+=lane*float3(.45,.28,.06);
   // Wet-road reflection is an analytic image contribution, not a claimed RT reflection.
   float reflected=exp(-abs(abs(surface_position.x)-3.4)*2.2)*(.35+.65*pow(sin(surface_position.z*.36),8));color+=reflected*neon(floor(surface_position.z/9))*.32;
  }
  float fog=exp(-distance*.018);color=lerp(float3(.015,.01,.035),color,fog);
  output_image[tid.xy]=float4(color/(1+color),1);return;
 }
 if(stage==2){
  if(object==100){shadow_image[tid.xy]=1;return;}
  float visibility=0;
  [loop]for(uint ray=0;ray<shadow_rays;++ray){
   float2 sample=frac(float2(ray*.6180339+.17,ray*.4142135+.31));
   float3 lp=float3(-2+sample.x*4,14,origin.z+12+sample.y*4);
   float3 delta=lp-surface_position;float limit=length(delta);float3 raydir=delta/limit;bool blocked=false;
   [loop]for(uint b=0;b<28;++b){if(b==object)continue;float3 lo,hi,n;building(b,lo,hi);float hit=hitBox(surface_position+normal*.02,raydir,lo,hi,n);blocked=blocked||hit<limit;}
   visibility+=blocked?0:1;
  }
  shadow_image[tid.xy]=float4(visibility/shadow_rays,0,0,1);return;
 }
 float visibility=shadow_image[tid.xy/shadow_scale].x;
 float3 color=output_image[tid.xy].rgb;
 // Emissive signs remain self-lit; shadows affect the reflected/diffuse component.
 float emissive=object<28?saturate(dot(emission(surface_position,normal,object),float3(.3,.3,.3))):0;
 color*=lerp(.6+.4*visibility,1,emissive);
 color*=1-.25*dot(uv-.5,uv-.5);output_image[tid.xy]=float4(saturate(color),1);
}
)HLSL";
