#pragma once

// Recycling of the subtrees rooted at pipeline breakers, after DuckDB's optimizers (ADR-0002).

#include "duckdb/optimizer/optimizer_extension.hpp"

namespace duckdb {
namespace recycling {

// Replays every recyclable subtree that repeats an earlier one and captures the others that may repeat, outermost
// first. Does nothing for plain EXPLAIN or when the statement may not recycle (MayRecycle). Applies its own binding
// replacements. RecyclingOptimize calls it after MarkBlocksFilteredFromOutside and before the query result's capture.
void RecycleBreakers(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);

} // namespace recycling
} // namespace duckdb
