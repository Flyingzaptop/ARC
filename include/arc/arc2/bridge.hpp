#pragma once
#include "arc/arc2/ir.hpp"
#include "arc/resource_graph.hpp"

namespace arc::arc2 {
// Converts a retained IR snapshot into the existing slow-path graph. Unknown
// accesses remain in IR; this adapter forwards only known resource IDs.
void feed_resource_graph(const IrSnapshot& ir, arc::ResourceGraph& graph);
} // namespace arc::arc2
