#include "generic_performance_proposal.hpp"
#include "generic_shader_cache.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <cassert>
#include <iostream>
namespace cache=arc::dx12::shader_cache;
int main(){
    const auto root=std::filesystem::temp_directory_path()/(L"arc-cache-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"-\u0442\u0435\u0441\u0442");
    std::filesystem::create_directories(root);const auto source=root/L"source",destination=root/L"restored";
    for(const char* ending:{"", ".contract", ".access.ll", ".spatial.bin"}){auto path=source;path+=ending;std::ofstream file(path);file<<"known bytes "<<ending;}
    const auto key=cache::file_digest(source);assert(key.size()==64);assert(!cache::restore(root/L"cache",key,destination));
    assert(cache::store(root/L"cache",key,source));assert(cache::restore(root/L"cache",key,destination));assert(cache::file_digest(destination)==key);
    {std::ofstream damaged(root/L"cache"/(key+".spatial.bin"));damaged<<"corrupt spatial";}
    assert(!cache::restore(root/L"cache",key,root/L"bad-spatial"));assert(cache::store(root/L"cache",key,source));
    {std::ofstream damaged(root/L"cache"/key);damaged<<"corrupt";}
    auto missing=root/L"must-not-exist";assert(!cache::restore(root/L"cache",key,missing)&&!std::filesystem::exists(missing));
    assert(cache::store(root/L"cache",key,source)&&cache::restore(root/L"cache",key,destination));
    assert(!cache::store(root/L"cache","../escape",source));assert(!cache::restore(root/L"cache",std::string(64,'f'),missing));
    cache::trim(root/L"cache",0);assert(!cache::restore(root/L"cache",key,missing));
    assert(cache::store_decline(root/L"cache",key,"Shader declined: unsupported_group_memory"));
    assert(cache::restore_decline(root/L"cache",key)=="Shader declined: unsupported_group_memory");
    assert(cache::restore_decline(root/L"cache",std::string(64,'e')).empty());
    {std::ofstream corrupt(root/L"cache"/(key+".json"));corrupt<<"{bad json";}
    assert(cache::restore_decline(root/L"cache",key).empty());
    cache::trim(root/L"cache",0);
    const auto proposals=root/L"proposals";arc::ComputePolicy recipe;recipe.pipeline=1;recipe.x_rate=2;recipe.sample_percent=75;
    assert(!arc::dx12::proposal::load(proposals,key,9));assert(arc::dx12::proposal::save(proposals,key,recipe));auto restored=arc::dx12::proposal::load(proposals,key,9);assert(restored&&restored->pipeline==9&&restored->sample_percent==75&&restored->x_rate==2);
    assert(!arc::dx12::proposal::load(proposals,std::string(64,'a'),9));assert(!arc::dx12::proposal::save(proposals,"../escape",recipe));
    {std::fstream file(proposals/(key+".json"),std::ios::in);auto data=nlohmann::json::parse(file);file.close();data["recipe"]["samples"]=25;std::ofstream corrupt(proposals/(key+".json"));corrupt<<data.dump();}
    assert(!arc::dx12::proposal::load(proposals,key,9));std::filesystem::remove(proposals/(key+".json"));std::filesystem::remove(proposals);
    // Delete only individually known files in this fresh, owned directory.
    for(const char* ending:{"", ".contract", ".access.ll", ".spatial.bin"})for(auto base:{source,destination}){base+=ending;std::filesystem::remove(base);}
    std::filesystem::remove(root/L"cache");std::filesystem::remove(root);
    std::cout<<"Shader cache cold/warm, corruption rejection, Unicode paths and eviction PASS\n";
}
