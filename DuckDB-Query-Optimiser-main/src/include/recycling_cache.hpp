#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace waddle {

// Why a pipeline breaker was not recycled. SkipReasonName gives each one its stats column and log name.
enum class SkipReason : uint8_t {
	VOLATILE,
	PARAMETER,
	EXTERNAL_SCAN,
	TEMP_OBJECT,
	CTE_REFERENCE,
	DELIM_GET,
	UNSUPPORTED // keep last: it ends the range SKIP_REASON_COUNT covers
};
static const size_t SKIP_REASON_COUNT = static_cast<size_t>(SkipReason::UNSUPPORTED) + 1;
const char *SkipReasonName(SkipReason reason);

// What a recycled result holds: a whole query result (or the first rows of an ordered one), or a subtree inside a
// statement (a pipeline breaker or a block). The two are counted apart.
enum class ResultKind : uint8_t { SUBTREE, QUERY };

struct RecyclingStats {
	uint64_t lookups = 0;
	uint64_t hits = 0;
	uint64_t misses = 0;
	uint64_t captures = 0;
	uint64_t aborted_captures = 0;
	uint64_t evictions = 0;
	uint64_t invalidations = 0;
	uint64_t current_bytes = 0;
	uint64_t peak_bytes = 0; // highest current_bytes this run, including before an invalidation
	uint64_t skips[SKIP_REASON_COUNT] = {};
	// Whole query results, counted apart from the pipeline breakers above.
	uint64_t query_lookups = 0;
	uint64_t query_hits = 0;
	uint64_t query_captures = 0;
};

struct RecycledPayload {
	virtual ~RecycledPayload() = default;
};

// Stores complete recycled results without depending on DuckDB plan or execution types.
class RecyclingCache {
public:
	// Null on a miss, or if transaction `transaction_number` may not see the data recycled results hold. The lookup
	// counts under the counters of `kind`. A result that `usable` rejects is a miss.
	std::shared_ptr<RecycledPayload> Lookup(const std::string &key, uint64_t transaction_number, ResultKind kind,
	                                        const std::function<bool(const RecycledPayload &)> &usable);
	// Stores a complete result within `budget` bytes, captured by transaction `transaction_number`. A result
	// larger than a quarter of the budget, or captured by a transaction that may not see current data, is aborted.
	// A result refused for its size is remembered, so that it is not captured again (KnownTooLarge).
	void Publish(const std::string &key, std::shared_ptr<RecycledPayload> result, size_t bytes, size_t budget,
	             uint64_t transaction_number, ResultKind kind);
	// The most bytes one result may hold under `budget`.
	static size_t MaxResultBytes(size_t budget);
	// Whether a capture under `key` was refused for its size, clearly over the current per-result cap: capturing it
	// again would copy every row only to be refused again.
	bool KnownTooLarge(const std::string &key, size_t budget) const;
	// A connection is about to write. Its write can become visible to other transactions before the Invalidate
	// it causes, so until the matching EndWrite no transaction may replay or publish.
	void BeginWrite();
	void EndWrite();
	// Discards every recycled result. Transactions numbered below `next_transaction_number` began before the
	// write that caused this, so they may have read the old data and are no longer current.
	void Invalidate(uint64_t next_transaction_number);
	// Whether transaction `transaction_number` may replay and publish: it began after the last invalidation and
	// no write is in flight. A query checks this up front to skip recycling work; Lookup and Publish check it
	// again under the lock, since a write may begin in between.
	bool CanRecycle(uint64_t transaction_number) const;
	void RecordSkip(SkipReason reason);
	RecyclingStats Stats() const;

private:
	struct RecycledResult {
		std::shared_ptr<RecycledPayload> data;
		size_t bytes;
		std::list<std::string>::iterator recency;
	};
	using ResultMap = std::unordered_map<std::string, RecycledResult>;

	bool CanRecycle(const std::lock_guard<std::mutex> &guard, uint64_t transaction_number) const;
	// The only eviction policy: drop least-recently-used results until at most `target` bytes remain.
	void Evict(size_t target);
	void Erase(ResultMap::iterator result);

	mutable std::mutex lock;
	ResultMap results;
	// Least recently used first. A replay holds its own reference, so eviction never frees a result mid-scan.
	std::list<std::string> recent_keys;
	RecyclingStats stats;
	uint64_t first_current_transaction_number = 0;
	uint64_t writes_in_flight = 0;
	// Captures refused for their size, with the bytes they held. Forgotten on Invalidate.
	std::unordered_map<std::string, size_t> refused_bytes;
};

} // namespace waddle
