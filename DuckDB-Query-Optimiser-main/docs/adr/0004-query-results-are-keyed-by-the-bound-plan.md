# Query results are keyed by the bound plan, before the optimizers run

A statement's whole result (its **query result**) is looked up before DuckDB's optimizers run, from a key that is DuckDB's own binary serialization of the bound plan. Table indices are renumbered in the order they first appear, and query locations (character offsets in the SQL text) are left out. On a hit the bound root is replaced by the replay scan, so the optimizers only see a scan. On a miss the key is remembered and the optimized root is wrapped in the capture operator after the optimizers have run.

The same lookup runs for every subtree of the bound plan that could root a statement (projection, ORDER BY, LIMIT, DISTINCT, set operation). A subquery that repeats an earlier statement word for word therefore replays that statement's result. The public stream's drill-downs do exactly this, for example `SELECT * FROM (<q0231>) d WHERE d.role = 'actress'`. It has to happen before the optimizers run: filter pushdown would move the drill-down's filter into the repeated statement, and after that its subtree no longer matches.

This departs from ADR-0003 in two ways:

- **A hook before the optimizers.** Pipeline breakers are still keyed after the optimizers (ADR-0003), since their capture must describe the optimized subtree it stores. Query results can be keyed earlier because the capture sits above the whole statement: nothing the optimizers do changes what the root returns.
- **DuckDB's serialization instead of our own key text.** Queries in the stream use correlated `EXISTS` (DELIM joins), `IN` lists planned as `CHUNK_GET` MARK joins, and functions with bind data. The hand-written key describes none of these. Serialization describes every operator DuckDB can plan, completely, because DuckDB can rebuild the plan from it. A bound plan that fails to serialize is simply not recycled.

## What makes a query result recyclable

Every operator must be a relational operator. Every scan must read a DuckDB table that is not temporary. Every expression must be consistent (no `random()`, `now()` or sequences) and parameter-free. A DELIM_GET is allowed only inside the delim side of a DELIM_JOIN within the same subtree. Every table index the subtree refers to must be defined inside it. The key also records whether `preserve_insertion_order` is on.

An ordered result is captured and replayed on one thread, so its order is kept. The capture decides this from the physical plan below it (`PhysicalPlanGenerator::PreserveInsertionOrder`).

## Consequences

- Invalidation, write-in-flight blocking, the byte budget and LRU eviction apply to query results as they do to pipeline breakers: the same cache holds both.
- The counters `query_lookups`, `query_hits` and `query_captures` are kept apart from the breaker counters. `waddle_recycling_query_results = false` turns query results off; the breaker tests do so to keep observing breakers.
- A hit still pays for parsing and binding, about 0.5 ms for the stream's largest statements.
