#pragma once

// The two plan rewrites before DuckDB's optimizers (ADR-0006). Neither recycles: both make the plan cheaper and give
// its parts keys that later statements repeat.

#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class Optimizer;

namespace recycling {

// Removes each LEFT JOIN whose right side nothing reads and is unique on the join's equality keys. `referenced`: the
// columns read above `op`; for the statement's root, its output columns. Runs before the query result's key is taken.
void RemoveUnusedLeftJoins(unique_ptr<LogicalOperator> &op, column_binding_set_t referenced);
// Replaces the join to DELIM_GET of each simple equality EXISTS by the relation it joins. If the plan holds a DELIM
// join, first runs `optimizer`'s filter pushdown, which turns a mark join used only as a filter into a semi join. Runs
// after RecycleBlocks.
void RemoveExistenceDelimJoins(Optimizer &optimizer, unique_ptr<LogicalOperator> &plan);

} // namespace recycling
} // namespace duckdb
