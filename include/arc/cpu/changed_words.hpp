#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace arc::cpu {
inline std::size_t encode_changed_words(const void* input,std::uint64_t* previous,std::size_t words,unsigned char* output,std::size_t capacity) noexcept {
    const auto mask_bytes=(words+7)/8;
    if(capacity<mask_bytes+words*8)return 0;
    std::memset(output,0,mask_bytes);std::size_t used=mask_bytes;
    auto* bytes=static_cast<const unsigned char*>(input);
    for(std::size_t i=0;i<words;++i){std::uint64_t value;std::memcpy(&value,bytes+i*8,8);
        if(value!=previous[i]){output[i/8]|=1u<<(i%8);std::memcpy(output+used,&value,8);used+=8;previous[i]=value;}}
    return used;
}
}
