# Two plan rewrites before the optimizers

Before DuckDB's optimizers run, the pre-optimize hook rewrites two plan shapes that the public stream's dashboards use in nearly every statement. Neither is recycling. Both make the first execution cheaper and give the rest of the plan keys that later statements repeat.

## Unused LEFT JOINs are removed

A LEFT JOIN whose right side nothing above reads, and whose right side is unique on the join's equality keys, returns exactly its left input. Such a join is removed. Uniqueness is proven only through projections, filters and an aggregate that groups by every equality key (one grouping set). Dashboards' detail pages LEFT JOIN a grouped biography, plot or rating subquery and read none of its columns; DuckDB keeps some of those joins.

Removing the join must not suppress an error that DuckDB would raise. DuckDB evaluates the right side as far as the join conditions read it: it removes projected and aggregated columns that nothing reads without evaluating them. The rewrite therefore keeps the join if an expression that the join conditions read, transitively, can throw, or if a condition itself can throw.

## Equality EXISTS loses its DELIM join

DuckDB plans a correlated `EXISTS (... WHERE inner.k = outer.k ...)` as a semi or anti DELIM join whose right side joins the distinct outer keys (a DELIM_GET) back to the subquery. When that inner join compares every delim column by equality with a column of an independent relation, the rewrite replaces the join to the DELIM_GET with that relation. `=` becomes an `IS NOT NULL` filter plus the outer join's `IS NOT DISTINCT FROM`. DuckDB's own deliminator keeps the DELIM join when the relation has a filter. That barrier kept the selective existence check out of join ordering, and its DELIM_GET made every subtree above it unrecyclable. Filter pushdown runs first, so that a mark join used only as a filter is already a semi join.

## Consequences

- The rewrites run only where a query result may be recycled: `waddle_recycling` and `waddle_recycling_query_results` on, an auto-commit transaction, and a recyclable statement (ADR-0004).
- The query result's key is computed after the join removal and before the EXISTS rewrite. The key describes the statement as it will run.
- `test/sql/recycling_unused_join.test` and `test/sql/recycling_exists.test` cover duplicates, NULLs, `NOT EXISTS`, grouping sets, throwing expressions and verification.
