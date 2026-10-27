#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace waddle {

struct KeyBinding {
	uint64_t table = 0;
	uint64_t column = 0;

	bool operator==(const KeyBinding &other) const {
		return table == other.table && column == other.column;
	}
};

struct KeyExpression {
	int kind = 0;
	std::string type;
	std::string value;
	bool is_column_reference = false;
	KeyBinding binding;
	std::vector<KeyExpression> children;
};

struct KeyJoinCondition {
	int comparison = 0;
	KeyExpression left;
	KeyExpression right;
};

// Plain description of the optimized subtree. The adapter translates DuckDB objects into this form. Each field
// describes one kind of operator and stays empty (or 0) for the others.
struct KeyNode {
	// The operator's type.
	int kind = 0;
	// A scan: the table it reads, its columns, projection and pushed-down filters (column, filter text). A block
	// is described as a scan too: its source is its key, its columns the (table, column) pairs the statement reads.
	std::string scan_source;
	std::vector<uint64_t> scan_columns;
	std::vector<uint64_t> scan_projection;
	std::vector<std::pair<uint64_t, std::string>> scan_filters;
	// A filter.
	std::vector<uint64_t> filter_projection;
	// A comparison join.
	int join_kind = 0;
	std::vector<uint64_t> left_projection;
	std::vector<uint64_t> right_projection;
	std::vector<KeyJoinCondition> join_conditions;
	// An aggregate (and the groups of a DISTINCT).
	int distinct_validity = 0;
	std::vector<uint64_t> grouping_set;
	std::vector<KeyExpression> groups;
	// Any operator's own expressions.
	std::vector<KeyExpression> expressions;
	// The columns the children produce, in order: an expression refers to one by its position here.
	std::vector<KeyBinding> input_bindings;
	std::vector<std::string> output_types;
	std::vector<std::unique_ptr<KeyNode>> children;
};

// Serializes the subtree and renumbers bound columns against their input positions.
// Returns false if a column refers outside the subtree.
bool EncodeRecyclingKey(const KeyNode &node, std::string &key);

} // namespace waddle
