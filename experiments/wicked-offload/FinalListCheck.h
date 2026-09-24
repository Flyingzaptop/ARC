#pragma once
#include <cstdint>
#include <span>
#include <vector>

// Independent CPU expected membership, actual post-join consumer indices.
inline bool ArcFinalListMatches(std::span<const uint8_t> expected,
                                std::span<const uint32_t> actual) {
    size_t count=0;
    for(auto bit:expected) count+=bit!=0;
    if(actual.size()!=count) return false;
    std::vector<uint8_t> seen(expected.size());
    for(auto index:actual) {
        if(index>=expected.size() || !expected[index] || seen[index]) return false;
        seen[index]=1;
    }
    return true;
}
