# Query Recycling

A DuckDB extension that speeds up a stream of repeated analytical queries against the IMDB database by reusing results of earlier queries, without ever changing an answer.

## Workload

**Query stream**:
The ordered sequence of SQL statements the grader executes, one statement per file, in filename order.
_Avoid_: Benchmark, query set (when the order matters)

**Run**:
One process executing the whole query stream from start to finish; nothing the extension keeps in memory survives from one run to the next.
_Avoid_: Pass, session, iteration

**Warm-up run**:
A run whose timings are thrown away, executed before the timed runs to warm the OS page cache.

**Vanilla answer**:
The result unmodified DuckDB v1.5.5 returns for a statement; the only definition of a correct answer.
_Avoid_: Expected output, ground truth

## Recycling

**Recyclable subtree**:
A fragment of a query plan whose output is eligible to be reused by later queries.
_Avoid_: Candidate, subexpression, fragment

**Query result**:
The whole result of a statement, recyclable from the statement's root (see ADR-0004); a later statement, or a subquery that repeats it word for word, replays it.

**Block**:
The joins and filters under a statement's select list or grouping (see ADR-0005); a block result replays for a later statement that reads any columns it holds, or whose ranges lie within its own.
_Avoid_: FROM clause, join tree, view

**Block family**:
The blocks that differ only in their constants; a captured block keeps every column its family's statements have read.

**Range**:
A WHERE conjunct of a block that keeps an integer or date column of a scan within constant bounds; a block's **base** is the block without its ranges.

**Recycled result**:
The stored, complete, immutable output of a recyclable subtree.
_Avoid_: Entry, materialization, intermediate, cached result

**Capture**:
Recording a recycled result while the query that produces it executes.
_Avoid_: Materialize, build, record

**Replay**:
Serving a recycled result in place of executing the recyclable subtree.
_Avoid_: Reuse (as a verb for this step), scan cache

**Hit**:
A recyclable subtree for which a matching recycled result exists and is replayed.

**Miss**:
A recyclable subtree for which no matching recycled result exists, so it executes normally.

**Skip**:
A pipeline breaker whose subtree is not recyclable, so it is neither looked up nor captured; counted under one skip reason (volatile, parameter, external scan, temp object, CTE reference, delim get, unsupported).

**Pipeline breaker**:
A plan operator that must consume its whole input before emitting output (aggregate, DISTINCT, hash-join build side, materialized CTE definition); where a recyclable subtree is rooted, apart from query results and blocks. Order-sensitive operators (ORDER BY, LIMIT, Top-N, window) never root one.

**Shared CTE definition**:
The subquery a WITH clause names, as DuckDB plans it once for all its references; recyclable as a whole, while subtrees that merely reference it are not.

**Recycling key**:
The canonical text form of a recyclable subtree; two subtrees match exactly when their recycling keys are equal.
_Avoid_: Signature, fingerprint, hash

**Invalidation**:
Discarding every recycled result when a transaction that wrote data or changed the schema commits; a rolled-back transaction changed nothing and invalidates nothing.
