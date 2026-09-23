#pragma once
#include "generic_shader_transform.hpp"
#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace arc::dx12::shader::structured {
enum class Action { Coarsen, ExactReuse, LocalArithmetic, SampleReduction, ComparisonReduction };
enum class Effect { Pure, ResourceRead, ResourceWrite, Barrier, PrivateMemory, Unknown };
struct Value {
    std::string name,type,operation,block;
    std::vector<std::string> operands;
    bool lane_varying{};
    bool finite_index{};
    unsigned index_min{},index_max{};
};
struct Operation {
    std::string opcode,result,type,callee;
    std::vector<std::string> operands;
    Effect effect{Effect::Unknown};
    unsigned line{};
};
struct Block {
    std::string label;
    std::vector<Operation> operations;
    std::set<std::string> successors,predecessors,control_values;
};
struct Binding {
    unsigned resource_class{},range_id{},lower{},upper{},space{};
    unsigned index_min{},index_max{};
    bool dynamic{},nonuniform{},annotated{};
    unsigned properties0{},properties1{};
};
struct Obligation {
    std::string kind;
    unsigned resource_class{},range_id{},lower{},upper{},space{};
};
struct Capability {
    bool admitted{};
    std::string reason;
    std::vector<Obligation> obligations;
};
struct Program {
    bool structured_valid{},finite_bindings_complete{},has_unknown_effects{},has_barrier{},writes_covered{};
    std::string reason;
    unsigned shader_model_minor{};
    std::array<unsigned,3> threads{};
    std::vector<ResourceContract> resources;
    std::vector<Block> blocks;
    std::map<std::string,Value> values;
    std::map<std::string,Binding> handles;
    unsigned resource_reads{},resource_writes{};
    Capability capability(Action action) const;
    std::optional<Binding> binding_handle(std::string_view name) const;
};
// The frontend accepts DXC 1.9.2602.24's textual DXIL 1.6 form. It proves
// only finite register-table bindings; direct heap handles fail closed.
Program analyze_dxil(std::string_view ir,const std::vector<ResourceContract>& resources,
    const std::array<unsigned,3>& threads,unsigned shader_model_minor);
}
