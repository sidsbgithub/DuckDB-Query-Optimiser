// How the adapter's operators execute (the capture sink, the block measure and the replay table function), and the
// plan edits that put them in place.

#include "operators.hpp"

#include "recycling_settings.hpp"

#include "duckdb/execution/executor.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

namespace recycling {

// How many measured blocks a base remembers, and how many bases; past this the oldest, or all, are forgotten.
static constexpr idx_t MAX_MEASURED_BLOCKS = 16;
static constexpr idx_t MAX_MEASURED_BASES = 1024;

// Remembers how many rows a block produced, counted while its statement ran.
static void RecordMeasuredBlock(ClientContext &context, const string &base, const string &key,
                                const BlockRanges &ranges, idx_t rows) {
	auto invalidations = Cache(context).Stats().invalidations;
	auto &state = State(context);
	lock_guard<mutex> guard(state.family_lock);
	auto &blocks = BoundedEntry(state.measured_blocks, base, MAX_MEASURED_BASES);
	for (idx_t i = 0; i < blocks.size(); i++) {
		if (blocks[i].key == key) {
			if (blocks[i].invalidations == invalidations) {
				rows = MaxValue(rows, blocks[i].rows);
			}
			blocks.erase_at(i);
			break;
		}
	}
	blocks.push_back(MeasuredBlock {key, ranges, rows, invalidations});
	if (blocks.size() > MAX_MEASURED_BLOCKS) {
		blocks.erase_at(0);
	}
}

static void Publish(ClientContext &context, const string &key, std::shared_ptr<ColumnDataCollection> result,
                    bool ordered, waddle::ResultKind kind, idx_t prefix_rows,
                    const vector<std::pair<BlockColumn, idx_t>> &block_columns) {
	auto budget = RecyclingBudget(context);
	// Whole allocated blocks, not just the bytes written into them: that is the memory the result holds.
	auto bytes = result->AllocationSize();
	auto payload = std::make_shared<CollectionPayload>(std::move(result), ordered, prefix_rows);
	payload->block_columns = block_columns;
	Cache(context).Publish(key, std::move(payload), bytes, budget, TransactionNumber(context), kind);
}

// A recycled result outlives the connection that captured it, so its blocks must belong to the database's
// buffer manager: the per-connection one is destroyed when that connection closes.
static BufferManager &ResultBufferManager(ClientContext &context) {
	return BufferManager::GetBufferManager(*context.db);
}

//===--------------------------------------------------------------------===//
// Capture, mode "sink": a pipeline breaker
//===--------------------------------------------------------------------===//
class CaptureSinkGlobal : public GlobalSinkState {
public:
	mutex lock;
	std::shared_ptr<ColumnDataCollection> collection;
};

class CaptureSinkLocal : public LocalSinkState {
public:
	unique_ptr<ColumnDataCollection> collection;
};

class CaptureSourceGlobal : public GlobalSourceState {
public:
	explicit CaptureSourceGlobal(ColumnDataCollection &collection)
	    : max_threads(MaxValue<idx_t>(collection.ChunkCount(), 1)) {
		collection.InitializeScan(scan_state);
	}
	idx_t MaxThreads() override {
		return max_threads;
	}
	ColumnDataParallelScanState scan_state;
	idx_t max_threads;
};

class CaptureSourceLocal : public LocalSourceState {
public:
	ColumnDataLocalScanState scan_state;
};

class PhysicalCaptureSink : public PhysicalOperator {
public:
	PhysicalCaptureSink(PhysicalPlan &plan, vector<LogicalType> types, idx_t cardinality, string key,
	                    waddle::ResultKind kind, bool ordered = false, idx_t prefix_rows = 0)
	    : PhysicalOperator(plan, PhysicalOperatorType::EXTENSION, std::move(types), cardinality), key(std::move(key)),
	      kind(kind), ordered(ordered), prefix_rows(prefix_rows) {
	}
	string key;
	waddle::ResultKind kind;
	// A query result whose order matters: capture and emit it on one thread, in the order the child produces it.
	bool ordered;
	idx_t prefix_rows;
	vector<std::pair<BlockColumn, idx_t>> block_columns;

	string GetName() const override {
		return "WADDLE_CAPTURE_SINK";
	}

	// Sink
	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return !ordered;
	}
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override {
		auto state = make_uniq<CaptureSinkGlobal>();
		state->collection = std::make_shared<ColumnDataCollection>(ResultBufferManager(context), types);
		return std::move(state);
	}
	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override {
		auto state = make_uniq<CaptureSinkLocal>();
		state->collection = make_uniq<ColumnDataCollection>(ResultBufferManager(context.client), types);
		return std::move(state);
	}
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override {
		input.local_state.Cast<CaptureSinkLocal>().collection->Append(chunk);
		return SinkResultType::NEED_MORE_INPUT;
	}
	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override {
		auto &gstate = input.global_state.Cast<CaptureSinkGlobal>();
		auto &lstate = input.local_state.Cast<CaptureSinkLocal>();
		lock_guard<mutex> guard(gstate.lock);
		gstate.collection->Combine(*lstate.collection);
		return SinkCombineResultType::FINISHED;
	}
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override {
		// DuckDB also finalizes a sink when scheduling one of its pipelines threw: PipelineEvent::Schedule pushes the
		// error and finishes the event with no tasks, so rows may be missing. Without an error or an interrupt, every
		// Sink and Combine has succeeded and the result is complete. The collection is immutable from here, so the
		// cache and our own source can share it. The query itself emits the rows either way.
		if (!pipeline.executor.HasError() && !context.interrupted) {
			Publish(context, key, input.global_state.Cast<CaptureSinkGlobal>().collection, ordered, kind,
			        prefix_rows, block_columns);
		}
		return SinkFinalizeType::READY;
	}

	// Source
	bool IsSource() const override {
		return true;
	}
	bool ParallelSource() const override {
		return !ordered;
	}
	OrderPreservationType SourceOrder() const override {
		return ordered ? OrderPreservationType::FIXED_ORDER : OrderPreservationType::NO_ORDER;
	}
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override {
		return make_uniq<CaptureSourceGlobal>(*sink_state->Cast<CaptureSinkGlobal>().collection);
	}
	unique_ptr<LocalSourceState> GetLocalSourceState(ExecutionContext &context,
	                                                 GlobalSourceState &gstate) const override {
		return make_uniq<CaptureSourceLocal>();
	}
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override {
		auto &gstate = input.global_state.Cast<CaptureSourceGlobal>();
		auto &lstate = input.local_state.Cast<CaptureSourceLocal>();
		sink_state->Cast<CaptureSinkGlobal>().collection->Scan(gstate.scan_state, lstate.scan_state, chunk);
		return chunk.size() == 0 ? SourceResultType::FINISHED : SourceResultType::HAVE_MORE_OUTPUT;
	}
};

//===--------------------------------------------------------------------===//
// Block measure: counts a block's rows before any capture of it, and copies nothing
//===--------------------------------------------------------------------===//
class BlockMeasureGlobal : public GlobalOperatorState {
public:
	std::atomic<idx_t> rows {0};
	// Threads whose input ran to its end; threads that passed rows through the measure, and those of them whose
	// input ran to its end.
	std::atomic<idx_t> threads_ended {0};
	std::atomic<idx_t> threads_counting {0};
	std::atomic<idx_t> threads_complete {0};
};

class BlockMeasureLocal : public OperatorState {
public:
	bool counting = false;
};

// Passes its input through unchanged, counting the rows. Once every thread has finished, it records the count, so
// that a later statement captures the block only if it fits. An operator above that ends the pipeline early (a
// LIMIT, say) leaves the count too low: DuckDB then skips FinalExecute on the threads it stopped, and such a count is
// not recorded. A measure that is not in its pipeline group's base pipeline, as in a later branch of a UNION, never
// gets OperatorFinalize and records nothing; its block is then not captured.
class PhysicalBlockMeasure : public PhysicalOperator {
public:
	PhysicalBlockMeasure(PhysicalPlan &plan, vector<LogicalType> types, idx_t cardinality, string base, string key,
	                     BlockRanges ranges)
	    : PhysicalOperator(plan, PhysicalOperatorType::EXTENSION, std::move(types), cardinality), base(std::move(base)),
	      key(std::move(key)), ranges(std::move(ranges)) {
	}
	string base;
	string key;
	BlockRanges ranges;

	string GetName() const override {
		return "WADDLE_BLOCK_MEASURE";
	}
	unique_ptr<GlobalOperatorState> GetGlobalOperatorState(ClientContext &context) const override {
		return make_uniq<BlockMeasureGlobal>();
	}
	unique_ptr<OperatorState> GetOperatorState(ExecutionContext &context) const override {
		return make_uniq<BlockMeasureLocal>();
	}
	OperatorResultType Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
	                           GlobalOperatorState &gstate, OperatorState &state) const override {
		auto &global = gstate.Cast<BlockMeasureGlobal>();
		auto &local = state.Cast<BlockMeasureLocal>();
		if (!local.counting) {
			local.counting = true;
			global.threads_counting++;
		}
		global.rows += input.size();
		chunk.Reference(input);
		return OperatorResultType::NEED_MORE_INPUT;
	}
	bool ParallelOperator() const override {
		return true;
	}
	// Called once a thread's input has run to its end, unless an operator above ended the pipeline first.
	OperatorFinalizeResultType FinalExecute(ExecutionContext &context, DataChunk &chunk, GlobalOperatorState &gstate,
	                                        OperatorState &state) const override {
		auto &global = gstate.Cast<BlockMeasureGlobal>();
		global.threads_ended++;
		if (state.Cast<BlockMeasureLocal>().counting) {
			global.threads_complete++;
		}
		return OperatorFinalizeResultType::FINISHED;
	}
	bool RequiresFinalExecute() const override {
		return true;
	}
	bool RequiresOperatorFinalize() const override {
		return true;
	}
	OperatorFinalResultType OperatorFinalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                                         OperatorFinalizeInput &input) const override {
		// A pipeline DuckDB knows can produce nothing (an empty build side above, say) never runs: no count then.
		auto &global = input.global_state.Cast<BlockMeasureGlobal>();
		if (!pipeline.executor.HasError() && !context.interrupted && global.threads_ended.load() > 0 &&
		    global.threads_complete.load() == global.threads_counting.load()) {
			RecordMeasuredBlock(context, base, key, ranges, global.rows.load());
		}
		return OperatorFinalResultType::FINISHED;
	}
};

PhysicalOperator &LogicalCapture::CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) {
	auto &child = planner.CreatePlan(*children[0]);
	bool ordered = kind == waddle::ResultKind::QUERY && PhysicalPlanGenerator::PreserveInsertionOrder(context, child);
	auto &op = planner.Make<PhysicalCaptureSink>(types, estimated_cardinality, key, kind, ordered, prefix_rows);
	op.children.push_back(child);
	return op;
}

PhysicalOperator &LogicalBlock::CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) {
	// The column binding resolver has made each expression a reference to a column of the child.
	reference<PhysicalOperator> result = planner.CreatePlan(*children[0]);
	if (capture) {
		auto &sink = planner.Make<PhysicalCaptureSink>(result.get().types, estimated_cardinality, key,
		                                               waddle::ResultKind::SUBTREE)
		                 .Cast<PhysicalCaptureSink>();
		for (idx_t i = 0; i < expressions.size(); i++) {
			sink.block_columns.emplace_back(columns[i], expressions[i]->Cast<BoundReferenceExpression>().index);
		}
		sink.children.push_back(result.get());
		result = sink;
	} else if (measure) {
		auto &counter = planner.Make<PhysicalBlockMeasure>(result.get().types, estimated_cardinality, measure_base,
		                                                   key, measure_ranges);
		counter.children.push_back(result.get());
		result = counter;
	}
	bool identity = result.get().types.size() == read_count;
	vector<unique_ptr<Expression>> select_list;
	for (idx_t i = 0; i < read_count; i++) {
		identity = identity && expressions[i]->Cast<BoundReferenceExpression>().index == i;
		select_list.push_back(expressions[i]->Copy());
	}
	if (identity) {
		return result.get();
	}
	auto &projection = planner.Make<PhysicalProjection>(types, std::move(select_list), estimated_cardinality);
	projection.children.push_back(result.get());
	return projection;
}

bool IsLogicalBlock(const LogicalOperator &op) {
	return op.type == LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR && dynamic_cast<const LogicalBlock *>(&op);
}

//===--------------------------------------------------------------------===//
// Replay: a table function over the recycled result
//===--------------------------------------------------------------------===//
struct ReplayBindData : public TableFunctionData {
	// shared_ptr: the scan keeps the result alive even if the slot is overwritten mid-query
	std::shared_ptr<ColumnDataCollection> result;
	bool ordered = false;
	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<ReplayBindData>();
		copy->result = result;
		copy->ordered = ordered;
		return std::move(copy);
	}
	bool Equals(const FunctionData &other) const override {
		return result == other.Cast<ReplayBindData>().result;
	}
};

struct ReplayGlobal : public GlobalTableFunctionState {
	ReplayGlobal(ColumnDataCollection &collection, bool ordered)
	    : max_threads(ordered ? 1 : MaxValue<idx_t>(collection.ChunkCount(), 1)) {
		collection.InitializeScan(scan_state);
	}
	idx_t MaxThreads() const override {
		return max_threads;
	}
	ColumnDataParallelScanState scan_state;
	idx_t max_threads;
};

struct ReplayLocal : public LocalTableFunctionState {
	ColumnDataLocalScanState scan_state;
};

static unique_ptr<GlobalTableFunctionState> ReplayInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<ReplayBindData>();
	return make_uniq<ReplayGlobal>(*bind_data.result, bind_data.ordered);
}

static unique_ptr<LocalTableFunctionState> ReplayInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                                                           GlobalTableFunctionState *gstate) {
	return make_uniq<ReplayLocal>();
}

static void ReplayScan(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &result = *input.bind_data->Cast<ReplayBindData>().result;
	result.Scan(input.global_state->Cast<ReplayGlobal>().scan_state, input.local_state->Cast<ReplayLocal>().scan_state,
	            output);
}

static TableFunction ReplayFunction(bool ordered) {
	TableFunction fn("waddle_replay", {}, ReplayScan, nullptr, ReplayInitGlobal, ReplayInitLocal);
	fn.projection_pushdown = false;
	if (ordered) {
		fn.order_preservation_type = OrderPreservationType::FIXED_ORDER;
	}
	// Opts out of plan verification's serialization round trip; see ADR-0003.
	fn.verify_serialization = false;
	return fn;
}

unique_ptr<LogicalGet> MakeReplay(Binder &binder, const CollectionPayload &payload, const vector<LogicalType> &types) {
	vector<string> names;
	for (idx_t i = 0; i < types.size(); i++) {
		names.push_back("c" + to_string(i));
	}
	auto bind_data = make_uniq<ReplayBindData>();
	bind_data->result = payload.collection;
	bind_data->ordered = payload.ordered;
	auto rows = bind_data->result->Count();
	auto get = make_uniq<LogicalGet>(binder.GenerateTableIndex(), ReplayFunction(payload.ordered), std::move(bind_data),
	                                 types, names);
	for (idx_t i = 0; i < types.size(); i++) {
		get->AddColumnId(i);
	}
	get->SetEstimatedCardinality(rows);
	get->ResolveOperatorTypes();
	return get;
}

void ReplaceWithReplay(Binder &binder, unique_ptr<LogicalOperator> &op, const CollectionPayload &payload,
                       vector<ReplacementBinding> &replacements) {
	auto old_bindings = op->GetColumnBindings();
	auto get = MakeReplay(binder, payload, op->types);
	auto new_bindings = get->GetColumnBindings();
	for (idx_t i = 0; i < old_bindings.size(); i++) {
		replacements.emplace_back(old_bindings[i], new_bindings[i]);
	}
	op = std::move(get);
}

void WrapInCapture(unique_ptr<LogicalOperator> &slot, string key, waddle::ResultKind kind, idx_t prefix_rows) {
	auto capture = make_uniq<LogicalCapture>(std::move(slot), std::move(key), kind, prefix_rows);
	capture->ResolveOperatorTypes();
	slot = std::move(capture);
}

void ReplaceBindings(LogicalOperator &plan, vector<ReplacementBinding> replacements) {
	if (replacements.empty()) {
		return;
	}
	ColumnBindingReplacer replacer;
	replacer.replacement_bindings = std::move(replacements);
	replacer.VisitOperator(plan);
}

} // namespace recycling
} // namespace duckdb
