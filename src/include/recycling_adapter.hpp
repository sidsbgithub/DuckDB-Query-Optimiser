#pragma once

#include "recycling_cache.hpp"

namespace duckdb {

class ClientContext;
class ExtensionLoader;

void RegisterRecycling(ExtensionLoader &loader);
waddle::RecyclingStats GetRecyclingStats(ClientContext &context);

} // namespace duckdb
