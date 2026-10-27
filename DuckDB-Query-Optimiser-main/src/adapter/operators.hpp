#pragma once

// The operators the adapter puts into plans: WADDLE_CAPTURE stores a subtree's rows as a recycled result,
// WADDLE_BLOCK marks a block and captures or counts it, and the waddle_replay scan replays a recycled result. Also
// the plan edits that put them in place: replaying a subtree (MakeReplay, ReplaceWithReplay, ReplaceBindings),
// wrapping one in a capture (WrapInCapture), and recognising a block marker (IsLogicalBlock).

#include "recycling_state.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

namespace duckdb {

class Binder;

namespace recycling {

//===--------------------------------------------------------------------===//
// Logical capture operator (wraps the pipeline breaker, keeps its bindings)
//===--------------------------------------------------------------------===//
class LogicalCapture : public LogicalExtensionOperator {
public:
	LogicalCapture(unique_ptr<LogicalOperator> child, string key, waddle::ResultKind kind, idx_t prefix_rows)
	    : key(std::move(key)), kind(kind), prefix_rows(prefix_rows) {
		// The joins above size their hash tables by this estimate: keep the child's.
		if (child->has_estimated_cardinality) {
			SetEstimatedCardinality(child->estimated_cardinality);
		}
		children.push_back(std::move(child));
	}
	string key;
	// A query result's order may matter; a subtree's does not.
	waddle::ResultKind kind;
	// Captures the first rows of an ordered result, this many of them; 0 otherwise.
	idx_t prefix_rows;

	vector<ColumnBinding> GetColumnBindings() override {
		return children[0]->GetColumnBindings();
	}
	string GetName() const override {
		return "WADDLE_CAPTURE";
	}
	// Opts out of plan verification's serialization round trip; see ADR-0003.
	bool SupportSerialization() const override {
		return false;
	}
	void Serialize(Serializer &serializer) const override {
		throw NotImplementedException("WADDLE_CAPTURE cannot be copied");
	}
	PhysicalOperator &CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) override;

protected:
	void ResolveTypes() override {
		types = children[0]->types;
	}
};

//===--------------------------------------------------------------------===//
// Logical block operator: marks a block, and captures it once its family repeats
//===--------------------------------------------------------------------===//
// Passes its child's rows through, with only the columns the statement reads. `expressions` reference the block
// columns: first the `read_count` columns the statement reads, which are the operator's own, then those only a
// capture keeps, in the order of `columns`. A pipeline breaker above describes the block by its key and the columns
// it reads, so everything above the block is the same whether the block is joined, captured or replayed.
class LogicalBlock : public LogicalExtensionOperator {
public:
	LogicalBlock(unique_ptr<LogicalOperator> child, string key, vector<BlockColumn> columns, idx_t read_count,
	             bool capture)
	    : key(std::move(key)), columns(std::move(columns)), read_count(read_count), capture(capture) {
		children.push_back(std::move(child));
	}
	string key;
	vector<BlockColumn> columns;
	idx_t read_count;
	bool capture;
	// Instead of capturing, count the block's rows, under its base and ranges (see PhysicalBlockMeasure).
	bool measure = false;
	string measure_base;
	BlockRanges measure_ranges;
	// The optimizers moved a predicate from outside the block into it (see MarkBlocksFilteredFromOutside): its rows
	// are then fewer than its key describes.
	bool filtered_from_outside = false;
	vector<ColumnBinding> resolved_bindings;

	vector<ColumnBinding> ReadBindings() {
		// Once the column binding resolver has made the expressions references, the bindings they had.
		if (read_count && expressions[0]->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return resolved_bindings;
		}
		vector<ColumnBinding> result;
		for (idx_t i = 0; i < read_count; i++) {
			result.push_back(expressions[i]->Cast<BoundColumnRefExpression>().binding);
		}
		return result;
	}
	// Only the columns the statement reads, in block column order: a recycling key refers to them by position.
	vector<ColumnBinding> GetColumnBindings() override {
		return ReadBindings();
	}
	void ResolveColumnBindings(ColumnBindingResolver &resolver, vector<ColumnBinding> &bindings) override {
		resolved_bindings = ReadBindings();
		LogicalExtensionOperator::ResolveColumnBindings(resolver, bindings);
	}
	string GetName() const override {
		return "WADDLE_BLOCK";
	}
	// Opts out of plan verification's serialization round trip; see ADR-0003.
	bool SupportSerialization() const override {
		return false;
	}
	// Optimizers that copy a plan, such as CTE inlining, leave it as it is when a copy is not implemented.
	void Serialize(Serializer &serializer) const override {
		throw NotImplementedException("WADDLE_BLOCK cannot be copied");
	}
	PhysicalOperator &CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) override;

protected:
	void ResolveTypes() override {
		types.clear();
		for (idx_t i = 0; i < read_count; i++) {
			types.push_back(expressions[i]->return_type);
		}
	}
};

// Whether `op` is a WADDLE_BLOCK marker.
bool IsLogicalBlock(const LogicalOperator &op);
// A scan that replays the payload's rows, as columns of these types, under a new table index of `binder`.
unique_ptr<LogicalGet> MakeReplay(Binder &binder, const CollectionPayload &payload, const vector<LogicalType> &types);
// Swaps `op` for a replay scan of the payload, and appends the old -> new binding of each column. The caller applies
// them with ReplaceBindings once it has replaced every subtree. `op`'s types must be resolved.
void ReplaceWithReplay(Binder &binder, unique_ptr<LogicalOperator> &op, const CollectionPayload &payload,
                       vector<ReplacementBinding> &replacements);
// Points the plan's references to replaced columns at the columns of their replays. Nothing to do if there are
// no replacements.
void ReplaceBindings(LogicalOperator &plan, vector<ReplacementBinding> replacements);
// Puts a WADDLE_CAPTURE of the subtree in `slot` in its place, which stores the subtree's rows under `key` as a
// recycled result of `kind`. `prefix_rows`: see LogicalCapture.
void WrapInCapture(unique_ptr<LogicalOperator> &slot, string key, waddle::ResultKind kind, idx_t prefix_rows = 0);

} // namespace recycling
} // namespace duckdb
