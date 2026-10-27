// Recycling of the subtrees rooted at pipeline breakers, after DuckDB's optimizers (ADR-0002): a subtree's
// recycling key, which describes it as a waddle::KeyNode (recycling_key.cpp encodes it), and the pass that replays
// or captures each recyclable subtree.

#include "pipeline_breakers.hpp"

#include "operators.hpp"
#include "plan_keys.hpp"
#include "recycling_key.hpp"
#include "recycling_state.hpp"
#include "recycling_settings.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/optional_filter.hpp"
#include "duckdb/planner/filter/struct_filter.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_distinct.hpp"
#include "duckdb/planner/operator/logical_explain.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/storage/storage_info.hpp"

namespace duckdb {

namespace recycling {

//===--------------------------------------------------------------------===//
// Recycling keys: a subtree described as a waddle::KeyNode
//===--------------------------------------------------------------------===//
static waddle::KeyBinding KeyBindingFor(const ColumnBinding &binding) {
	waddle::KeyBinding result;
	result.table = binding.table_index;
	result.column = binding.column_index;
	return result;
}

// Why a subtree is not recyclable: the counted reason, plus detail for diagnostics.
struct Skip {
	waddle::SkipReason reason = waddle::SkipReason::UNSUPPORTED;
	string detail;
};

// Replaces every column reference with the same placeholder, keeping its type.
static void ReplaceColumnsWithPlaceholder(unique_ptr<Expression> &expression) {
	if (expression->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		expression = make_uniq<BoundReferenceExpression>(expression->return_type, 0);
		return;
	}
	ExpressionIterator::EnumerateChildren(*expression,
	                                      [](unique_ptr<Expression> &child) { ReplaceColumnsWithPlaceholder(child); });
}

// The expression's serialization, without its column bindings, whose table indices depend on the rest of the query:
// the recycling key encodes the columns separately, as children. Serialization describes the expression completely,
// including function bind data that its text leaves out. Empty if it cannot be serialized.
static string BindingFreeKey(const Expression &expression) {
	auto copy = expression.Copy();
	ReplaceColumnsWithPlaceholder(copy);
	// An alias names a column; it does not change what the expression computes.
	ExpressionIterator::EnumerateExpression(copy, [](Expression &child) { child.alias.clear(); });
	std::unordered_set<idx_t> no_tables;
	RecyclingKeySerializer serializer(no_tables);
	if (!TrySerialize(serializer, *copy)) {
		return string();
	}
	return serializer.key;
}

// A scan can ignore join-generated filters only if every producer is inside the subtree.
using DynamicFilterSources = std::unordered_map<const DynamicTableFilterSet *, size_t>;
static void CollectDynamicFilterSources(const LogicalOperator &op, DynamicFilterSources &sources) {
	if (op.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		auto &join = op.Cast<LogicalComparisonJoin>();
		if (join.filter_pushdown) {
			for (auto &probe : join.filter_pushdown->probe_info) {
				sources[probe.dynamic_filters.get()]++;
			}
		}
	}
	for (auto &child : op.children) {
		CollectDynamicFilterSources(*child, sources);
	}
}

// Describes one subtree as a waddle::KeyNode. The first check that fails says why the subtree is not recyclable
// (`skip`), and the description stops there.
class SubtreeDescriber {
public:
	// `plan_sources` counts the dynamic filter producers of the whole plan that `subtree` is part of.
	SubtreeDescriber(const DynamicFilterSources &plan_sources, const LogicalOperator &subtree) {
		DynamicFilterSources sources;
		CollectDynamicFilterSources(subtree, sources);
		for (auto &source : sources) {
			auto plan_source = plan_sources.find(source.first);
			if (plan_source != plan_sources.end() && plan_source->second == source.second) {
				internal_filters.insert(source.first);
			}
		}
	}

	// Why the subtree is not recyclable, once Describe has returned false.
	Skip skip;
	// The scans whose join filters come from a join outside the subtree, which would drop rows from its result.
	vector<const LogicalGet *> external_filter_scans;

	bool Describe(const LogicalOperator &op, waddle::KeyNode &result) {
		result.kind = static_cast<int>(op.type);
		if (IsLogicalBlock(op)) {
			return DescribeBlock(op.Cast<LogicalBlock>(), result);
		}
		bool described;
		switch (op.type) {
		case LogicalOperatorType::LOGICAL_GET:
			described = DescribeScan(op.Cast<LogicalGet>(), result);
			break;
		case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
			described = DescribeJoin(op, result);
			break;
		case LogicalOperatorType::LOGICAL_FILTER:
		case LogicalOperatorType::LOGICAL_PROJECTION:
		case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
		case LogicalOperatorType::LOGICAL_DISTINCT:
			described = DescribeSingleChild(op, result);
			break;
		case LogicalOperatorType::LOGICAL_CTE_REF:
			return Reject(waddle::SkipReason::CTE_REFERENCE, op.GetName());
		case LogicalOperatorType::LOGICAL_DELIM_GET:
			return Reject(waddle::SkipReason::DELIM_GET, op.GetName());
		default:
			return Reject(waddle::SkipReason::UNSUPPORTED, "operator " + op.GetName());
		}
		if (!described) {
			return false;
		}
		for (auto &type : op.types) {
			result.output_types.push_back(type.ToString());
		}
		return true;
	}

private:
	// The dynamic filters whose every producer is inside the subtree.
	std::unordered_set<const DynamicTableFilterSet *> internal_filters;

	bool Reject(waddle::SkipReason reason, const string &detail) {
		skip.reason = reason;
		skip.detail = detail;
		return false;
	}

	// Volatile functions and unbound parameters make a result depend on when and how it is executed.
	bool IsContextFree(const Expression &expression) {
		if (expression.IsVolatile()) {
			return Reject(waddle::SkipReason::VOLATILE, expression.ToString());
		}
		if (expression.HasParameter()) {
			return Reject(waddle::SkipReason::PARAMETER, expression.ToString());
		}
		return true;
	}

	// Top-N can install a mutable filter below an aggregate. Its current threshold is
	// outside the subtree and cannot be represented by a stable recycling key.
	// A pushed-down predicate keeps its expression, including unbound prepared-statement parameters.
	bool IsRecyclableFilter(const TableFilter &filter) {
		switch (filter.filter_type) {
		case TableFilterType::DYNAMIC_FILTER:
		case TableFilterType::BLOOM_FILTER:
			return Reject(waddle::SkipReason::UNSUPPORTED, "mutable scan filter");
		case TableFilterType::EXPRESSION_FILTER:
			return IsContextFree(*filter.Cast<ExpressionFilter>().expr);
		case TableFilterType::OPTIONAL_FILTER: {
			auto &child = filter.Cast<OptionalFilter>().child_filter;
			return !child || IsRecyclableFilter(*child);
		}
		case TableFilterType::STRUCT_EXTRACT:
			return IsRecyclableFilter(*filter.Cast<StructFilter>().child_filter);
		case TableFilterType::CONJUNCTION_AND:
		case TableFilterType::CONJUNCTION_OR:
			for (auto &child : static_cast<const ConjunctionFilter &>(filter).child_filters) {
				if (!IsRecyclableFilter(*child)) {
					return false;
				}
			}
			return true;
		default:
			return true;
		}
	}

	bool DescribeExpression(const Expression &expression, waddle::KeyExpression &result) {
		if (!IsContextFree(expression)) {
			return false;
		}
		if (expression.HasSubquery()) {
			return Reject(waddle::SkipReason::UNSUPPORTED, "subquery " + expression.ToString());
		}
		result.kind = static_cast<int>(expression.GetExpressionType());
		result.type = expression.return_type.ToString();
		if (expression.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			auto &column = expression.Cast<BoundColumnRefExpression>();
			if (column.depth != 0) {
				return Reject(waddle::SkipReason::UNSUPPORTED, "correlated column " + column.ToString());
			}
			result.is_column_reference = true;
			result.binding = KeyBindingFor(column.binding);
		} else if (expression.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			result.value = expression.Cast<BoundConstantExpression>().value.ToSQLString();
		} else {
			result.value = BindingFreeKey(expression);
			if (result.value.empty()) {
				return Reject(waddle::SkipReason::UNSUPPORTED, "unserializable " + expression.ToString());
			}
		}
		bool eligible = true;
		ExpressionIterator::EnumerateChildren(expression, [&](const Expression &child) {
			result.children.emplace_back();
			if (eligible && !DescribeExpression(child, result.children.back())) {
				eligible = false;
			}
		});
		return eligible;
	}

	// Describes each expression as the next element of `result`.
	bool DescribeExpressions(const vector<unique_ptr<Expression>> &expressions,
	                         std::vector<waddle::KeyExpression> &result) {
		for (auto &expression : expressions) {
			result.emplace_back();
			if (!DescribeExpression(*expression, result.back())) {
				return false;
			}
		}
		return true;
	}

	// Describes `child` as the next child of `result`, whose expressions refer to the child's columns by position.
	bool DescribeChild(LogicalOperator &child, waddle::KeyNode &result) {
		result.children.emplace_back(new waddle::KeyNode());
		if (!Describe(child, *result.children.back())) {
			return false;
		}
		for (auto &binding : child.GetColumnBindings()) {
			result.input_bindings.push_back(KeyBindingFor(binding));
		}
		return true;
	}

	// A block: its rows are those of its key, whether it is joined, captured or replayed.
	bool DescribeBlock(const LogicalBlock &block, waddle::KeyNode &result) {
		if (block.filtered_from_outside) {
			return Reject(waddle::SkipReason::UNSUPPORTED, "block filtered from outside");
		}
		result.scan_source = "block:" + block.key;
		for (idx_t i = 0; i < block.read_count; i++) {
			result.scan_columns.push_back(block.columns[i].first);
			result.scan_columns.push_back(block.columns[i].second);
			result.output_types.push_back(block.expressions[i]->return_type.ToString());
		}
		return true;
	}

	bool DescribeScan(const LogicalGet &scan, waddle::KeyNode &result) {
		// Only DuckDB's own tables change solely through commits that invalidate the cache: a table in an attached
		// Postgres or SQLite catalog can change underneath it.
		auto table = scan.GetTable();
		if (!table || !table->IsDuckTable()) {
			return Reject(waddle::SkipReason::EXTERNAL_SCAN, scan.function.name);
		}
		if (table->temporary) {
			return Reject(waddle::SkipReason::TEMP_OBJECT, table->name);
		}
		if (scan.dynamic_filters && !internal_filters.count(scan.dynamic_filters.get())) {
			external_filter_scans.push_back(&scan);
		}
		for (auto &filter : scan.table_filters.filters) {
			if (!IsRecyclableFilter(*filter.second)) {
				return false;
			}
		}
		if (!scan.children.empty() || !scan.projected_input.empty() || scan.ordinality_idx.IsValid() ||
		    scan.extra_info.sample_options || scan.row_group_order_options) {
			return Reject(waddle::SkipReason::UNSUPPORTED, "scan option on " + table->name);
		}
		result.scan_source = scan.function.name + ":" + to_string(reinterpret_cast<uintptr_t>(table.get()));
		for (auto &column : scan.GetColumnIds()) {
			if (!column.HasPrimaryIndex() || column.HasChildren()) {
				return Reject(waddle::SkipReason::UNSUPPORTED, "nested column scan on " + table->name);
			}
			result.scan_columns.push_back(column.GetPrimaryIndex());
		}
		for (auto projection : scan.projection_ids) {
			result.scan_projection.push_back(projection);
		}
		for (auto &filter : scan.table_filters.filters) {
			result.scan_filters.emplace_back(filter.first, filter.second->ToString(""));
		}
		return true;
	}

	bool DescribeJoin(const LogicalOperator &op, waddle::KeyNode &result) {
		auto &join = op.Cast<LogicalComparisonJoin>();
		if (op.children.size() != 2 || join.predicate || !join.mark_types.empty() ||
		    !join.duplicate_eliminated_columns.empty() || join.delim_flipped) {
			return Reject(waddle::SkipReason::UNSUPPORTED, "join form " + op.GetName());
		}
		result.join_kind = static_cast<int>(join.join_type);
		for (auto projection : join.left_projection_map) {
			result.left_projection.push_back(projection);
		}
		for (auto projection : join.right_projection_map) {
			result.right_projection.push_back(projection);
		}
		for (auto &child : op.children) {
			if (!DescribeChild(*child, result)) {
				return false;
			}
		}
		for (auto &condition : join.conditions) {
			result.join_conditions.emplace_back();
			auto &key_condition = result.join_conditions.back();
			key_condition.comparison = static_cast<int>(condition.comparison);
			if (!DescribeExpression(*condition.left, key_condition.left) ||
			    !DescribeExpression(*condition.right, key_condition.right)) {
				return false;
			}
		}
		return true;
	}

	// A filter, projection, aggregate or DISTINCT.
	bool DescribeSingleChild(const LogicalOperator &op, waddle::KeyNode &result) {
		if (op.children.size() != 1) {
			return Reject(waddle::SkipReason::UNSUPPORTED, "child count of " + op.GetName());
		}
		if (!DescribeChild(*op.children[0], result)) {
			return false;
		}
		if (op.type == LogicalOperatorType::LOGICAL_FILTER) {
			for (auto projection : op.Cast<LogicalFilter>().projection_map) {
				result.filter_projection.push_back(projection);
			}
		} else if (op.type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
			if (!DescribeGrouping(op.Cast<LogicalAggregate>(), result)) {
				return false;
			}
		} else if (op.type == LogicalOperatorType::LOGICAL_DISTINCT) {
			auto &distinct = op.Cast<LogicalDistinct>();
			// DISTINCT ON keeps the first row of each group in ORDER BY order: order-sensitive.
			if (distinct.distinct_type != DistinctType::DISTINCT || distinct.order_by) {
				return Reject(waddle::SkipReason::UNSUPPORTED, "order-sensitive DISTINCT ON");
			}
			// DISTINCT groups its input by its targets.
			if (!DescribeExpressions(distinct.distinct_targets, result.groups)) {
				return false;
			}
		}
		return DescribeExpressions(op.expressions, result.expressions);
	}

	bool DescribeGrouping(const LogicalAggregate &aggregate, waddle::KeyNode &result) {
		if (aggregate.grouping_sets.size() > 1 || !aggregate.grouping_functions.empty()) {
			return Reject(waddle::SkipReason::UNSUPPORTED, "grouping sets");
		}
		result.distinct_validity = static_cast<int>(aggregate.distinct_validity);
		if (!aggregate.grouping_sets.empty()) {
			for (auto group_index : aggregate.grouping_sets[0]) {
				result.grouping_set.push_back(group_index);
			}
		}
		return DescribeExpressions(aggregate.groups, result.groups);
	}
};

// A subtree's recycling key, or why it has none.
struct SubtreeKey {
	bool recyclable = false;
	string key;
	// See SubtreeDescriber.
	vector<const LogicalGet *> external_filter_scans;
	Skip skip;
};

// Builds the recycling key of a subtree. `plan_sources` counts the dynamic filter producers of the whole plan.
static SubtreeKey BuildRecyclingKey(const DynamicFilterSources &plan_sources, const LogicalOperator &op) {
	SubtreeKey result;
	SubtreeDescriber describer(plan_sources, op);
	waddle::KeyNode description;
	if (!describer.Describe(op, description)) {
		result.skip = describer.skip;
	} else if (!waddle::EncodeRecyclingKey(description, result.key)) {
		result.skip.reason = waddle::SkipReason::UNSUPPORTED;
		result.skip.detail = "column bound outside the subtree";
	} else {
		result.recyclable = true;
	}
	result.external_filter_scans = std::move(describer.external_filter_scans);
	return result;
}

//===--------------------------------------------------------------------===//
// Replaying and capturing pipeline breakers
//===--------------------------------------------------------------------===//
// The pipeline breakers that may root a recyclable subtree.
static bool IsRecyclableRoot(const LogicalOperator &op) {
#ifndef WADDLE_ABLATE_AGGREGATE
	if (op.type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		return true;
	}
#endif
#ifndef WADDLE_ABLATE_DISTINCT
	if (op.type == LogicalOperatorType::LOGICAL_DISTINCT) {
		return true;
	}
#endif
	return false;
}

static bool IsRecyclableCTEDefinition(const LogicalOperator &op) {
#ifdef WADDLE_ABLATE_CTE
	return false;
#else
	return op.type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE;
#endif
}

// DuckDB builds the right input of an equality comparison join into a hash table unless it prefers
// an IE join for multiple range conditions. See PhysicalPlanGenerator::PlanComparisonJoin.
static bool HasHashJoinBuildSide(ClientContext &context, const LogicalOperator &op) {
#ifdef WADDLE_ABLATE_JOIN_BUILD
	return false;
#else
	if (op.type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN || op.children.size() != 2) {
		return false;
	}
	idx_t range_count = 0;
	auto &join = op.Cast<LogicalComparisonJoin>();
	if (!join.HasEquality(range_count)) {
		return false;
	}
	if (range_count < 2 || join.join_type == JoinType::SEMI || join.join_type == JoinType::ANTI ||
	    join.join_type == JoinType::RIGHT_SEMI || join.join_type == JoinType::RIGHT_ANTI ||
	    join.join_type == JoinType::MARK) {
		return true;
	}
	return !Settings::Get<PreferRangeJoinsSetting>(context);
#endif
}

// The breaker kind that diagnostics name for a recyclable root.
static const char *BreakerName(const LogicalOperator &op) {
	return op.type == LogicalOperatorType::LOGICAL_DISTINCT ? "distinct" : "aggregate";
}

static bool HasRecyclableRoot(ClientContext &context, const LogicalOperator &op) {
	if (IsRecyclableRoot(op) || IsRecyclableCTEDefinition(op) || HasHashJoinBuildSide(context, op)) {
		return true;
	}
	for (auto &child : op.children) {
		if (HasRecyclableRoot(context, *child)) {
			return true;
		}
	}
	return false;
}

// What the pass over one plan carries from subtree to subtree.
struct HookContext {
	HookContext(ClientContext &context, Binder &binder)
	    : context(context), binder(binder), diagnostics(DiagnosticsEnabled(context)) {
	}
	ClientContext &context;
	Binder &binder;
	bool diagnostics;
	DynamicFilterSources dynamic_filter_sources;
	vector<ReplacementBinding> replacements;
	// Inside a block that is captured whole: its estimated rows.
	optional_idx captured_block_rows;
};

// Writes one recycling event to DuckDB's log (see `CALL enable_logging('WaddleRecycling')`).
static void Diagnose(HookContext &hook, const string &message) {
	if (hook.diagnostics) {
		DUCKDB_LOG_INTERNAL(hook.context, RecyclingLogType::NAME, RecyclingLogType::LEVEL, message);
	}
}

static void RecordSkip(HookContext &hook, const char *breaker, const Skip &skip) {
	Cache(hook.context).RecordSkip(skip.reason);
	Diagnose(hook, string("skip reason=") + waddle::SkipReasonName(skip.reason) + " breaker=" + breaker +
	                   " detail=" + skip.detail);
}

// Replays the recycled result on a hit. Returns false on a miss.
static bool TryReplay(HookContext &hook, unique_ptr<LogicalOperator> &op, const string &key, const char *breaker) {
	auto payload = LookupResult(hook.context, key, waddle::ResultKind::SUBTREE);
	Diagnose(hook, string(payload ? "hit" : "miss") + " breaker=" + breaker + " key=" + key);
	if (!payload) {
		return false;
	}
	ReplaceWithReplay(hook.binder, op, *payload, hook.replacements);
	return true;
}

// The breaker kind a parent makes of its child: a join's build side, or a shared CTE definition, which DuckDB
// materializes before evaluating its references. Null if the parent makes it no recyclable subtree.
static const char *ChildBreakerName(ClientContext &context, const LogicalOperator &parent, idx_t child_index) {
	if (child_index == 0 && IsRecyclableCTEDefinition(parent)) {
		return "cte_definition";
	}
	if (child_index == 1 && HasHashJoinBuildSide(context, parent)) {
		return "join_build_side";
	}
	return nullptr;
}

// Whether a scan reads a whole table with at least a row group per thread. It already runs on every thread, so a
// replay of its rows would save nothing, and its capture would copy the whole table.
static bool IsWholeLargeTableScan(ClientContext &context, LogicalGet &scan) {
	if (!scan.table_filters.filters.empty() || !scan.function.cardinality) {
		return false;
	}
	auto rows = scan.function.cardinality(context, scan.bind_data.get());
	auto threads = NumericCast<idx_t>(TaskScheduler::GetScheduler(context).NumberOfThreads());
	return rows && rows->has_estimated_cardinality && rows->estimated_cardinality >= threads * DEFAULT_ROW_GROUP_SIZE;
}

// Whether to capture a recyclable subtree that missed, under `key`.
static bool ShouldCapture(HookContext &hook, const LogicalOperator &op, const string &key, const char *breaker) {
	// A block captured whole holds the rows of a subtree nearly as large as itself: capturing both costs twice.
	if (hook.captured_block_rows.IsValid() &&
	    MaxValue<idx_t>(op.estimated_cardinality, 1) * 4 >= hook.captured_block_rows.GetIndex()) {
		return false;
	}
	// A capture refused for its size before would copy every row again only to be refused again.
	if (Cache(hook.context).KnownTooLarge(key, RecyclingBudget(hook.context))) {
		Diagnose(hook, string("too_large breaker=") + breaker + " key=" + key);
		return false;
	}
	return true;
}

static bool RecycleSubtrees(HookContext &hook, unique_ptr<LogicalOperator> &op, const char *breaker);

// Recycles the subtrees inside a block that is captured whole, knowing its estimated rows.
static bool RecycleInsideCapturedBlock(HookContext &hook, LogicalOperator &block) {
	auto outer = hook.captured_block_rows;
	hook.captured_block_rows = MaxValue<idx_t>(block.children[0]->estimated_cardinality, 1);
	bool contains_skip = RecycleSubtrees(hook, block.children[0], nullptr);
	hook.captured_block_rows = outer;
	return contains_skip;
}

// Recycles every recyclable subtree, outermost first. A hit replays the whole subtree, so nothing inside it is
// looked up. A miss looks inside for smaller hits, then captures the subtree, so that a later query can hit it or
// any recyclable subtree inside it. `breaker` names the kind of subtree the parent makes of `op`, if any.
// A capture is skipped for whole large-table scans, whose replay would save nothing, and for results already refused
// for their size.
// Returns true if it counted a skip: a skip is counted at the innermost skipped subtree, since the subtrees
// around it contain the same cause.
static bool RecycleSubtrees(HookContext &hook, unique_ptr<LogicalOperator> &op, const char *breaker) {
	if (IsLogicalBlock(*op) && op->Cast<LogicalBlock>().capture) {
		return RecycleInsideCapturedBlock(hook, *op);
	}
	if (op->type == LogicalOperatorType::LOGICAL_GET && IsWholeLargeTableScan(hook.context, op->Cast<LogicalGet>())) {
		return false;
	}
	if (!breaker && IsRecyclableRoot(*op)) {
		breaker = BreakerName(*op);
	}
	// The key describes the subtree before anything inside it is replayed or captured.
	SubtreeKey subtree;
	if (breaker) {
		subtree = BuildRecyclingKey(hook.dynamic_filter_sources, *op);
	}
	if (subtree.recyclable && TryReplay(hook, op, subtree.key, breaker)) {
		return false;
	}
	bool capture = subtree.recyclable && ShouldCapture(hook, *op, subtree.key, breaker);
	if (capture) {
		// The subtree will be captured whole. A join filter only skips rows that its join discards anyway, so the
		// join above stays correct without it. Done before the children, which a replay may free.
		for (auto scan : subtree.external_filter_scans) {
			const_cast<LogicalGet *>(scan)->dynamic_filters = nullptr;
		}
	}
	bool contains_skip = false;
	for (idx_t i = 0; i < op->children.size(); i++) {
		if (RecycleSubtrees(hook, op->children[i], ChildBreakerName(hook.context, *op, i))) {
			contains_skip = true;
		}
	}
	if (capture) {
		WrapInCapture(op, subtree.key, waddle::ResultKind::SUBTREE);
	} else if (breaker && !subtree.recyclable && !contains_skip) {
		RecordSkip(hook, breaker, subtree.skip);
		return true;
	}
	return contains_skip;
}

void RecycleBreakers(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	if (plan->type == LogicalOperatorType::LOGICAL_EXPLAIN &&
	    plan->Cast<LogicalExplain>().explain_type == ExplainType::EXPLAIN_STANDARD) {
		return;
	}
	if (!MayRecycle(input.context) || !HasRecyclableRoot(input.context, *plan)) {
		return;
	}
	HookContext hook(input.context, input.optimizer.binder);
	CollectDynamicFilterSources(*plan, hook.dynamic_filter_sources);
	RecycleSubtrees(hook, plan, nullptr);
	ReplaceBindings(*plan, std::move(hook.replacements));
}

} // namespace recycling
} // namespace duckdb
