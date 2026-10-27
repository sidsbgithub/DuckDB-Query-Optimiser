// The extension's settings.

#include "recycling_settings.hpp"

#include "duckdb/common/types/value.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"

namespace duckdb {

namespace recycling {

static constexpr const char *RECYCLING = "waddle_recycling";
static constexpr const char *QUERY_RESULTS = "waddle_recycling_query_results";
static constexpr const char *MAX_BYTES = "waddle_recycling_max_bytes";
static constexpr const char *DIAGNOSTICS = "waddle_recycling_diagnostics";

void RegisterSettings(DBConfig &config) {
	config.AddExtensionOption(RECYCLING, "Enable query recycling", LogicalType::BOOLEAN, Value::BOOLEAN(true));
	config.AddExtensionOption(QUERY_RESULTS, "Replay whole results of repeated statements, including as subqueries",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true));
	// Global by default: the budget bounds one cache shared by every connection.
	config.AddExtensionOption(MAX_BYTES, "Maximum bytes retained for recycled results", LogicalType::UBIGINT,
	                          Value::UBIGINT(2048ULL * 1024 * 1024), nullptr, SetScope::GLOBAL);
	config.AddExtensionOption(
	    DIAGNOSTICS,
	    "Log recycling keys, hits, misses and skip reasons (read them after CALL enable_logging('WaddleRecycling'))",
	    LogicalType::BOOLEAN, Value::BOOLEAN(false));
}

// Whether the boolean setting `name` exists and is false.
static bool IsDisabled(ClientContext &context, const char *name) {
	Value setting;
	return context.TryGetCurrentSetting(name, setting) && !setting.GetValue<bool>();
}

// Whether the boolean setting `name` exists and is true.
static bool IsEnabled(ClientContext &context, const char *name) {
	Value setting;
	return context.TryGetCurrentSetting(name, setting) && setting.GetValue<bool>();
}

bool RecyclingEnabled(ClientContext &context) {
	return !IsDisabled(context, RECYCLING);
}

bool QueryResultsEnabled(ClientContext &context) {
	return !IsDisabled(context, QUERY_RESULTS);
}

bool DiagnosticsEnabled(ClientContext &context) {
	return IsEnabled(context, DIAGNOSTICS);
}

uint64_t RecyclingBudget(ClientContext &context) {
	Value setting;
	context.TryGetCurrentSetting(MAX_BYTES, setting);
	return setting.GetValue<uint64_t>();
}

} // namespace recycling
} // namespace duckdb
