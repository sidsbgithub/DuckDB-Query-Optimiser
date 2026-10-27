#include "recycling_cache.hpp"

#include <algorithm>
#include <cassert>
#include <iterator>

namespace waddle {

// How many refused keys are remembered; past this they are all forgotten.
static const size_t MAX_REFUSED_KEYS = 1024;

// A quarter of the budget, so that a single large capture cannot evict most of the cache (Oracle's result cache has
// the same rule, RESULT_CACHE_MAX_RESULT).
size_t RecyclingCache::MaxResultBytes(size_t budget) {
	return budget / 4;
}

const char *SkipReasonName(SkipReason reason) {
	switch (reason) {
	case SkipReason::VOLATILE:
		return "volatile";
	case SkipReason::PARAMETER:
		return "parameter";
	case SkipReason::EXTERNAL_SCAN:
		return "external_scan";
	case SkipReason::TEMP_OBJECT:
		return "temp_object";
	case SkipReason::CTE_REFERENCE:
		return "cte_reference";
	case SkipReason::DELIM_GET:
		return "delim_get";
	case SkipReason::UNSUPPORTED:
		return "unsupported";
	}
	return "unsupported";
}

std::shared_ptr<RecycledPayload> RecyclingCache::Lookup(const std::string &key, uint64_t transaction_number,
                                                       ResultKind kind,
                                                       const std::function<bool(const RecycledPayload &)> &usable) {
	bool query_result = kind == ResultKind::QUERY;
	std::lock_guard<std::mutex> guard(lock);
	(query_result ? stats.query_lookups : stats.lookups)++;
	auto found = results.find(key);
	if (found == results.end() || !CanRecycle(guard, transaction_number) || (usable && !usable(*found->second.data))) {
		if (!query_result) {
			stats.misses++;
		}
		return nullptr;
	}
	(query_result ? stats.query_hits : stats.hits)++;
	recent_keys.splice(recent_keys.end(), recent_keys, found->second.recency);
	return found->second.data;
}

void RecyclingCache::Publish(const std::string &key, std::shared_ptr<RecycledPayload> result, size_t bytes,
                             size_t budget, uint64_t transaction_number, ResultKind kind) {
	bool query_result = kind == ResultKind::QUERY;
	std::lock_guard<std::mutex> guard(lock);
	// A write that committed, or is in flight and may have committed, while this result was captured may have made it
	// stale. A result over the per-result cap would push out too many others. Either way the query that captured it
	// already has its answer, so aborting only forgoes later hits.
	if (!CanRecycle(guard, transaction_number)) {
		stats.aborted_captures++;
		return;
	}
	if (bytes > MaxResultBytes(budget)) {
		// Remembered, so that the same capture is not copied again only to be refused. A query result is not: its
		// capture is a copy of rows the statement produces anyway. A narrower block result under the same key, with
		// fewer columns, stays cached for the statements that read only those.
		if (!query_result) {
			if (refused_bytes.size() >= MAX_REFUSED_KEYS) {
				refused_bytes.clear();
			}
			refused_bytes[key] = bytes;
		}
		stats.aborted_captures++;
		return;
	}
	refused_bytes.erase(key);
	// Two concurrent misses on the same key both capture; the later result replaces the earlier one.
	auto existing = results.find(key);
	if (existing != results.end()) {
		Erase(existing);
	}
	Evict(budget - bytes);
	recent_keys.push_back(key);
	RecycledResult recycled = {std::move(result), bytes, std::prev(recent_keys.end())};
	results.emplace(key, std::move(recycled));
	stats.current_bytes += bytes;
	stats.peak_bytes = std::max(stats.peak_bytes, stats.current_bytes);
	(query_result ? stats.query_captures : stats.captures)++;
}

void RecyclingCache::Evict(size_t target) {
	while (stats.current_bytes > target && !recent_keys.empty()) {
		Erase(results.find(recent_keys.front()));
		stats.evictions++;
	}
}

void RecyclingCache::Erase(ResultMap::iterator result) {
	stats.current_bytes -= result->second.bytes;
	recent_keys.erase(result->second.recency);
	results.erase(result);
}

void RecyclingCache::Invalidate(uint64_t next_transaction_number) {
	std::lock_guard<std::mutex> guard(lock);
	results.clear();
	recent_keys.clear();
	refused_bytes.clear();
	stats.current_bytes = 0;
	stats.invalidations++;
	// Concurrent commits may invalidate out of order; never move the boundary back.
	first_current_transaction_number = std::max(first_current_transaction_number, next_transaction_number);
}

void RecyclingCache::BeginWrite() {
	std::lock_guard<std::mutex> guard(lock);
	writes_in_flight++;
}

void RecyclingCache::EndWrite() {
	std::lock_guard<std::mutex> guard(lock);
	assert(writes_in_flight > 0);
	writes_in_flight--;
}

bool RecyclingCache::CanRecycle(uint64_t transaction_number) const {
	std::lock_guard<std::mutex> guard(lock);
	return CanRecycle(guard, transaction_number);
}

bool RecyclingCache::CanRecycle(const std::lock_guard<std::mutex> &, uint64_t transaction_number) const {
	return writes_in_flight == 0 && transaction_number >= first_current_transaction_number;
}

bool RecyclingCache::KnownTooLarge(const std::string &key, size_t budget) const {
	std::lock_guard<std::mutex> guard(lock);
	auto found = refused_bytes.find(key);
	auto cap = MaxResultBytes(budget);
	// A margin, so that a result within a few blocks of the cap, whose allocation varies between runs by about a
	// partly filled block per thread, is retried.
	return found != refused_bytes.end() && found->second > cap + cap / 64;
}

void RecyclingCache::RecordSkip(SkipReason reason) {
	std::lock_guard<std::mutex> guard(lock);
	stats.skips[static_cast<size_t>(reason)]++;
}

RecyclingStats RecyclingCache::Stats() const {
	std::lock_guard<std::mutex> guard(lock);
	return stats;
}

} // namespace waddle
