// The extension's entry point: registers the waddle() greeting, the waddle_recycling_stats() table function and,
// through the adapter's front door (recycling_adapter.hpp), query recycling itself.

#include "waddle_extension.hpp"
#include "recycling_adapter.hpp"
#include "duckdb.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// waddle_recycling_stats(): one row with every recycling counter
//===--------------------------------------------------------------------===//
struct StatsColumn {
	string name;
	uint64_t value;
};

// The columns of waddle_recycling_stats(), in order, with their values in `stats`.
static vector<StatsColumn> StatsColumns(const waddle::RecyclingStats &stats) {
	vector<StatsColumn> columns = {{"lookups", stats.lookups},
	                               {"hits", stats.hits},
	                               {"misses", stats.misses},
	                               {"captures", stats.captures},
	                               {"aborted_captures", stats.aborted_captures},
	                               {"evictions", stats.evictions},
	                               {"invalidations", stats.invalidations},
	                               {"current_bytes", stats.current_bytes},
	                               {"peak_bytes", stats.peak_bytes}};
	for (size_t i = 0; i < waddle::SKIP_REASON_COUNT; i++) {
		auto reason = static_cast<waddle::SkipReason>(i);
		columns.push_back({string("skipped_") + waddle::SkipReasonName(reason), stats.skips[i]});
	}
	columns.push_back({"query_lookups", stats.query_lookups});
	columns.push_back({"query_hits", stats.query_hits});
	columns.push_back({"query_captures", stats.query_captures});
	return columns;
}

struct StatsData : public TableFunctionData {
	bool done = false;
};

static unique_ptr<FunctionData> StatsBind(ClientContext &context, TableFunctionBindInput &input,
                                          vector<LogicalType> &types, vector<string> &names) {
	for (auto &column : StatsColumns(waddle::RecyclingStats())) {
		names.push_back(column.name);
		types.push_back(LogicalType::UBIGINT);
	}
	return make_uniq<StatsData>();
}

static void StatsScan(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->CastNoConst<StatsData>();
	if (data.done) {
		return;
	}
	data.done = true;
	auto columns = StatsColumns(GetRecyclingStats(context));
	for (idx_t i = 0; i < columns.size(); i++) {
		output.SetValue(i, 0, Value::UBIGINT(columns[i].value));
	}
	output.SetCardinality(1);
}

//===--------------------------------------------------------------------===//
// waddle(name): the template's greeting
//===--------------------------------------------------------------------===//
inline void WaddleScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "...........🦆 " + name.GetString());
	});
}

static void LoadInternal(ExtensionLoader &loader) {
	loader.RegisterFunction(ScalarFunction("waddle", {LogicalType::VARCHAR}, LogicalType::VARCHAR, WaddleScalarFun));
	loader.RegisterFunction(TableFunction("waddle_recycling_stats", {}, StatsScan, StatsBind));
	RegisterRecycling(loader);
}

void WaddleExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string WaddleExtension::Name() {
	return "waddle";
}

std::string WaddleExtension::Version() const {
#ifdef EXT_VERSION_WADDLE
	return EXT_VERSION_WADDLE;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(waddle, loader) {
	duckdb::LoadInternal(loader);
}
}
