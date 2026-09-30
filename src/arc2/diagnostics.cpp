#include "arc/arc2/frontend.hpp"
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <vector>
#include <fstream>
#include <filesystem>
extern "C" __declspec(dllexport) unsigned long long WINAPI Arc2DebugErrors(IUnknown* device,const wchar_t* path){
 try{Microsoft::WRL::ComPtr<ID3D12InfoQueue> info;auto raw=arc::arc2::unwrap_unknown(device);if(!raw||FAILED(raw->QueryInterface(IID_PPV_ARGS(&info))))return ~0ull;
 unsigned long long errors=0;std::ofstream out;if(path)out.open(std::filesystem::path(path));
 const auto count=info->GetNumStoredMessagesAllowedByRetrievalFilter();for(UINT64 i=0;i<count;++i){SIZE_T size{};info->GetMessage(i,nullptr,&size);std::vector<unsigned char> buffer(size);auto message=reinterpret_cast<D3D12_MESSAGE*>(buffer.data());if(SUCCEEDED(info->GetMessage(i,message,&size))){if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)++errors;if(out)out<<message->ID<<':'<<message->Severity<<':'<<message->pDescription<<'\n';}}return errors;
 }catch(...){return ~0ull;}
}
