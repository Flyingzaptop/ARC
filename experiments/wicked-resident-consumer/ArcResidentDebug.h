#pragma once
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <string>
inline std::string arc_resident_debug_path;
inline std::atomic<unsigned> arc_resident_debug_count{0};
inline void CALLBACK ArcResidentDebugMessage(D3D12_MESSAGE_CATEGORY,D3D12_MESSAGE_SEVERITY severity,D3D12_MESSAGE_ID id,LPCSTR description,void*){
    if(severity>D3D12_MESSAGE_SEVERITY_WARNING||arc_resident_debug_count.fetch_add(1)>1000)return;
    FILE* f{};fopen_s(&f,arc_resident_debug_path.c_str(),"a");if(f){std::fprintf(f,"severity=%d id=%d %s\n",int(severity),int(id),description);std::fclose(f);}
}
inline void ArcResidentDebugInstall(ID3D12Device* device){
    char mode[8]{};GetEnvironmentVariableA("ARC_RESIDENT_QUEUE",mode,8);
    if(mode[0]=='1'||mode[0]=='2'){
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 options{};
        HRESULT hr=device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1,&options,sizeof(options));
        char path[1024]{};GetEnvironmentVariableA("ARC_WICKED_CPU_PROFILE",path,1024);
        auto name=std::string(path)+".capabilities.json";FILE* f{};fopen_s(&f,name.c_str(),"w");
        if(f){auto luid=device->GetAdapterLuid();std::fprintf(f,"{\"int64_shader_ops\":%s,\"adapter_luid_high\":%ld,\"adapter_luid_low\":%lu}\n",SUCCEEDED(hr)&&options.Int64ShaderOps?"true":"false",luid.HighPart,luid.LowPart);std::fclose(f);}
        if(FAILED(hr)||!options.Int64ShaderOps)ExitProcess(98);
    }
    char enabled[8]{};GetEnvironmentVariableA("ARC_RESIDENT_DEBUG",enabled,8);if(enabled[0]!='1')return;
    char path[1024]{};GetEnvironmentVariableA("ARC_WICKED_CPU_PROFILE",path,1024);arc_resident_debug_path=std::string(path)+".d3d12.txt";
    Microsoft::WRL::ComPtr<ID3D12InfoQueue1> info;
    if(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&info)))){DWORD cookie{};info->RegisterMessageCallback(ArcResidentDebugMessage,D3D12_MESSAGE_CALLBACK_FLAG_NONE,nullptr,&cookie);}
}
