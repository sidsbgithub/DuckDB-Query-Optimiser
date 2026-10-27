# Blocks are recycled by table columns and ranges

A **block** is the joins and filters under a statement's select list or grouping: the child of a projection or aggregate that is a filter, join or cross product with at least one join below it. Before the optimizers run, every block is keyed and marked with a `WADDLE_BLOCK` operator. A block result replays in place of the block when it holds every column the statement reads from it.

This departs from ADR-0002, which roots recyclable subtrees only at pipeline breakers. A dashboard repeats a block and reads other columns of it: counts by name, then by gender, then by role, then a page of rows. No pipeline breaker is shared between those statements. The join build sides they share differ in the columns they carry, and each still leaves the largest join to execute again. In the public stream, the tiles of a cast dashboard took 40–100 ms with breaker recycling alone and take 2–8 ms replaying the block. The cost is a materialization under the parent when a block is captured, which is why a block is captured only once its family has repeated (below).

## Keys

A block's key is DuckDB's serialization of the bound block, as for query results (ADR-0004), with two changes:

- **Scans by table.** A scan's list of columns is left out, and a column reference to a scan names the table column instead of the scan's position. The key does not depend on which columns the statement reads.
- **Ranges apart.** A WHERE conjunct at the block's root that keeps a scan column of an integer or date type within constant bounds (`=`, `<`, `<=`, `>`, `>=`, `BETWEEN`, `IS NOT NULL`) is a **range**. The key is the **base**, the block serialized without its ranges, followed by the ranges.

The **family** key is the base with every constant left out. Statements that differ only in their constants share a family.

## Capture and replay

- A block result holds a set of block columns, each a canonical table index and a table column. A statement's block replays it when it holds every column the statement reads.
- A block is captured once a statement repeats its key, or once its family has shown that its statements repeat blocks. The capture keeps every column its family has read, the columns of its ranges, and every column of tables of at most 1000 rows. Capturing a family's first block would cost a materialization for blocks that mostly never repeat.
- On a miss, a block replays the result of a captured block with the same base whose ranges contain its own: a one-year drill-down, or a later dashboard whose years lie within an earlier one's. The replay is filtered by the statement's own range conjuncts. When the block is to be captured, those filtered rows are captured as its own result.
- A large join build side inside a captured block is not captured as well. It would hold nearly the same rows.

## The marker

`WADDLE_BLOCK` exposes only the columns the statement reads, in block-column order. A pipeline breaker above a block describes it by its key and those columns, so the breaker's key is the same whether the block is joined, captured or replayed. Without this, capturing a block (which widens its scans) or replaying it would change the key of every breaker above it. The public stream's shared CTE aggregates stopped replaying until this was done.

The marker cannot be serialized or copied. An optimizer that copies a plan (CTE inlining) leaves a materialized CTE as it is instead.

## Consequences

- Replayed rows come in another order, so a block is recycled under a projection only when `preserve_insertion_order` is off. Under an aggregate the order does not matter.
- Block lookups count with the breaker counters (`lookups`, `hits`, `misses`, `captures`). Blocks are keyed in the same hook as query results, so `waddle_recycling_query_results = false` turns them off too.
- Families and captured bases are remembered for the whole run as hints; a block result is still looked up and validated in the cache.
- Block results made the public stream evict under the old 1 GiB budget, so the budget is 2 GiB.
