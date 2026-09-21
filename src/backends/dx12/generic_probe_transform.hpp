#pragma once
#include "generic_shader_transform.hpp"
namespace arc::dx12::shader {
struct ProbeTransform {bool admitted{};std::string reason,ir;unsigned outputs{};};
// Redirect application texture stores to a private raw root UAV. Sparse samples
// carry a fresh 64-bit epoch and channel masks; no application output is written.
ProbeTransform sparse_probe(const Transform&);
}
