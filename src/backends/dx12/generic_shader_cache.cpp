#include "generic_shader_cache.hpp"
#include "json.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <fstream>
#include <vector>
#include <array>
#include <algorithm>
#include <stdexcept>
namespace arc::dx12::shader_cache {
namespace {
using Json=nlohmann::json;
std::filesystem::path suffix(std::filesystem::path path,std::string_view ending){path+=std::filesystem::path(ending);return path;}
const std::array<const char*,3> suffixes{"", ".contract", ".access.ll"};
bool valid_key(const std::string& key){return key.size()==64&&std::all_of(key.begin(),key.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');});}
std::vector<std::byte> read(const std::filesystem::path& path,std::uintmax_t cap=128*1024*1024){
    const auto size=std::filesystem::file_size(path);if(!size||size>cap)throw std::runtime_error("Shader cache file size");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));std::ifstream file(path,std::ios::binary);file.read(reinterpret_cast<char*>(bytes.data()),bytes.size());if(!file)throw std::runtime_error("Shader cache read");return bytes;
}
void write(const std::filesystem::path& path,std::span<const std::byte> bytes){std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());file.close();if(!file)throw std::runtime_error("Shader cache write");}
}
std::string digest(std::span<const std::byte> bytes){
    if(bytes.size()>ULONG_MAX)throw std::runtime_error("Hash size");std::array<UCHAR,32> hash{};
    if(BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())),ULONG(bytes.size()),hash.data(),ULONG(hash.size()))<0)throw std::runtime_error("SHA256");
    constexpr char hex[]="0123456789abcdef";std::string result;result.reserve(64);for(auto value:hash){result+=hex[value>>4];result+=hex[value&15];}return result;
}
std::string file_digest(const std::filesystem::path& path){return digest(read(path));}
bool restore(const std::filesystem::path& root,const std::string& key,const std::filesystem::path& binary)noexcept{
    try{if(!valid_key(key))return false;const auto stem=root/key;if(std::filesystem::file_size(suffix(stem,".json"))>4096)return false;std::ifstream file(suffix(stem,".json"));auto manifest=Json::parse(file);if(manifest.at("schema")!=1||manifest.at("key")!=key)return false;
        std::array<std::vector<std::byte>,3> contents;
        for(unsigned i=0;i<suffixes.size();++i){contents[i]=read(suffix(stem,suffixes[i]),8*1024*1024);if(digest(contents[i])!=manifest.at("hashes").at(i).get<std::string>())return false;}
        for(unsigned i=0;i<suffixes.size();++i)write(suffix(binary,suffixes[i]),contents[i]);
        std::error_code ignored;std::filesystem::last_write_time(suffix(stem,".json"),std::filesystem::file_time_type::clock::now(),ignored);return true;
    }catch(...){return false;}
}
bool store(const std::filesystem::path& root,const std::string& key,const std::filesystem::path& binary)noexcept{
    try{if(!valid_key(key))return false;std::filesystem::create_directories(root);Json manifest{{"schema",1},{"key",key},{"hashes",Json::array()}};
        const auto stem=root/key;const auto tag=".tmp-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64());
        for(auto ending:suffixes){const auto bytes=read(suffix(binary,ending),8*1024*1024);manifest["hashes"].push_back(digest(bytes));const std::filesystem::path final=suffix(stem,ending),temp=suffix(final,tag);write(temp,bytes);if(!MoveFileExW(temp.c_str(),final.c_str(),MOVEFILE_REPLACE_EXISTING))return false;}
        const std::filesystem::path final=suffix(stem,".json"),temp=suffix(final,tag);{std::ofstream file(temp);file<<manifest.dump();file.close();if(!file)return false;}
        return MoveFileExW(temp.c_str(),final.c_str(),MOVEFILE_REPLACE_EXISTING)!=FALSE;
    }catch(...){return false;}
}
std::string restore_decline(const std::filesystem::path& root,const std::string& key)noexcept{
    try{if(!valid_key(key))return {};const auto path=root/(key+".json");if(std::filesystem::file_size(path)>4096)return {};
        std::ifstream file(path);const auto value=Json::parse(file);const auto reason=value.at("declined").get<std::string>();
        if(value.at("schema")!=1||value.at("key")!=key||reason.empty()||reason.size()>512||value.at("reason_sha256")!=digest({reinterpret_cast<const std::byte*>(reason.data()),reason.size()}))return {};
        return reason;
    }catch(...){return {};}
}
bool store_decline(const std::filesystem::path& root,const std::string& key,const std::string& reason)noexcept{
    try{if(!valid_key(key)||reason.empty()||reason.size()>512)return false;std::filesystem::create_directories(root);
        const auto final=root/(key+".json"),temp=root/(key+".tmp-"+std::to_string(GetCurrentProcessId()));
        const Json value{{"schema",1},{"key",key},{"declined",reason},{"reason_sha256",digest({reinterpret_cast<const std::byte*>(reason.data()),reason.size()})}};
        std::ofstream file(temp);file<<value.dump();file.close();return file&&MoveFileExW(temp.c_str(),final.c_str(),MOVEFILE_REPLACE_EXISTING);
    }catch(...){return false;}
}
void trim(const std::filesystem::path& root,std::uintmax_t maximum)noexcept{
    try{if(!std::filesystem::is_directory(root))return;std::uintmax_t total=0;std::vector<std::filesystem::directory_entry> manifests;
        for(const auto& entry:std::filesystem::directory_iterator(root)){if(!entry.is_regular_file())continue;total+=entry.file_size();if(entry.path().extension()==L".json"&&valid_key(entry.path().stem().string()))manifests.push_back(entry);}
        std::sort(manifests.begin(),manifests.end(),[](const auto& a,const auto& b){return a.last_write_time()<b.last_write_time();});
        for(const auto& entry:manifests){if(total<=maximum)break;const auto key=entry.path().stem().string();for(const char* suffix:{".json","", ".contract", ".access.ll"}){const auto path=root/(key+suffix);if(std::filesystem::is_regular_file(path)){const auto bytes=std::filesystem::file_size(path);if(std::filesystem::remove(path))total=total>bytes?total-bytes:0;}}}
    }catch(...){}
}
}
