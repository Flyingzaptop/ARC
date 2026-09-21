#pragma once
#include <filesystem>
#include <set>
#include <vector>
#include <algorithm>
#include <fstream>
#include <cstdint>
namespace arc::dx12 {
// Deletes only .pixels payloads in this session's own non-symlink trial folders.
// Final archives, other sessions and decision metadata are never rotated here.
inline bool reserve_trial_storage(const std::filesystem::path& root,std::uint64_t reserve,const std::set<std::filesystem::path>& pinned){
    constexpr std::uint64_t limit=1ull<<30;if(reserve>limit)return false;
    struct Payload {std::filesystem::path path;std::uint64_t bytes;std::filesystem::file_time_type time;};
    std::vector<Payload> files;std::uint64_t total{};
    for(const auto& directory:std::filesystem::directory_iterator(root)){
        if(directory.is_symlink()||!directory.is_directory()||!directory.path().filename().wstring().starts_with(L"trial-"))continue;
        for(const auto& file:std::filesystem::directory_iterator(directory.path())){
            if(file.is_symlink()||!file.is_regular_file()||file.path().extension()!=L".pixels")continue;
            const auto size=file.file_size();total+=size;
            if(!pinned.contains(directory.path()))files.push_back({file.path(),size,file.last_write_time()});
        }
    }
    std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){return a.time<b.time;});
    for(const auto& file:files){if(total+reserve<=limit)break;
        if(std::filesystem::remove(file.path)){total-=file.bytes;std::ofstream note(file.path.parent_path()/L"pixels-pruned.txt");note<<"Unpinned temporary pixel payloads rotated under the 1 GiB session quota. Decision metadata retained.\n";}}
    return total+reserve<=limit;
}
}
