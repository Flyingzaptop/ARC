#pragma once
#include <string>
#include <string_view>
namespace arc::dx12::shader {
struct ZeroFactorTransform {std::string ir;unsigned regions{},estimated_skipped_operations{};};
// Skip pure acyclic regions whose escaping values are all provably zero when
// a dominating scalar factor is zero. Proof uses existing fast-math contracts;
// memory writes, loops, opaque calls and escaping nonzero values prohibit it.
ZeroFactorTransform short_circuit_zero_factors(std::string_view controlled_ir);
}
