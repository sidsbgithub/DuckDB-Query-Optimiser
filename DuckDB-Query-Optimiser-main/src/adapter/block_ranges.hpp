#pragma once

// The ranges of blocks (ADR-0005): which WHERE conjuncts put a range on a column, and how ranges combine and contain
// one another. Part of the blocks mechanism; only blocks.cpp includes it.

#include "recycling_state.hpp"

namespace duckdb {

class ClientContext;
class Expression;
struct ColumnBinding;

namespace recycling {

// If the conjunct keeps the rows whose column is within a range (a comparison or BETWEEN of an integer or date column
// with constants, or IS NOT NULL), the column and the range.
bool ExtractRange(ClientContext &context, const Expression &conjunct, ColumnBinding &column, BlockRange &range);
// Narrows `range` to the values that are also within `other`.
void Intersect(BlockRange &range, const BlockRange &other);
// Whether every block with ranges `inner` keeps only rows that a block with ranges `outer` keeps.
bool RangesContain(const BlockRanges &outer, const BlockRanges &inner);
// The ranges as text, for a block's key.
string RangesKey(const BlockRanges &ranges);

} // namespace recycling
} // namespace duckdb
