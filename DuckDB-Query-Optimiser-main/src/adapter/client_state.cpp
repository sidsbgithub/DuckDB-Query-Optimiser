// The per-connection callbacks: write announcements, invalidation on commit, and re-planning of prepared statements.

#include "client_state.hpp"

#include "recycling_state.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/prepared_statement_data.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

namespace duckdb {

namespace recycling {

//===--------------------------------------------------------------------===//
// Per-connection callbacks: invalidation and prepared plans
//===--------------------------------------------------------------------===//
// Whether the statement writes a database whose tables may be recycled. This mirrors MetaTransaction::ModifyDatabase,
// which does not mark writes to the temp or system catalog either: scans of temp objects are never recycled.
static bool WritesRecyclableData(const StatementProperties &properties) {
	for (auto &modified : properties.modified_databases) {
		if (modified.first != TEMP_CATALOG && modified.first != SYSTEM_CATALOG) {
			return true;
		}
	}
	return false;
}

bool RecyclingClientState::CanRequestRebind() {
	return true;
}

RebindQueryInfo RecyclingClientState::OnFinalizePrepare(ClientContext &context, PreparedStatementData &prepared,
                                                        PreparedStatementMode mode) {
	bool attaches_or_detaches = prepared.statement_type == StatementType::ATTACH_STATEMENT ||
	                            prepared.statement_type == StatementType::DETACH_STATEMENT;
	if (attaches_or_detaches || WritesRecyclableData(prepared.properties)) {
		AnnounceWrite(context);
	}
	attached_or_detached = attached_or_detached || attaches_or_detaches;
	return RebindQueryInfo::DO_NOT_REBIND;
}

void RecyclingClientState::TransactionCommit(MetaTransaction &transaction, ClientContext &context) {
	if (transaction.ModifiedDatabase() || attached_or_detached) {
		Cache(context).Invalidate(DatabaseManager::Get(context).ActiveTransactionNumber());
	}
	EndWrite(context);
}

void RecyclingClientState::TransactionRollback(MetaTransaction &transaction, ClientContext &context,
                                               optional_ptr<ErrorData> error) {
	EndWrite(context);
}

RebindQueryInfo RecyclingClientState::OnExecutePrepared(ClientContext &context, PreparedStatementCallbackInfo &info,
                                                        RebindQueryInfo current_rebind) {
	return RebindQueryInfo::ATTEMPT_TO_REBIND;
}

RebindQueryInfo RecyclingClientState::OnRebindPreparedStatement(ClientContext &context,
                                                                BindPreparedStatementCallbackInfo &info,
                                                                RebindQueryInfo current_rebind) {
	return RebindQueryInfo::ATTEMPT_TO_REBIND;
}

void RecyclingClientState::AnnounceWrite(ClientContext &context) {
	if (!announced_write) {
		Cache(context).BeginWrite();
		announced_write = true;
	}
}

void RecyclingClientState::EndWrite(ClientContext &context) {
	if (announced_write) {
		Cache(context).EndWrite();
		announced_write = false;
	}
	attached_or_detached = false;
}

shared_ptr<RecyclingClientState> RegisterClientState(ClientContext &context) {
	return context.registered_state->GetOrCreate<RecyclingClientState>("waddle_recycling");
}

void RecyclingConnectionCallback::OnConnectionOpened(ClientContext &context) {
	RegisterClientState(context);
}

} // namespace recycling
} // namespace duckdb
