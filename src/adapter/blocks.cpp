// Blocks: the joins and filters under a select list or grouping, recycled by table columns and ranges (ADR-0005).

#include "blocks.hpp"

#include "block_ranges.hpp"
#include "operators.hpp"
#include "plan_keys.hpp"
#include "recycling_state.hpp"
#include "recycling_settings.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace duckdb {

namespace recycling {

//===--------------------------------------------------------------------===//
// Blocks: the joins and filters under a select list or grouping
//===--------------------------------------------------------------------===//
// Statements of a dashboard repeat a block and read other columns of it: counts by name, then by gender, then a
// page of rows. A block result keeps every column its family's statements have read, so the next statement
// replays the block instead of joining again. A block's key describes its scans by table, not by the columns a
// statement reads from them; its family key also leaves out constants, so that a family learns which columns to
// keep before a statement repeats its constants. A block is captured only once a statement repeats it, or once its
// family has shown that its statements repeat blocks: most blocks never repeat, and a capture costs a
// materialization. A block result also holds the rows of a block with narrower ranges on its columns; see ADR-0005.

// How many block families, and how many bases of captured blocks, the extension remembers; it forgets them all
// past this.
static constexpr idx_t MAX_BLOCK_FAMILIES = 1024;
// How many captured blocks a base remembers for blocks whose ranges they contain.
static constexpr idx_t MAX_RANGED_BLOCKS = 16;
// A captured block keeps every column of a table this small, which costs little: dimension tables such as roles.
static constexpr idx_t MAX_SMALL_TABLE_ROWS = 1000;

static bool IsBlockRoot(const LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_FILTER:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
		return true;
	default:
		return false;
	}
}

static bool HasJoin(const LogicalOperator &op) {
	if (op.children.size() > 1) {
		return true;
	}
	for (auto &child : op.children) {
		if (HasJoin(*child)) {
			return true;
		}
	}
	return false;
}

// A block, and the projection or aggregate that reads it.
struct FoundBlock {
	LogicalOperator &reader;
	unique_ptr<LogicalOperator> *slot;
};

// The outermost blocks.
static void FindBlocks(unique_ptr<LogicalOperator> &op, vector<FoundBlock> &blocks) {
	for (auto &child : op->children) {
		if ((op->type == LogicalOperatorType::LOGICAL_PROJECTION ||
		     op->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) &&
		    IsBlockRoot(*child) && HasJoin(*child)) {
			blocks.push_back(FoundBlock {*op, &child});
		} else {
			FindBlocks(child, blocks);
		}
	}
}

// The columns that operators outside `block` read.
static void CollectReferences(LogicalOperator &op, const LogicalOperator &block, column_binding_set_t &references) {
	if (&op == &block) {
		return;
	}
	LogicalOperatorVisitor::EnumerateExpressions(
	    op, [&](unique_ptr<Expression> *expression) { CollectColumns(**expression, references); });
	for (auto &child : op.children) {
		CollectReferences(*child, block, references);
	}
}

// A block's scans by their table index.
using BlockScans = std::unordered_map<idx_t, reference<LogicalGet>>;

static void CollectScans(LogicalOperator &op, BlockScans &scans) {
	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		auto &scan = op.Cast<LogicalGet>();
		scans.emplace(scan.table_index, scan);
	}
	for (auto &child : op.children) {
		CollectScans(*child, scans);
	}
}

// A block's key is the key of the block without the ranges its WHERE puts on its columns, its base, and the ranges.
// A block result holds the rows of every block with the same base whose ranges it contains.
struct BlockKey {
	// "block:", the base and the ranges: the key the block's result is captured under.
	string key;
	// The block serialized without its range conjuncts.
	string base;
	// The base without its constants: the key of the block's family.
	string family;
	// Table index -> the canonical table index the base gives it.
	std::unordered_map<idx_t, idx_t> canonical;
	BlockScans scans;
	BlockRanges ranges;
	// The WHERE conjuncts the ranges come from.
	vector<unique_ptr<Expression>> range_conjuncts;
};

// The table index of each canonical table index in the block's key.
static std::unordered_map<idx_t, idx_t> TableIndices(const BlockKey &key) {
	std::unordered_map<idx_t, idx_t> table_of;
	for (auto &entry : key.canonical) {
		table_of.emplace(entry.second, entry.first);
	}
	return table_of;
}

static const BlockColumn NO_BLOCK_COLUMN(DConstants::INVALID_INDEX, DConstants::INVALID_INDEX);

// The table column a block output holds, if a scan of the block produces it. Other outputs, such as the mark of an
// EXISTS, may not survive the optimizers.
static BlockColumn ColumnOf(const BlockKey &key, const ColumnBinding &binding) {
	auto canonical = key.canonical.find(binding.table_index);
	auto scan = key.scans.find(binding.table_index);
	if (canonical == key.canonical.end() || scan == key.scans.end()) {
		return NO_BLOCK_COLUMN;
	}
	auto &column_ids = scan->second.get().GetColumnIds();
	if (binding.column_index >= column_ids.size()) {
		return NO_BLOCK_COLUMN;
	}
	return BlockColumn(canonical->second, column_ids[binding.column_index].GetPrimaryIndex());
}

// Before the optimizers run, a scan reads its columns whole, unfiltered; a block whose scans do not cannot be keyed by
// its tables.
static bool ReadsWholeColumns(const BlockScans &scans) {
	for (auto &entry : scans) {
		auto &scan = entry.second.get();
		if (!scan.projection_ids.empty() || !scan.table_filters.filters.empty()) {
			return false;
		}
		for (auto &column : scan.GetColumnIds()) {
			if (!column.HasPrimaryIndex() || column.HasChildren()) {
				return false;
			}
		}
	}
	return true;
}

// A WHERE conjunct that puts a range on a column of one of the block's scans.
struct RangeConjunct {
	// The conjunct's position in the WHERE.
	idx_t position;
	ColumnBinding column;
	BlockRange range;
};

// Takes the range conjuncts out of the block's WHERE, into `key.range_conjuncts`, in their order. RestoreRangeConjuncts
// puts copies back.
static vector<RangeConjunct> TakeRangeConjuncts(ClientContext &context, LogicalFilter &where, BlockKey &key) {
	vector<RangeConjunct> ranges;
	auto &conjuncts = where.expressions;
	for (idx_t i = 0; i < conjuncts.size(); i++) {
		ColumnBinding column;
		BlockRange range;
		if (ExtractRange(context, *conjuncts[i], column, range) && key.scans.count(column.table_index)) {
			ranges.push_back(RangeConjunct {i, column, std::move(range)});
		}
	}
	for (idx_t i = ranges.size(); i-- > 0;) {
		key.range_conjuncts.insert(key.range_conjuncts.begin(), std::move(conjuncts[ranges[i].position]));
		conjuncts.erase_at(ranges[i].position);
	}
	return ranges;
}

static void RestoreRangeConjuncts(LogicalFilter &where, const BlockKey &key, const vector<RangeConjunct> &ranges) {
	for (idx_t i = 0; i < ranges.size(); i++) {
		where.expressions.insert(where.expressions.begin() + NumericCast<int64_t>(ranges[i].position),
		                         key.range_conjuncts[i]->Copy());
	}
}

// Serializes the block, without its range conjuncts, as its base and as its family. False if the block is not closed
// or cannot be serialized.
static bool SerializeBaseAndFamily(LogicalOperator &block, BlockKey &key) {
	std::unordered_set<idx_t> defined;
	CollectTableIndices(block, defined);
	for (bool family : {false, true}) {
		RecyclingKeySerializer serializer(defined);
		serializer.block_scans = &key.scans;
		serializer.without_values = family;
		if (!TrySerialize(serializer, block) || !serializer.closed) {
			return false;
		}
		if (family) {
			key.family = std::move(serializer.key);
		} else {
			key.base = std::move(serializer.key);
			key.canonical = std::move(serializer.canonical);
		}
	}
	return true;
}

// False if the block is not closed or cannot be serialized, or a range is on a column no scan of the block produces.
static bool BuildBlockKey(ClientContext &context, LogicalOperator &block, BlockKey &result) {
	CollectScans(block, result.scans);
	if (!ReadsWholeColumns(result.scans)) {
		return false;
	}
	// The base leaves out the WHERE conjuncts that put a range on a column of a scan.
	vector<RangeConjunct> ranges;
	optional_ptr<LogicalFilter> where;
	if (block.type == LogicalOperatorType::LOGICAL_FILTER) {
		where = &block.Cast<LogicalFilter>();
		ranges = TakeRangeConjuncts(context, *where, result);
	}
	bool serialized = SerializeBaseAndFamily(block, result);
	if (where) {
		RestoreRangeConjuncts(*where, result, ranges);
	}
	if (!serialized) {
		return false;
	}
	for (auto &conjunct : ranges) {
		auto column = ColumnOf(result, conjunct.column);
		if (column == NO_BLOCK_COLUMN) {
			return false;
		}
		auto inserted = result.ranges.emplace(column, conjunct.range);
		if (!inserted.second) {
			Intersect(inserted.first->second, conjunct.range);
		}
	}
	result.key = "block:" + result.base + RangesKey(result.ranges);
	return true;
}

// Records that this statement reads these columns from its block. Returns whether to capture the block, and every
// column its family's statements have read.
static bool RecordBlockRead(ClientContext &context, const BlockKey &key, const vector<BlockColumn> &columns,
                            std::set<BlockColumn> &family_columns) {
	auto &state = State(context);
	lock_guard<mutex> guard(state.family_lock);
	auto &family = BoundedEntry(state.block_families, key.family, MAX_BLOCK_FAMILIES);
	family.columns.insert(columns.begin(), columns.end());
	family_columns = family.columns;
	bool repeated = !family.key_hashes.insert(std::hash<string>()(key.key)).second;
	family.reused = family.reused || repeated;
	return family.reused;
}

// The binding of each block column the block outputs, adding to its scans the table columns they do not read.
static void BindBlockColumns(LogicalOperator &block, BlockKey &key, const vector<BlockColumn> &columns,
                             vector<unique_ptr<Expression>> &expressions, vector<BlockColumn> &bound) {
	auto table_of = TableIndices(key);
	std::unordered_set<idx_t> output_tables;
	for (auto &binding : block.GetColumnBindings()) {
		output_tables.insert(binding.table_index);
	}
	for (auto &column : columns) {
		auto table = table_of.find(column.first);
		if (table == table_of.end() || !output_tables.count(table->second) || !key.scans.count(table->second)) {
			continue;
		}
		auto &scan = key.scans.at(table->second).get();
		auto &column_ids = scan.GetColumnIds();
		idx_t position = 0;
		while (position < column_ids.size() && column_ids[position].GetPrimaryIndex() != column.second) {
			position++;
		}
		if (position == column_ids.size()) {
			if (column.second >= scan.returned_types.size()) {
				continue;
			}
			scan.AddColumnId(column.second);
		}
		auto type = scan.GetColumnType(column_ids[position]);
		expressions.push_back(make_uniq<BoundColumnRefExpression>(type, ColumnBinding(table->second, position)));
		bound.push_back(column);
	}
}

static idx_t FindBlockColumn(const CollectionPayload &result, const BlockColumn &column) {
	for (auto &entry : result.block_columns) {
		if (entry.first == column) {
			return entry.second;
		}
	}
	return DConstants::INVALID_INDEX;
}

// A block result that holds these columns of the block: the block's own, or that of a block with the same base whose
// ranges contain the block's. `within_ranges` says whether it is the latter, whose rows the block's ranges filter.
static std::shared_ptr<CollectionPayload> LookupBlock(ClientContext &context, const BlockKey &key,
                                                      const vector<BlockColumn> &columns, bool &within_ranges) {
	auto holds = [&](const vector<BlockColumn> &needed) {
		return [&needed](const waddle::RecycledPayload &result) {
			for (auto &column : needed) {
				if (FindBlockColumn(static_cast<const CollectionPayload &>(result), column) ==
				    DConstants::INVALID_INDEX) {
					return false;
				}
			}
			return true;
		};
	};
	within_ranges = false;
	auto payload = LookupResult(context, key.key, waddle::ResultKind::SUBTREE, holds(columns));
	if (payload) {
		return payload;
	}
	auto &state = State(context);
	auto candidates = SnapshotEntry(state, state.ranged_blocks, key.base);
	// The block's ranges filter the rows, so the result must hold their columns too.
	auto needed = columns;
	for (auto &range : key.ranges) {
		needed.push_back(range.first);
	}
	for (auto candidate = candidates.rbegin(); candidate != candidates.rend(); candidate++) {
		if (candidate->first == key.key || !RangesContain(candidate->second, key.ranges)) {
			continue;
		}
		payload = LookupResult(context, candidate->first, waddle::ResultKind::SUBTREE, holds(needed));
		if (payload) {
			within_ranges = true;
			return payload;
		}
	}
	return nullptr;
}

// Remembers a block about to be captured, so that blocks whose ranges it contains can replay it.
static void RecordRangedBlock(ClientContext &context, const BlockKey &key) {
	auto &state = State(context);
	lock_guard<mutex> guard(state.family_lock);
	auto &blocks = BoundedEntry(state.ranged_blocks, key.base, MAX_BLOCK_FAMILIES);
	for (idx_t i = 0; i < blocks.size(); i++) {
		if (blocks[i].first == key.key) {
			blocks.erase_at(i);
			break;
		}
	}
	blocks.emplace_back(key.key, key.ranges);
	if (blocks.size() > MAX_RANGED_BLOCKS) {
		blocks.erase_at(0);
	}
}

// Whether a block's capture fits the per-result cap, as far as measured blocks tell.
enum class BlockSize { FITS, TOO_LARGE, UNKNOWN };

// Roughly the bytes a captured row of these block columns takes: fixed-size values as stored, others as 32 bytes (a
// string's pointer and length plus a short body).
static idx_t EstimateRowBytes(const BlockKey &key, const vector<BlockColumn> &columns) {
	auto table_of = TableIndices(key);
	idx_t bytes = 0;
	for (auto &column : columns) {
		auto table = table_of.find(column.first);
		auto scan = table == table_of.end() ? key.scans.end() : key.scans.find(table->second);
		if (scan == key.scans.end() || column.second >= scan->second.get().returned_types.size()) {
			bytes += 32;
			continue;
		}
		auto physical = scan->second.get().returned_types[column.second].InternalType();
		bytes += TypeIsConstantSize(physical) ? GetTypeIdSize(physical) : 32;
	}
	return MaxValue<idx_t>(bytes, 8);
}

// How many times the rows of its largest measured sibling (a block with the same base and other ranges) a block is
// assumed to have at most, when nothing else is known about it.
static constexpr idx_t SIBLING_ROWS_FACTOR = 4;

// What the measured blocks with the block's base say about its size. A measured block whose ranges contain the
// block's holds every row of it; one whose ranges the block's contain holds only rows of it. A block with other ranges
// is taken to fit if every measured sibling would fit with SIBLING_ROWS_FACTOR times its rows: a dashboard varies
// its constants over blocks of similar size. If the block is larger after all, its publish refuses it and remembers.
static BlockSize KnownBlockSize(ClientContext &context, const BlockKey &key, idx_t row_bytes) {
	auto budget = RecyclingBudget(context);
	if (Cache(context).KnownTooLarge(key.key, budget)) {
		return BlockSize::TOO_LARGE;
	}
	auto cap = waddle::RecyclingCache::MaxResultBytes(budget);
	// No margin: the 32 bytes of a string already overstate the 16 a collection takes for one of up to 12 characters.
	// A block that is larger than estimated costs one copy, which its publish refuses and remembers.
	auto fits = [&](idx_t rows) {
		return rows * row_bytes <= cap;
	};
	auto &state = State(context);
	auto measured = SnapshotEntry(state, state.measured_blocks, key.base);
	bool within_fitting = false;
	bool around_too_large = false;
	bool any_sibling = false;
	bool siblings_fit = true;
	auto invalidations = Cache(context).Stats().invalidations;
	for (auto &block : measured) {
		if (block.invalidations != invalidations) {
			continue;
		}
		if (block.key == key.key) {
			return fits(block.rows) ? BlockSize::FITS : BlockSize::TOO_LARGE;
		}
		within_fitting = within_fitting || (RangesContain(block.ranges, key.ranges) && fits(block.rows));
		around_too_large = around_too_large || (RangesContain(key.ranges, block.ranges) && !fits(block.rows));
		any_sibling = true;
		siblings_fit = siblings_fit && fits(block.rows * SIBLING_ROWS_FACTOR);
	}
	if (within_fitting) {
		return BlockSize::FITS;
	}
	if (around_too_large) {
		return BlockSize::TOO_LARGE;
	}
	return any_sibling && siblings_fit ? BlockSize::FITS : BlockSize::UNKNOWN;
}

// The columns a statement reads from its block, in block column order, each with the binding the statement reads it
// by.
using ReadColumns = std::map<BlockColumn, ColumnBinding>;

static vector<BlockColumn> BlockColumnsOf(const ReadColumns &read) {
	vector<BlockColumn> columns;
	for (auto &entry : read) {
		columns.push_back(entry.first);
	}
	return columns;
}

// The block columns that the operators outside the block read from it. False if one of them is not a column of a
// scan of the block.
static bool FindReadColumns(LogicalOperator &root, LogicalOperator &block, const BlockKey &key, ReadColumns &read) {
	column_binding_set_t references;
	CollectReferences(root, block, references);
	for (auto &binding : block.GetColumnBindings()) {
		if (!references.count(binding)) {
			continue;
		}
		auto column = ColumnOf(key, binding);
		if (column == NO_BLOCK_COLUMN) {
			return false;
		}
		read.emplace(column, binding);
	}
	return true;
}

// Replaces the block with a replay of `payload`, a block result that holds the columns the statement reads. A result
// of a block whose ranges contain this block's (`within_ranges`) is filtered by this block's range conjuncts; if
// `capture`, those rows are captured as this block's own result, with every column of the other.
static void ReplayBlock(ClientContext &context, Binder &binder, unique_ptr<LogicalOperator> &block, const BlockKey &key,
                        const CollectionPayload &payload, const ReadColumns &read, bool within_ranges, bool capture,
                        vector<ReplacementBinding> &replacements) {
	auto replay = MakeReplay(binder, payload, payload.collection->Types());
	auto replay_index = replay->table_index;
	auto replay_binding = [&](const BlockColumn &column) {
		return ColumnBinding(replay_index, FindBlockColumn(payload, column));
	};
	vector<unique_ptr<Expression>> expressions;
	vector<BlockColumn> columns = BlockColumnsOf(read);
	for (auto &entry : read) {
		auto binding = replay_binding(entry.first);
		replacements.emplace_back(entry.second, binding);
		expressions.push_back(make_uniq<BoundColumnRefExpression>(replay->types[binding.column_index], binding));
	}
	if (capture) {
		for (auto &entry : payload.block_columns) {
			if (!read.count(entry.first) && std::find(columns.begin(), columns.end(), entry.first) == columns.end()) {
				ColumnBinding binding(replay_index, entry.second);
				expressions.push_back(make_uniq<BoundColumnRefExpression>(replay->types[entry.second], binding));
				columns.push_back(entry.first);
			}
		}
		RecordRangedBlock(context, key);
	}
	unique_ptr<LogicalOperator> rows = std::move(replay);
	if (within_ranges) {
		auto filter = make_uniq<LogicalFilter>();
		for (auto &conjunct : key.range_conjuncts) {
			auto condition = conjunct->Copy();
			ExpressionIterator::EnumerateExpression(condition, [&](Expression &child) {
				if (child.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
					auto &column = child.Cast<BoundColumnRefExpression>();
					column.binding = replay_binding(ColumnOf(key, column.binding));
				}
			});
			filter->expressions.push_back(std::move(condition));
		}
		filter->children.push_back(std::move(rows));
		rows = std::move(filter);
	}
	block = make_uniq<LogicalBlock>(std::move(rows), key.key, std::move(columns), read.size(), capture);
	block->expressions = std::move(expressions);
}

// The columns a capture of the block keeps: those the statement reads, then every other column its family's
// statements have read, the columns of its ranges (a block whose ranges this block's contain filters its rows by
// them), and every column of a small table.
static vector<BlockColumn> ColumnsToCapture(ClientContext &context, const BlockKey &key, const ReadColumns &read,
                                            std::set<BlockColumn> family_columns) {
	for (auto &range : key.ranges) {
		family_columns.insert(range.first);
	}
	for (auto &entry : key.scans) {
		auto &scan = entry.second.get();
		auto stats = scan.function.cardinality ? scan.function.cardinality(context, scan.bind_data.get()) : nullptr;
		auto canonical = key.canonical.find(entry.first);
		if (stats && stats->has_max_cardinality && stats->max_cardinality <= MAX_SMALL_TABLE_ROWS &&
		    canonical != key.canonical.end()) {
			for (idx_t column = 0; column < scan.returned_types.size(); column++) {
				family_columns.emplace(canonical->second, column);
			}
		}
	}
	vector<BlockColumn> columns = BlockColumnsOf(read);
	for (auto &column : family_columns) {
		if (!read.count(column)) {
			columns.push_back(column);
		}
	}
	return columns;
}

// Marks a block that no block result holds with WADDLE_BLOCK. If `capture`, it captures the block once the block's
// size is known to fit; while its size is unknown, it counts the block's rows instead.
static void MarkBlock(ClientContext &context, unique_ptr<LogicalOperator> &block, BlockKey &key,
                      const ReadColumns &read, bool capture, const std::set<BlockColumn> &family_columns) {
	auto read_columns = BlockColumnsOf(read);
	auto columns = capture ? ColumnsToCapture(context, key, read, family_columns) : read_columns;
	// A capture copies every row of the block, which DuckDB may estimate a hundred times too low: capture only a
	// block known to fit. A block whose size is not known yet counts its rows instead, from its family's first
	// statement on, so that the statement that repeats it knows whether it fits.
	auto size = KnownBlockSize(context, key, EstimateRowBytes(key, columns));
	bool measure = size == BlockSize::UNKNOWN;
	if (capture && size != BlockSize::FITS) {
		capture = false;
		columns = read_columns;
	}
	vector<unique_ptr<Expression>> expressions;
	vector<BlockColumn> bound;
	BindBlockColumns(*block, key, columns, expressions, bound);
	if (bound.size() < read_columns.size() || !std::equal(read_columns.begin(), read_columns.end(), bound.begin())) {
		return;
	}
	if (capture) {
		RecordRangedBlock(context, key);
	}
	auto marked = make_uniq<LogicalBlock>(std::move(block), key.key, std::move(bound), read_columns.size(), capture);
	marked->expressions = std::move(expressions);
	if (measure) {
		marked->measure = true;
		marked->measure_base = key.base;
		marked->measure_ranges = key.ranges;
	}
	block = std::move(marked);
}

// Replays a block result that holds the columns the statement reads from the block, or marks the block. A block is
// captured once its family repeats a block; a captured block keeps every column its family has read, and the columns
// of its ranges. A block is captured only once its size is known to fit; until then each statement that reads it
// counts its rows instead.
static void RecycleBlock(ClientContext &context, Binder &binder, LogicalOperator &root, LogicalOperator &reader,
                         unique_ptr<LogicalOperator> &block, vector<ReplacementBinding> &replacements) {
	// A replay returns the rows in another order, which only a grouping or no preserved order hides.
	if (Settings::Get<PreserveInsertionOrderSetting>(context) &&
	    reader.type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		return;
	}
	BlockKey key;
	if (!IsRecyclableQuery(*block) || !BuildBlockKey(context, *block, key)) {
		return;
	}
	ReadColumns read;
	if (!FindReadColumns(root, *block, key, read)) {
		return;
	}
	std::set<BlockColumn> family_columns;
	bool capture = RecordBlockRead(context, key, BlockColumnsOf(read), family_columns);
	bool within_ranges;
	auto payload = LookupBlock(context, key, BlockColumnsOf(read), within_ranges);
	if (payload) {
		// Only the rows within the block's ranges are new: a block's own result is already captured.
		ReplayBlock(context, binder, block, key, *payload, read, within_ranges, capture && within_ranges,
		            replacements);
	} else {
		MarkBlock(context, block, key, read, capture, family_columns);
	}
}

void RecycleBlocks(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &root) {
	vector<FoundBlock> blocks;
	FindBlocks(root, blocks);
	vector<ReplacementBinding> replacements;
	for (auto &block : blocks) {
		RecycleBlock(input.context, input.optimizer.binder, *root, block.reader, *block.slot, replacements);
	}
	ReplaceBindings(*root, std::move(replacements));
	if (!blocks.empty()) {
		root->ResolveOperatorTypes();
	}
}

// A block is marked directly under the projection or aggregate that reads it, so a filter directly above it after the
// optimizers holds predicates from outside the block. DuckDB's join order optimizer walks through such a filter and
// through the marker, and moves the predicates into the joins it reorders under the marker, leaving the filter empty
// (duckdb/src/optimizer/join_order/relation_manager.cpp, ExtractJoinRelations and ExtractEdges). The block's rows are
// then fewer than its key describes: it must not be captured or counted, and no recycling key may describe it by its
// key. The statement itself stays correct, since DuckDB applies the predicates where it moved them.
void MarkBlocksFilteredFromOutside(LogicalOperator &op) {
	for (auto &child : op.children) {
		if (op.type == LogicalOperatorType::LOGICAL_FILTER && IsLogicalBlock(*child)) {
			auto &block = child->Cast<LogicalBlock>();
			block.filtered_from_outside = true;
			block.capture = false;
			block.measure = false;
		}
		MarkBlocksFilteredFromOutside(*child);
	}
}

} // namespace recycling
} // namespace duckdb
