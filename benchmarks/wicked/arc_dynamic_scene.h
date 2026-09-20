#pragma once
// Owned fixture motion only. No scene data is passed to ARC's optimizer.
#include <windows.h>
#include <chrono>
#include <cmath>
#include <fstream>
#include <string>
namespace arc_dynamic_scene {
inline bool enabled(){static const bool value=[] {wchar_t v[8]{};return GetEnvironmentVariableW(L"ARC_WICKED_DYNAMIC_CAMERA",v,8)==1&&v[0]==L'1';}();return value;}
inline void update(int selected){
    if(!enabled())return;
    using namespace DirectX;auto& scene=wi::scene::GetScene();auto& camera=wi::scene::GetCamera();
    const auto first=scene.objects.GetCount()?scene.objects.GetEntity(0):wi::ecs::INVALID_ENTITY;
    static int last=-1;static wi::ecs::Entity previous=wi::ecs::INVALID_ENTITY;static XMFLOAT4X4 origin{};
    static auto started=std::chrono::steady_clock::now();static unsigned frames=0;
    static std::ofstream log([]{wchar_t value[32768]{};GetEnvironmentVariableW(L"ARC_BENCH_OUTPUT",value,32768);return std::wstring(value)+L"/motion.jsonl";}());
    if(last!=selected||previous!=first){last=selected;previous=first;started=std::chrono::steady_clock::now();XMStoreFloat4x4(&origin,XMMatrixInverse(nullptr,camera.GetView()));frames=0;}
    const float t=std::chrono::duration<float>(std::chrono::steady_clock::now()-started).count();
    const float phase=t*XM_2PI/8.f;
    const auto world=XMMatrixRotationRollPitchYaw(.035f*std::sin(phase*.7f),-.12f*std::sin(phase),0)*XMLoadFloat4x4(&origin)*XMMatrixTranslation(1.1f*std::sin(phase),.25f*std::sin(phase*.7f),.5f*(1-std::cos(phase)));
    camera.TransformCamera(world);camera.UpdateCamera();
    // 2D Hello World keeps its own existing sprite animation; camera motion
    // matters in the 3D scenes, including the already-animated 65k instances.
    if(log&&frames++%30==0)log<<"{\"scene\":"<<selected<<",\"seconds\":"<<t<<",\"eye\":["<<camera.Eye.x<<','<<camera.Eye.y<<','<<camera.Eye.z<<"],\"objects\":"<<scene.objects.GetCount()<<"}\n";
}
}
