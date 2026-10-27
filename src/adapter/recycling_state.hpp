#pragma once

// The adapter's shared vocabulary: the database-wide recycling state that every connection shares, the payload of a
// recycled result, the columns and ranges of blocks, and the facts every pass asks for. Every module that touches the
// shared state or recycled results includes it; plan_keys and plan_rewrites do not.

#include "recycling_cache.hpp"

#include "duckdb/common/common.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/logging/log_type.hpp"
#include "duckdb/storage/object_cache.hpp"

#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace duckdb {

class ClientContext;

namespace recycling {

// A column of a block: the canonical table index, within the block's key, of the scan that reads it, and its table
// column. See blocks.cpp.
using BlockColumn = std::pair<idx_t, idx_t>;

// A range that a block's WHERE puts on one of its columns: it keeps the rows whose value is not null and within the
// bounds. A null bound is open.
struct BlockRange {
	Value lower;
	bool lower_inclusive = true;
	Value upper;
	bool upper_inclusive = true;
};
using BlockRanges = std::map<BlockColumn, BlockRange>;

// The statements that have read a block family: the blocks they read, and which of their columns.
struct BlockFamily {
	std::set<BlockColumn> columns;
	std::unordered_set<size_t> key_hashes;
	// Whether two statements read the same block.
	bool reused = false;
};

// A block whose rows were counted before any capture of it: a capture copies every row, and DuckDB's estimates of a
// block's rows can be a hundred times too low, so a block is captured only once its size is known to fit.
struct MeasuredBlock {
	string key;
	BlockRanges ranges;
	idx_t rows;
	// The cache's invalidations when the rows were counted: a write since may have changed them.
	uint64_t invalidations;
};

// The object cache owns this state and releases it before DuckDB's buffer manager.
struct RecyclingState : public ObjectCacheEntry {
	static string ObjectType() {
		return "waddle_recycling_cache";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx();
	}
	waddle::RecyclingCache cache;
	// Block families by their key. A block capture keeps the columns its family's statements have read.
	std::mutex family_lock;
	std::unordered_map<string, BlockFamily> block_families;
	// The captured blocks by their key without ranges: each block's key and ranges, the newest last.
	std::unordered_map<string, vector<std::pair<string, BlockRanges>>> ranged_blocks;
	// The measured blocks by their key without ranges, the newest last.
	std::unordered_map<string, vector<MeasuredBlock>> measured_blocks;
};

class RecyclingLogType : public LogType {
public:
	static constexpr const char *NAME = "WaddleRecycling";
	static constexpr LogLevel LEVEL = LogLevel::LOG_INFO;
	RecyclingLogType() : LogType(NAME, LEVEL) {
	}
};

struct CollectionPayload : public waddle::RecycledPayload {
	CollectionPayload(std::shared_ptr<ColumnDataCollection> value, bool ordered, idx_t prefix_rows)
	    : collection(std::move(value)), ordered(ordered), prefix_rows(prefix_rows) {
	}
	std::shared_ptr<ColumnDataCollection> collection;
	// The rows are in the order the query must return them: replay them in order, on one thread.
	bool ordered;
	// For the first rows of an ordered result: how many were asked for. Fewer rows means the result ended.
	idx_t prefix_rows;
	// For a block result: the collection column that holds each block column. Two block columns that the joins
	// make equal may share one.
	vector<std::pair<BlockColumn, idx_t>> block_columns;
};

// The database-wide recycling state, which DuckDB's object cache owns and creates on first use. `family_lock` guards
// its three block maps and the cache has its own lock; no code holds both at once.
RecyclingState &State(ClientContext &context);
// The database-wide cache of recycled results: State(context).cache.
waddle::RecyclingCache &Cache(ClientContext &context);
// Numbers transactions in the order they began, database-wide. Needs the statement's transaction.
uint64_t TransactionNumber(ClientContext &context);
// The recycled result of `kind` under `key`: null on a miss, if this statement's transaction may not see the data
// recycled results hold, or if `usable` rejects it.
std::shared_ptr<CollectionPayload>
LookupResult(ClientContext &context, const string &key, waddle::ResultKind kind,
             const std::function<bool(const waddle::RecycledPayload &)> &usable = nullptr);
// Whether the statement may recycle at all: recycling is on, the transaction is auto-commit and the cache may serve
// it. Lookup and Publish check the cache again under its lock.
bool MayRecycle(ClientContext &context);
// The entry under `key` of one of RecyclingState's block maps. A map that holds `max_entries` forgets every entry
// before it takes a new key. The caller holds family_lock.
template <class MAP>
typename MAP::mapped_type &BoundedEntry(MAP &map, const string &key, idx_t max_entries) {
	if (map.size() >= max_entries && !map.count(key)) {
		map.clear();
	}
	return map[key];
}

// A copy of the entry under `key` of one of RecyclingState's block maps, or an empty one, taken under family_lock so
// that the caller can then call the cache without holding it.
template <class MAP>
typename MAP::mapped_type SnapshotEntry(RecyclingState &state, MAP &map, const string &key) {
	lock_guard<mutex> guard(state.family_lock);
	auto entry = map.find(key);
	return entry == map.end() ? typename MAP::mapped_type() : entry->second;
}

} // namespace recycling
} // namespace duckdb
