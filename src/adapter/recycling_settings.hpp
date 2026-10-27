#pragma once

// The extension's settings: their names and defaults, and the values the other adapter files read from them. Each
// setting is named here once.

#include "duckdb/common/common.hpp"

namespace duckdb {

class ClientContext;
class DBConfig;

namespace recycling {

// Registers every setting below with its default.
void RegisterSettings(DBConfig &config);
// `waddle_recycling`: whether recycling is on (default true).
bool RecyclingEnabled(ClientContext &context);
// `waddle_recycling_query_results`: whether whole query results and pages are recycled (default true).
bool QueryResultsEnabled(ClientContext &context);
// `waddle_recycling_diagnostics`: whether recycling events are logged (default false).
bool DiagnosticsEnabled(ClientContext &context);
// `waddle_recycling_max_bytes`: how many bytes all recycled results may hold together (default 2 GiB).
uint64_t RecyclingBudget(ClientContext &context);

} // namespace recycling
} // namespace duckdb
