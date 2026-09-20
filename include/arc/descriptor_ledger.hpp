#pragma once
#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>
#include <unordered_map>
#include <utility>

namespace arc {
// Optional exact native-view metadata. Stored in the interned value, not in
// every heap slot, so large bindless heaps still cost four bytes per slot.
// Legacy observations leave known=false and cannot authorize mutations.
struct DescriptorShape {
    std::uint32_t format{}, dimension{}, first_slice{}, slices{}, plane{}, flags{};
    std::uint32_t elements{}, stride{}, component_mapping{};
    std::uint64_t byte_offset{}, byte_size{}, first_element{}, counter_resource{};
    bool known{};
    auto operator<=>(const DescriptorShape&)const=default;
};
struct DescriptorValue {
    std::uint64_t resource{};
    std::uint32_t kind{},first_mip{},mips{};
    DescriptorShape shape{};
    auto operator<=>(const DescriptorValue&)const=default;
};
struct DescriptorValueHash {
    std::size_t operator()(const DescriptorValue& v) const noexcept {
        std::uint64_t h=0x9e3779b97f4a7c15ull;
        auto add=[&](std::uint64_t n){h^=n+0x9e3779b97f4a7c15ull+(h<<6)+(h>>2);};
        add(v.resource);add(v.kind);add(v.first_mip);add(v.mips);
        const auto& s=v.shape;add(s.format);add(s.dimension);add(s.first_slice);add(s.slices);add(s.plane);add(s.flags);
        add(s.elements);add(s.stride);add(s.component_mapping);add(s.byte_offset);add(s.byte_size);add(s.first_element);add(s.counter_resource);add(s.known);
        return static_cast<std::size_t>(h);
    }
};
struct DescriptorLedgerLimits {
    std::size_t slots{8u*1024u*1024u}, values{65536}, orphan_slots{65536}, heaps{4096};
};
// Dense bounded slot IDs + reference-counted interned values. Duplicate/null
// descriptors consume four bytes per native slot, not one tree node per slot.
// This records descriptor contents only, never proves actual shader access.
class DescriptorLedger {
public:
    explicit DescriptorLedger(DescriptorLedgerLimits limits={}):limits_(limits){}
    bool register_heap(std::uint64_t id,std::uint64_t start,std::uint32_t stride,std::uint32_t count);
    void retire_heap(std::uint64_t id);
    bool write(std::uint64_t address,DescriptorValue);
    // Preserve the interned identity for the common single-descriptor copy.
    // Unknown source contents invalidate the destination.
    bool copy_one(std::uint64_t destination,std::uint64_t source);
    void forget(std::uint64_t address);
    void forget_all(); // preserve heap identities, invalidate every cached value
    [[nodiscard]] std::optional<DescriptorValue> read(std::uint64_t address)const;
    [[nodiscard]] std::uint64_t heap_at(std::uint64_t address)const;
    [[nodiscard]] std::size_t slot_count()const noexcept{return slots_;}
    [[nodiscard]] std::size_t known_count()const noexcept{return known_;}
    [[nodiscard]] std::size_t value_count()const noexcept{return lookup_.size();}
    [[nodiscard]] std::size_t orphan_count()const noexcept{return orphans_.size();}
    [[nodiscard]] std::size_t null_count()const noexcept{return nulls_;}
private:
    struct Heap {std::uint64_t id{},start{};std::uint32_t stride{};std::vector<std::uint32_t> entries;std::unordered_map<std::uint32_t,std::size_t> references;};
    const Heap* locate_heap(std::uint64_t address)const;
    Heap* locate_heap(std::uint64_t address){return const_cast<Heap*>(std::as_const(*this).locate_heap(address));}
    struct Value {DescriptorValue data;std::size_t references{};};
    DescriptorLedgerLimits limits_;
    std::map<std::uint64_t,Heap> heaps_;
    std::map<std::uint64_t,std::uint64_t> starts_;
    std::unordered_map<DescriptorValue,std::uint32_t,DescriptorValueHash> lookup_;
    std::vector<Value> values_{{}};
    std::vector<std::uint32_t> free_;
    std::map<std::uint64_t,std::uint32_t> orphans_;
    std::size_t slots_{},known_{},nulls_{};
    std::uint32_t intern(DescriptorValue);
    void release(std::uint32_t,std::size_t count=1);
};
}
