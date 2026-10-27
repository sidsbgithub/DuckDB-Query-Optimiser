// The two plan rewrites before DuckDB's optimizers (ADR-0006).

#include "plan_rewrites.hpp"

#include "plan_keys.hpp"

#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_delim_get.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

#include <algorithm>

namespace duckdb {

namespace recycling {

// The input column that the projection outputs, unchanged, as `output`, if it does.
static bool ProjectedColumn(const LogicalProjection &projection, ColumnBinding output, ColumnBinding &input) {
	if (output.table_index != projection.table_index || output.column_index >= projection.expressions.size()) {
		return false;
	}
	auto &expression = *projection.expressions[output.column_index];
	if (expression.type != ExpressionType::BOUND_COLUMN_REF) {
		return false;
	}
	input = expression.Cast<BoundColumnRefExpression>().binding;
	return true;
}

// Whether equality on these output columns selects at most one row. Follow only operators that preserve
// uniqueness, and require every grouping key (not just a subset) at an aggregate.
static bool UniqueOn(LogicalOperator &op, const vector<ColumnBinding> &columns) {
	if (op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &projection = op.Cast<LogicalProjection>();
		vector<ColumnBinding> input_columns;
		for (auto &column : columns) {
			ColumnBinding input;
			if (ProjectedColumn(projection, column, input)) {
				input_columns.push_back(input);
			}
		}
		return UniqueOn(*op.children[0], input_columns);
	}
	if (op.type == LogicalOperatorType::LOGICAL_FILTER) {
		return UniqueOn(*op.children[0], columns);
	}
	if (op.type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		return false;
	}
	auto &aggregate = op.Cast<LogicalAggregate>();
	if (aggregate.grouping_sets.size() > 1 || !aggregate.grouping_functions.empty()) {
		return false;
	}
	for (idx_t i = 0; i < aggregate.groups.size(); i++) {
		auto binding = ColumnBinding(aggregate.group_index, i);
		if (std::find(columns.begin(), columns.end(), binding) == columns.end()) {
			return false;
		}
	}
	return true;
}

// Whether the subtree evaluates an expression that can throw, given the columns of it that are read. Like DuckDB's
// removal of unused columns, a projection or aggregate evaluates only the expressions whose columns are read.
static bool EvaluatesThrowingExpression(LogicalOperator &op, const column_binding_set_t &read) {
	vector<reference<Expression>> evaluated;
	if (op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &projection = op.Cast<LogicalProjection>();
		for (idx_t i = 0; i < projection.expressions.size(); i++) {
			if (read.count(ColumnBinding(projection.table_index, i))) {
				evaluated.push_back(*projection.expressions[i]);
			}
		}
	} else if (op.type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		auto &aggregate = op.Cast<LogicalAggregate>();
		for (auto &group : aggregate.groups) {
			evaluated.push_back(*group);
		}
		for (idx_t i = 0; i < aggregate.expressions.size(); i++) {
			if (read.count(ColumnBinding(aggregate.aggregate_index, i))) {
				evaluated.push_back(*aggregate.expressions[i]);
			}
		}
	} else {
		LogicalOperatorVisitor::EnumerateExpressions(
		    op, [&](unique_ptr<Expression> *expression) { evaluated.push_back(**expression); });
	}
	// A projection or aggregate makes columns of its own; any other operator passes its children's through.
	bool makes_columns = op.type == LogicalOperatorType::LOGICAL_PROJECTION ||
	                     op.type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY;
	column_binding_set_t child_read = makes_columns ? column_binding_set_t() : read;
	for (auto &expression : evaluated) {
		if (expression.get().CanThrow()) {
			return true;
		}
		CollectColumns(expression.get(), child_read);
	}
	for (auto &child : op.children) {
		if (EvaluatesThrowingExpression(*child, child_read)) {
			return true;
		}
	}
	return false;
}

//===--------------------------------------------------------------------===//
// Unused LEFT JOINs
//===--------------------------------------------------------------------===//
// Whether the LEFT JOIN `op` can be removed: nothing reads its right side (`referenced` holds the columns read above
// it), its right side has at most one row for each left row, and removing it suppresses no error.
static bool IsRemovableLeftJoin(LogicalOperator &op, const column_binding_set_t &referenced) {
	auto &join = op.Cast<LogicalComparisonJoin>();
	auto &right = *op.children[1];
	// Eliminating the join must not suppress an error from evaluating its discarded side, which DuckDB evaluates as
	// far as the join conditions read it.
	column_binding_set_t condition_columns;
	for (auto &condition : join.conditions) {
		CollectColumns(*condition.right, condition_columns);
	}
	if (join.join_type != JoinType::LEFT || join.predicate || !IsRecyclableQuery(right) ||
	    EvaluatesThrowingExpression(right, condition_columns)) {
		return false;
	}
	for (auto &binding : right.GetColumnBindings()) {
		if (referenced.count(binding)) {
			return false;
		}
	}
	vector<ColumnBinding> equal_columns;
	for (auto &condition : join.conditions) {
		if (condition.left->CanThrow() || condition.right->CanThrow()) {
			return false;
		}
		if ((condition.comparison == ExpressionType::COMPARE_EQUAL ||
		     condition.comparison == ExpressionType::COMPARE_NOT_DISTINCT_FROM) &&
		    condition.right->type == ExpressionType::BOUND_COLUMN_REF) {
			equal_columns.push_back(condition.right->Cast<BoundColumnRefExpression>().binding);
		}
	}
	return UniqueOn(right, equal_columns);
}

// An unused LEFT JOIN with at most one matching right row preserves exactly the left multiset. Running before
// optimization also removes its work from the join-order search and from subsequent recycling keys.
void RemoveUnusedLeftJoins(unique_ptr<LogicalOperator> &op, column_binding_set_t referenced) {
	if (op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN && IsRemovableLeftJoin(*op, referenced)) {
		op = std::move(op->children[0]);
		RemoveUnusedLeftJoins(op, std::move(referenced));
		return;
	}
	LogicalOperatorVisitor::EnumerateExpressions(
	    *op, [&](unique_ptr<Expression> *expression) { CollectColumns(**expression, referenced); });
	for (auto &child : op->children) {
		RemoveUnusedLeftJoins(child, referenced);
	}
}

//===--------------------------------------------------------------------===//
// DELIM joins of simple EXISTS
//===--------------------------------------------------------------------===//
static bool HasDelimJoin(const LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_DELIM_JOIN) {
		return true;
	}
	for (auto &child : op.children) {
		if (HasDelimJoin(*child)) {
			return true;
		}
	}
	return false;
}

// Whether the outer join compares exactly the keys that feed `delim`. Traces the outer join's RHS columns from `rhs`
// down through projections to `inner`; a transformed key or any other correlation shape is left to DuckDB.
static bool ComparesDelimKeys(const LogicalComparisonJoin &outer, LogicalOperator &rhs, const LogicalOperator &inner,
                              const LogicalDelimGet &delim) {
	std::unordered_set<idx_t> outer_keys;
	for (auto &condition : outer.conditions) {
		if (condition.comparison != ExpressionType::COMPARE_NOT_DISTINCT_FROM ||
		    condition.right->type != ExpressionType::BOUND_COLUMN_REF) {
			return false;
		}
		auto binding = condition.right->Cast<BoundColumnRefExpression>().binding;
		auto current = &rhs;
		while (current != &inner) {
			if (current->type == LogicalOperatorType::LOGICAL_PROJECTION &&
			    !ProjectedColumn(current->Cast<LogicalProjection>(), binding, binding)) {
				return false;
			}
			current = current->children[0].get();
		}
		if (binding.table_index != delim.table_index || binding.column_index >= delim.chunk_types.size() ||
		    !outer_keys.insert(binding.column_index).second ||
		    !condition.left->Equals(*outer.duplicate_eliminated_columns[binding.column_index])) {
			return false;
		}
	}
	return true;
}

// Whether every condition of the inner join equates a distinct column of `delim` with a column of the relation it
// joins. If so, maps each such DELIM_GET column to that column in `replacer`, and adds an IS NOT NULL filter on the
// column for each `=`, since only IS NOT DISTINCT FROM matches NULL keys.
static bool MatchesDelimColumns(const LogicalComparisonJoin &inner, idx_t delim_side, const LogicalDelimGet &delim,
                                ColumnBindingReplacer &replacer, vector<unique_ptr<Expression>> &null_filters) {
	std::unordered_set<idx_t> matched;
	for (auto &condition : inner.conditions) {
		auto &key = delim_side == 0 ? *condition.left : *condition.right;
		auto &value = delim_side == 0 ? *condition.right : *condition.left;
		if ((condition.comparison != ExpressionType::COMPARE_EQUAL &&
		     condition.comparison != ExpressionType::COMPARE_NOT_DISTINCT_FROM) ||
		    key.type != ExpressionType::BOUND_COLUMN_REF || value.type != ExpressionType::BOUND_COLUMN_REF) {
			return false;
		}
		auto binding = key.Cast<BoundColumnRefExpression>().binding;
		if (binding.table_index != delim.table_index || !matched.insert(binding.column_index).second) {
			return false;
		}
		replacer.replacement_bindings.emplace_back(binding, value.Cast<BoundColumnRefExpression>().binding);
		if (condition.comparison == ExpressionType::COMPARE_EQUAL) {
			auto filter =
			    make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, LogicalType::BOOLEAN);
			filter->children.push_back(value.Copy());
			null_filters.push_back(std::move(filter));
		}
	}
	return true;
}

// In a simple EXISTS, the RHS joins the distinct outer keys back to an independent relation on every key.
// The enclosing semi/anti join already tests those keys, so the inner join to DELIM_GET is redundant. Unlike
// DuckDB's general deliminator, do not retain it merely because the independent relation has a filter: that
// barrier prevents the selective existence check from participating in join ordering and recycling.
static void RemoveExistenceDelimJoin(unique_ptr<LogicalOperator> &op) {
	if (op->type != LogicalOperatorType::LOGICAL_DELIM_JOIN) {
		return;
	}
	auto &outer = op->Cast<LogicalComparisonJoin>();
	if (outer.delim_flipped || outer.predicate ||
	    (outer.join_type != JoinType::SEMI && outer.join_type != JoinType::ANTI)) {
		return;
	}
	// The inner join: below the projections and filters of the RHS, an INNER join with a DELIM_GET on one side.
	auto slot = &op->children[1];
	while ((*slot)->type == LogicalOperatorType::LOGICAL_PROJECTION ||
	       (*slot)->type == LogicalOperatorType::LOGICAL_FILTER) {
		slot = &(*slot)->children[0];
	}
	if ((*slot)->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return;
	}
	auto &inner = (*slot)->Cast<LogicalComparisonJoin>();
	if (inner.join_type != JoinType::INNER || inner.predicate) {
		return;
	}
	idx_t delim_side;
	if (inner.children[0]->type == LogicalOperatorType::LOGICAL_DELIM_GET) {
		delim_side = 0;
	} else if (inner.children[1]->type == LogicalOperatorType::LOGICAL_DELIM_GET) {
		delim_side = 1;
	} else {
		return;
	}
	auto &delim = inner.children[delim_side]->Cast<LogicalDelimGet>();
	auto &relation = inner.children[1 - delim_side];
	auto key_count = delim.chunk_types.size();
	if (!IsRecyclableQuery(*relation) || inner.conditions.size() != key_count || outer.conditions.size() != key_count ||
	    outer.duplicate_eliminated_columns.size() != key_count) {
		return;
	}
	ColumnBindingReplacer replacer;
	vector<unique_ptr<Expression>> null_filters;
	if (!ComparesDelimKeys(outer, *op->children[1], inner, delim) ||
	    !MatchesDelimColumns(inner, delim_side, delim, replacer, null_filters)) {
		return;
	}
	auto replacement = std::move(relation);
	if (!null_filters.empty()) {
		auto filter = make_uniq<LogicalFilter>();
		filter->expressions = std::move(null_filters);
		filter->children.push_back(std::move(replacement));
		replacement = std::move(filter);
	}
	*slot = std::move(replacement);
	replacer.VisitOperator(*op);
	outer.type = LogicalOperatorType::LOGICAL_COMPARISON_JOIN;
	outer.duplicate_eliminated_columns.clear();
	outer.mark_types.clear();
}

// Innermost first.
static void RemoveExistenceDelimJoinsBelow(unique_ptr<LogicalOperator> &op) {
	for (auto &child : op->children) {
		RemoveExistenceDelimJoinsBelow(child);
	}
	RemoveExistenceDelimJoin(op);
}

void RemoveExistenceDelimJoins(Optimizer &optimizer, unique_ptr<LogicalOperator> &plan) {
	if (!HasDelimJoin(*plan)) {
		return;
	}
	FilterPushdown pushdown(optimizer);
	unordered_set<idx_t> bindings;
	pushdown.CheckMarkToSemi(*plan, bindings);
	plan = pushdown.Rewrite(std::move(plan));
	RemoveExistenceDelimJoinsBelow(plan);
}

} // namespace recycling
} // namespace duckdb
