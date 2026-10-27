// The adapter's front door, and the only adapter file the extension calls (through recycling_adapter.hpp). It
// registers the settings, the optimizer hook, the log type and the per-connection callbacks; it recycles whole query
// results and pages of ordered results (ADR-0004); and RecyclingPreOptimize and RecyclingOptimize run the passes in
// src/adapter/ in a fixed order (ADR-0007).

#include "recycling_adapter.hpp"

#include "adapter/blocks.hpp"
#include "adapter/client_state.hpp"
#include "adapter/operators.hpp"
#include "adapter/pipeline_breakers.hpp"
#include "adapter/plan_keys.hpp"
#include "adapter/plan_rewrites.hpp"
#include "adapter/recycling_state.hpp"
#include "adapter/recycling_settings.hpp"

#include "duckdb/logging/log_manager.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/extension_callback.hpp"
#include "duckdb/planner/operator/logical_explain.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_top_n.hpp"

namespace duckdb {

namespace recycling {

//===--------------------------------------------------------------------===//
// Whole query results
//===--------------------------------------------------------------------===//
// The operator whose result the client receives: below EXPLAIN ANALYZE, which only reports on it.
static unique_ptr<LogicalOperator> *QueryRoot(unique_ptr<LogicalOperator> &plan) {
	if (plan->type != LogicalOperatorType::LOGICAL_EXPLAIN) {
		return &plan;
	}
	if (plan->Cast<LogicalExplain>().explain_type != ExplainType::EXPLAIN_ANALYZE || plan->children.size() != 1) {
		return nullptr;
	}
	return &plan->children[0];
}

// The operators that can root a statement's bound plan.
static bool CanRootQuery(const LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_PROJECTION:
	case LogicalOperatorType::LOGICAL_ORDER_BY:
	case LogicalOperatorType::LOGICAL_LIMIT:
	case LogicalOperatorType::LOGICAL_DISTINCT:
	case LogicalOperatorType::LOGICAL_UNION:
	case LogicalOperatorType::LOGICAL_EXCEPT:
	case LogicalOperatorType::LOGICAL_INTERSECT:
		return true;
	default:
		return false;
	}
}

// Replays the query result under `key` in place of `slot`, and appends the old -> new binding map the parents need.
// False on a miss. The lookup counts under the query-result counters.
static bool ReplayResult(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &slot, const string &key,
                         vector<ReplacementBinding> &replacements,
                         const std::function<bool(const waddle::RecycledPayload &)> &usable = nullptr) {
	auto payload = LookupResult(input.context, key, waddle::ResultKind::QUERY, usable);
	if (!payload) {
		return false;
	}
	slot->ResolveOperatorTypes();
	ReplaceWithReplay(input.optimizer.binder, slot, *payload, replacements);
	return true;
}

// Replays every outermost subtree that repeats an earlier query whole. Only query results are looked up here, since
// only they were captured before any optimizer changed their plan.
static void ReplayQueryResults(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &op,
                               vector<ReplacementBinding> &replacements) {
	if (CanRootQuery(*op) && IsRecyclableQuery(*op)) {
		auto key = QueryResultKey(input.context, *op);
		if (!key.empty() && ReplayResult(input, op, key, replacements)) {
			return;
		}
	}
	for (auto &child : op->children) {
		ReplayQueryResults(input, child, replacements);
	}
}

//===--------------------------------------------------------------------===//
// Pages of ordered results
//===--------------------------------------------------------------------===//
// The slot below the projections with one child at the top of `slot`. Projections keep their input's rows, in order.
static unique_ptr<LogicalOperator> &BelowProjections(unique_ptr<LogicalOperator> &slot) {
	auto current = &slot;
	while ((*current)->type == LogicalOperatorType::LOGICAL_PROJECTION && (*current)->children.size() == 1) {
		current = &(*current)->children[0];
	}
	return *current;
}

// The page of a LIMIT with a constant limit and a constant or no offset; false for any other LIMIT.
static bool ConstantPage(const LogicalLimit &op, Page &page) {
	if (op.limit_val.Type() != LimitNodeType::CONSTANT_VALUE) {
		return false;
	}
	if (op.offset_val.Type() == LimitNodeType::CONSTANT_VALUE) {
		page.offset = op.offset_val.GetConstantValue();
	} else if (op.offset_val.Type() == LimitNodeType::UNSET) {
		page.offset = 0;
	} else {
		return false;
	}
	page.limit = op.limit_val.GetConstantValue();
	return true;
}

// A page of an ordered statement: a constant LIMIT and OFFSET directly over its ORDER BY, below projections only.
static optional_ptr<LogicalLimit> FindPage(unique_ptr<LogicalOperator> &root, Page &page) {
	auto op = BelowProjections(root).get();
	if (op->type != LogicalOperatorType::LOGICAL_LIMIT || op->children.size() != 1 ||
	    op->children[0]->type != LogicalOperatorType::LOGICAL_ORDER_BY) {
		return nullptr;
	}
	auto &limit = op->Cast<LogicalLimit>();
	return ConstantPage(limit, page) ? &limit : nullptr;
}

// How many ordered rows to capture for a page, so that the next few pages replay from them.
static idx_t PrefixRows(const Page &page) {
	return MaxValue<idx_t>(4 * (page.offset + page.limit), 1024);
}

// Before the optimizers run, on a statement that is a page of an ordered result: replays the page from the ordered
// rows that an earlier page of the statement captured, or remembers the page in `planned` so that CapturePrefix
// captures those rows.
static void RecyclePage(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &root, PlannedQueryResult &planned,
                        vector<ReplacementBinding> &replacements) {
	Page page;
	auto limit = FindPage(root, page);
	if (!limit) {
		return;
	}
	auto &order = limit->children[0];
	auto prefix_key = QueryResultKey(input.context, *order);
	if (prefix_key.empty()) {
		return;
	}
	prefix_key = "prefix:" + prefix_key;
	// The captured rows hold the page, or are the whole ordered result.
	auto holds_page = [&](const waddle::RecycledPayload &result) {
		auto &prefix = static_cast<const CollectionPayload &>(result);
		auto rows = prefix.collection->Count();
		return page.offset + page.limit <= rows || rows < prefix.prefix_rows;
	};
	if (!ReplayResult(input, order, prefix_key, replacements, holds_page)) {
		planned.prefix_key = std::move(prefix_key);
		planned.page = page;
	}
}

// After the optimizers have run: widens the page to the first rows of the ordered result, captures them under the
// ORDER BY's key, and takes the page from them.
static void CapturePrefix(PlannedQueryResult &planned, unique_ptr<LogicalOperator> &root) {
	auto slot = &BelowProjections(root);
	auto &op = **slot;
	auto &page = planned.page;
	auto rows = PrefixRows(page);
	if (op.type == LogicalOperatorType::LOGICAL_TOP_N) {
		auto &top_n = op.Cast<LogicalTopN>();
		if (top_n.limit != page.limit || top_n.offset != page.offset) {
			return;
		}
		top_n.limit = rows;
		top_n.offset = 0;
	} else if (op.type == LogicalOperatorType::LOGICAL_LIMIT &&
	           BelowProjections(op.children[0])->type == LogicalOperatorType::LOGICAL_ORDER_BY) {
		auto &limit = op.Cast<LogicalLimit>();
		Page optimized;
		if (!ConstantPage(limit, optimized) || optimized.limit != page.limit || optimized.offset != page.offset) {
			return;
		}
		limit.limit_val = BoundLimitNode::ConstantValue(NumericCast<int64_t>(rows));
		limit.offset_val = BoundLimitNode();
	} else {
		return;
	}
	WrapInCapture(*slot, std::move(planned.prefix_key), waddle::ResultKind::QUERY, rows);
	auto offset = page.offset ? BoundLimitNode::ConstantValue(NumericCast<int64_t>(page.offset)) : BoundLimitNode();
	auto page_limit = make_uniq<LogicalLimit>(BoundLimitNode::ConstantValue(NumericCast<int64_t>(page.limit)),
	                                          std::move(offset));
	page_limit->children.push_back(std::move(*slot));
	page_limit->ResolveOperatorTypes();
	*slot = std::move(page_limit);
}

//===--------------------------------------------------------------------===//
// The two optimizer hooks
//===--------------------------------------------------------------------===//
// Before the optimizers run, on a statement that may recycle its result:
// 1. removes unused LEFT JOINs (ADR-0006), so that the key describes the statement as it will run;
// 2. replays the statement's whole result, or remembers its key so that CaptureQueryResult captures the result
//    once the plan is optimized (ADR-0004);
// 3. replays a page from the ordered rows an earlier page captured, or remembers the page so that its rows are
//    captured; if nothing was replayed, replays the subqueries that repeat an earlier statement whole;
// 4. recycles blocks (ADR-0005);
// 5. removes the DELIM join of each equality EXISTS (ADR-0006).
static void RecyclingPreOptimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	auto state = RegisterClientState(input.context);
	auto &planned = state->planned;
	planned = PlannedQueryResult();
	if (!MayRecycle(input.context) || !QueryResultsEnabled(input.context)) {
		return;
	}
	auto root = QueryRoot(plan);
	if (!root || !IsRecyclableQuery(**root)) {
		return;
	}
	auto outputs = (*root)->GetColumnBindings();
	RemoveUnusedLeftJoins(*root, column_binding_set_t(outputs.begin(), outputs.end()));
	auto key = QueryResultKey(input.context, **root);
	if (key.empty()) {
		return;
	}
	// Nothing above the query root reads its columns, so no binding needs replacing.
	vector<ReplacementBinding> unused;
	if (ReplayResult(input, *root, key, unused)) {
		return;
	}
	planned.key = std::move(key);
	planned.optimizer = &input.optimizer;
	vector<ReplacementBinding> replacements;
	RecyclePage(input, *root, planned, replacements);
	// A subquery may repeat an earlier statement whole, as a drill-down that filters a dashboard query does.
	if (replacements.empty()) {
		for (auto &child : (*root)->children) {
			ReplayQueryResults(input, child, replacements);
		}
	}
	ReplaceBindings(*plan, std::move(replacements));
	RecycleBlocks(input, *root);
	RemoveExistenceDelimJoins(input.optimizer, plan);
}

// After the optimizers have run: captures the result of a query whose key RecyclingPreOptimize remembered, and the
// ordered rows of a page.
static void CaptureQueryResult(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	auto state = RegisterClientState(input.context);
	if (state->planned.key.empty() || state->planned.optimizer.get() != &input.optimizer) {
		return;
	}
	auto planned = std::move(state->planned);
	state->planned = PlannedQueryResult();
	auto root = QueryRoot(plan);
	if (!root) {
		return;
	}
	if (!planned.prefix_key.empty()) {
		CapturePrefix(planned, *root);
	}
	WrapInCapture(*root, std::move(planned.key), waddle::ResultKind::QUERY);
}

// After the optimizers: (1) marks the blocks the optimizers filtered from outside, so that (2) the pipeline-breaker
// pass neither captures nor keys them, then (3) captures the whole query result or page prefix, outermost.
static void RecyclingOptimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	MarkBlocksFilteredFromOutside(*plan);
	RecycleBreakers(input, plan);
	CaptureQueryResult(input, plan);
}

} // namespace recycling

waddle::RecyclingStats GetRecyclingStats(ClientContext &context) {
	return recycling::Cache(context).Stats();
}

void RegisterRecycling(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	db.GetLogManager().RegisterLogType(make_uniq<recycling::RecyclingLogType>());
	auto &config = DBConfig::GetConfig(db);
	recycling::RegisterSettings(config);
	OptimizerExtension extension;
	extension.pre_optimize_function = recycling::RecyclingPreOptimize;
	extension.optimize_function = recycling::RecyclingOptimize;
	OptimizerExtension::Register(config, extension);
	// Connections opened from now on register through the callback; those already open, such as the one running
	// LOAD, are registered here.
	ExtensionCallback::Register(config, make_shared_ptr<recycling::RecyclingConnectionCallback>());
	for (auto &context : ConnectionManager::Get(db).GetConnectionList()) {
		recycling::RegisterClientState(*context);
	}
}

} // namespace duckdb
