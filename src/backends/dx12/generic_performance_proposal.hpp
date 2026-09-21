#pragma once
#include "generic_shader_cache.hpp"
#include "arc/optimizer_policy.hpp"
#include "json.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <optional>
namespace arc::dx12::proposal {
inline bool valid_key(const std::string& key){return key.size()==64&&std::all_of(key.begin(),key.end(),[](char c){return(c>='a'&&c<='f')||(c>='0'&&c<='9');});}
inline std::string hash(const std::string& text){return shader_cache::digest({reinterpret_cast<const std::byte*>(text.data()),text.size()});}
inline std::optional<arc::ComputePolicy> load(const std::filesystem::path& root,const std::string& key,std::uint64_t pipeline)noexcept{
    try{if(!valid_key(key)||!pipeline)return {};const auto path=root/(key+".json");if(std::filesystem::file_size(path)>16384)return {};std::ifstream file(path);const auto data=nlohmann::json::parse(file);
        if(data.at("schema")!=1||data.at("key")!=key||data.at("requires_fresh_validation")!=true||data.at("sha256")!=hash(data.at("recipe").dump()))return {};
        const auto& r=data.at("recipe");arc::ComputePolicy p;p.pipeline=pipeline;p.x_rate=r.at("x");p.y_rate=r.at("y");p.comparison_taps=r.at("comparison");p.zero_factor=r.at("zero");p.mip_steps=r.at("mip");p.sample_percent=r.at("samples");p.protect_edges=r.at("spatial");p.edge_threshold=r.at("threshold");
        arc::PolicyBundle check{1,{p},false};return check.valid()?std::optional(p):std::nullopt;
    }catch(...){return {};}
}
inline bool save(const std::filesystem::path& root,const std::string& key,const arc::ComputePolicy& p)noexcept{
    try{if(!valid_key(key)||!arc::PolicyBundle{1,{p},false}.valid())return false;std::filesystem::create_directories(root);
        const nlohmann::json recipe{{"x",p.x_rate},{"y",p.y_rate},{"comparison",p.comparison_taps},{"zero",p.zero_factor},{"mip",p.mip_steps},{"samples",p.sample_percent},{"spatial",p.protect_edges},{"threshold",p.edge_threshold}};
        const nlohmann::json data{{"schema",1},{"key",key},{"requires_fresh_validation",true},{"recipe",recipe},{"sha256",hash(recipe.dump())}};
        const auto path=root/(key+".json"),temporary=root/(key+".tmp-"+std::to_string(GetCurrentProcessId()));{std::ofstream file(temporary);file<<data.dump();file.close();if(!file)return false;}
        if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return false;
        std::vector<std::filesystem::directory_entry> entries;for(const auto& e:std::filesystem::directory_iterator(root))if(e.is_regular_file()&&!e.is_symlink()&&e.path().extension()==L".json"&&valid_key(e.path().stem().string()))entries.push_back(e);
        std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b){return a.last_write_time()<b.last_write_time();});for(std::size_t i=0;entries.size()>256&&i<entries.size()-256;++i)if(entries[i].path()!=path)std::filesystem::remove(entries[i].path());return true;
    }catch(...){return false;}
}
}
