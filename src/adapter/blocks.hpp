#pragma once

// Blocks: the joins and filters under a select list or grouping, recycled by table columns and ranges (ADR-0005).

#include "duckdb/optimizer/optimizer_extension.hpp"

namespace duckdb {
namespace recycling {

// Before DuckDB's optimizers: marks each outermost block with WADDLE_BLOCK, which captures it or counts its rows, or
// replays a block result that holds the columns the statement reads. Applies its own binding replacements.
void RecycleBlocks(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &root);
// After DuckDB's optimizers, before anything else: a block with a filter directly above it may hold predicates from
// outside it. Such a block is neither captured nor counted, and no recycling key describes it.
void MarkBlocksFilteredFromOutside(LogicalOperator &op);

} // namespace recycling
} // namespace duckdb
