// The database-wide recycling state, and the transaction facts the other adapter files ask for.

#include "recycling_state.hpp"

#include "recycling_settings.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

namespace duckdb {

namespace recycling {

constexpr const char *RecyclingLogType::NAME;
constexpr LogLevel RecyclingLogType::LEVEL;

RecyclingState &State(ClientContext &context) {
	return *ObjectCache::GetObjectCache(context).GetOrCreate<RecyclingState>(RecyclingState::ObjectType());
}

waddle::RecyclingCache &Cache(ClientContext &context) {
	return State(context).cache;
}

uint64_t TransactionNumber(ClientContext &context) {
	return MetaTransaction::Get(context).global_transaction_id;
}

std::shared_ptr<CollectionPayload> LookupResult(ClientContext &context, const string &key, waddle::ResultKind kind,
                                                const std::function<bool(const waddle::RecycledPayload &)> &usable) {
	return std::dynamic_pointer_cast<CollectionPayload>(
	    Cache(context).Lookup(key, TransactionNumber(context), kind, usable));
}

// The database-wide cache holds data as committed at the last invalidation. An explicit transaction may see its own
// uncommitted writes, one that began before the invalidation may not see the committed write, and any transaction may
// see a write in flight that has committed but not yet invalidated.
bool MayRecycle(ClientContext &context) {
	if (!RecyclingEnabled(context)) {
		return false;
	}
	return context.transaction.IsAutoCommit() && Cache(context).CanRecycle(TransactionNumber(context));
}

} // namespace recycling
} // namespace duckdb
