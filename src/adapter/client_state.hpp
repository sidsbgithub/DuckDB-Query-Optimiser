#pragma once

// The per-connection callbacks. A statement that writes announces its write once it is planned, and its commit
// invalidates the cache that every connection shares; prepared statements are planned again on each execution. The
// front door also keeps here the query result it plans, between its two optimizer hooks.

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/planner/extension_callback.hpp"

namespace duckdb {

class ClientContext;
class Optimizer;

namespace recycling {

// A page of an ordered result: `limit` rows after the first `offset`.
struct Page {
	idx_t limit = 0;
	idx_t offset = 0;
};

// The query result being planned: set before the optimizers run and taken once they have run.
struct PlannedQueryResult {
	string key;
	// Identifies the plan, since a statement may plan another one inside it.
	optional_ptr<Optimizer> optimizer;
	// When the statement is a page of an ordered result: the key of the ordered rows it reads, and the page.
	string prefix_key;
	Page page;
};

class RecyclingClientState : public ClientContextState {
public:
	PlannedQueryResult planned;

	// DuckDB calls OnFinalizePrepare only when some registered state can request a rebind. The price is that it plans
	// a copy of each parsed statement (ClientContext::CreatePreparedStatement), within noise on a 0.2 ms query.
	bool CanRequestRebind() override;
	// A write becomes visible to other connections when its transaction commits, but DuckDB calls
	// TransactionCommit only after that (TransactionContext::Commit). So a write is announced once its statement is
	// planned, before it executes, and nothing is replayed or published from then until the commit has invalidated
	// the cache or the transaction has rolled back. Unlike the optimizer hook, this runs for every plan, including
	// ones planned with the optimizer disabled. ATTACH and DETACH change which tables exist without modifying a
	// database, so they are announced explicitly.
	RebindQueryInfo OnFinalizePrepare(ClientContext &context, PreparedStatementData &prepared,
	                                  PreparedStatementMode mode) override;
	// DuckDB marks a transaction that writes data or changes the schema with the database it modified; writes to
	// temp objects are not marked, which is safe because scans of temp objects are never recycled. A rolled-back
	// transaction changed nothing and leaves the cache intact.
	void TransactionCommit(MetaTransaction &transaction, ClientContext &context) override;
	void TransactionRollback(MetaTransaction &transaction, ClientContext &context,
	                         optional_ptr<ErrorData> error) override;

	// A prepared plan can outlive the cache state it was planned against: a replay may serve a result
	// that a write has since invalidated. Re-planning each execution makes the hook decide again.
	// Ordinary statements are planned and executed once and reach neither rebind callback.
	// Client API: Prepare() then Execute()
	RebindQueryInfo OnExecutePrepared(ClientContext &context, PreparedStatementCallbackInfo &info,
	                                  RebindQueryInfo current_rebind) override;
	// SQL: PREPARE then EXECUTE
	RebindQueryInfo OnRebindPreparedStatement(ClientContext &context, BindPreparedStatementCallbackInfo &info,
	                                          RebindQueryInfo current_rebind) override;

private:
	// Announces at most once per transaction, however many of its statements write.
	void AnnounceWrite(ClientContext &context);
	void EndWrite(ClientContext &context);

	// Unsynchronized: DuckDB plans a connection's statements and ends its transactions only while holding that
	// connection's ClientContext lock.
	bool announced_write = false;
	bool attached_or_detached = false;
};

class RecyclingConnectionCallback : public ExtensionCallback {
public:
	void OnConnectionOpened(ClientContext &context) override;
};

// The connection's callbacks, registered on first use. Every connection must have them, whether or not it recycles:
// its writes must invalidate the cache that other connections share.
shared_ptr<RecyclingClientState> RegisterClientState(ClientContext &context);

} // namespace recycling
} // namespace duckdb
