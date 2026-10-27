# Query recycling implementation plan

Status: milestones 0–3 implemented; delivery (milestone 4) in progress. Measurements are in [docs/experiments/](docs/experiments/README.md). Vocabulary is defined in [CONTEXT.md](CONTEXT.md); the reasoning behind structural decisions lives in [docs/adr/](docs/adr/).

## 1. Goal

Add query recycling to the `waddle` extension: capture recycled results of recyclable subtrees within a memory budget and replay them in later queries of the same run.

Priorities: **correctness > maintainability > speedup.** Every team member must be able to explain a hit, miss, capture, eviction and invalidation from the code. Prefer fewer concepts, settings and policies over marginal speed.

| Rubric | Focus | Evidence |
|---|---|---|
| Understanding (50%) | Small architecture, documented tradeoffs | Presentation, ADRs, plan walkthroughs |
| Result (25%) | Vanilla answers, fast query stream | Correctness, per-query times, geomean, total |
| Quality (25%) | Generic matching, bounded cache, clean modules | Focused modules, real tests, ablations, LOC vs. speedup |

## 2. Constraints

- Keep the name `waddle`; build against DuckDB **v1.5.5**. Runtime extension changes belong in `src/`, `test/`, and `CMakeLists.txt`. Delivery measurements may also change the benchmark harness under `scripts/` and add records under `docs/`. No core patches or workflow/version changes.
- Grader: `threads = 6`, `memory_limit = '8000MB'`, `preserve_insertion_order = false`; the query stream runs twice, each run in its own process; the faster run's geomean counts. **Wrong answers rank before speed.**
- Every statement runs under `EXPLAIN (ANALYZE, FORMAT JSON)`, so capture happens during execution, not result fetching.
- The score is the profile's `latency`, whose timer starts **before planning** (v1.5.5 `client_context.cpp`), so optimizer-hook work counts. Queries without a pipeline breaker must leave the hook almost immediately.
- Geomean with a 0.1 ms floor: overhead on cheap queries costs as much as savings on expensive ones.
- No hard-coded query IDs, constants or answers — the private set only shares patterns.
- Recycled results live within one run only ([ADR-0001](docs/adr/0001-recycled-results-live-within-one-run.md)).
- The grader uses the last *successful CI build*, not the latest commit.

## 3. Scope

**First version** ([ADR-0002](docs/adr/0002-recyclable-subtrees-are-rooted-at-pipeline-breakers.md)): recyclable subtrees rooted at pipeline breakers (aggregate, DISTINCT, hash-join build side, shared CTE definition) and at the query root; exact matching by recycling key; capture on first sight; byte-capped LRU eviction; invalidation on writes.

**Deferred, decided by measurement:** predicate containment for queries that differ only in constants (measure exact-match hit rate on the public set first), value-density eviction (only if evictions measurably cost hits), scan/filter recycling (ablation only).

**Out of scope:** persistence across runs, disk spill, learned policies, join reordering.

## 4. Correctness rules

Replay only when the recycling keys are equal, the subtree is deterministic, and the recycled result is complete. **If unsure, run the original plan.**

Never recyclable:
- Order-sensitive roots: ORDER BY, LIMIT, Top-N, window. Pipeline breakers *below* them still are.
- Subtrees containing a CTE reference or delim get (they depend on context outside the subtree). The shared CTE's definition itself is recyclable.
- Volatile functions, unbound parameters, table-function/external scans, temp objects.

Invalidation: any statement that writes data or changes the schema discards every recycled result when its transaction commits. `SET`, `PRAGMA`, read-only statements and rolled-back writes do not. A query whose transaction began before the last invalidation neither replays nor publishes.

## 5. Architecture

DuckDB internals are confined to one adapter module ([ADR-0003](docs/adr/0003-duckdb-internals-confined-to-one-adapter.md)).

| Module | File | Responsibility | Includes DuckDB planner headers? |
|---|---|---|---|
| Registration | `src/waddle_extension.cpp` | Settings, stats function, hook registration | Minimal |
| Adapter | `src/recycling_adapter.cpp` (front door) and `src/adapter/` (ADR-0007) | Optimizer hook and whole query results; shared state, per-connection callbacks, operators, plan keys, pipeline breakers, plan rewrites, blocks | **Yes — only here** |
| Recycling key | `src/recycling_key.cpp` | Canonical text form, binding renumbering | No (plain structs in) |
| Cache | `src/recycling_cache.cpp` | Recycled results, byte accounting, LRU `evict()`, invalidation, stats | No |
| Tests | `test/sql/recycling_*.test` | Behavior and invariants through the SQL seam | — |

- **Recycling key:** one recursive function prints operators, table/column IDs, types, typed constants and expressions with column bindings renumbered, so aliases and whitespace still hit; different constants, types or join kinds miss. Match = full string equality. Keys are printable in diagnostics. The key is built from the *optimized* plan, so it must handle aggregate outputs pruned per parent, dynamic filters pushed into scans, and compressed-materialization projections.
- **Capture/replay:** a miss wraps the subtree in a materializing capture operator (a pipeline breaker): each thread stores chunks locally, `Combine` merges them, and the sink `Finalize` publishes the recycled result, then the operator emits it to its parent. Errors, cancellation or exceeding the size cap discard it. A pass-through operator is not used, because it cannot tell a complete input from one cut short by a `LIMIT` ([ADR-0003](docs/adr/0003-duckdb-internals-confined-to-one-adapter.md)). Plain `EXPLAIN` never captures. A hit swaps the subtree for a table-function scan over the recycled result, whose bindings are remapped onto the parents.
- **Nested subtrees:** the hook looks up recyclable subtrees outermost first. A hit replays the whole subtree, so nothing inside it is looked up; an exact duplicate query therefore replays its outermost breaker, not a build side inside it. A miss looks inside for smaller hits, and every subtree that missed is captured, so a later query that shares only an inner subtree (the same aggregate or build side under a different parent) still hits. A skip is counted once, at the innermost skipped subtree.
- **Ownership:** the cache lives in the database's `ObjectCache` (non-evictable entry), never in a static, so recycled results are freed before the buffer manager.
- **Lifecycle:** only complete recycled results are visible; a result evicted while being replayed is freed after its last reader. Never hold the cache lock while executing or scanning.
- **Memory:** recycled results count against `waddle_recycling_max_bytes`, inside the 8000 MB budget. If a capture would exceed the per-result cap, abandon it and let the query continue.

## 6. Settings and stats

- `waddle_recycling` — master switch; off means vanilla behavior.
- `waddle_recycling_max_bytes` — cache budget; default chosen from a sweep.
- `waddle_recycling_diagnostics` — log recycling keys, hits, misses and skip reasons.
- `waddle_recycling_stats()` — table function: lookups, hits, misses, skip reasons, captures, aborted captures, evictions, invalidations, current/peak bytes.

Correctness checks and the memory cap are never switchable. Ablations use temporary branches or compile-time switches, not permanent settings.

## 7. Milestones

0. **Baseline & spike** — init submodules, build, run tests; confirm the v1.5.5 optimizer hook, table-function replay and extension-operator capture work under `EXPLAIN ANALYZE`; list which pipeline-breaker subtrees repeat in the public stream. *Hook spike done* (branch `prototype/recycling-hooks`): hook and replay confirmed; capture must be a sink operator. Repeating subtrees in the public stream: [docs/workload-analysis.md](docs/workload-analysis.md).
1. **Vertical slice** — aggregate/DISTINCT roots, exact recycling key, cache with LRU, master switch, stats. *Gate:* miss/capture/hit tests pass; disabled mode is vanilla; `benchmark.test` passes.
2. **Hardening** — eligibility rules from §4, invalidation, aborted captures, 6-thread capture, readers vs. eviction. *Gate:* no stale or partial replays; budget respected.
3. **Coverage** — hash-join build sides, shared CTE definitions, then measure whether containment or value-density eviction is worth it. *Gate:* correctness holds; each feature justified by an ablation.
4. **Delivery** — full correctness run, final experiments, remove dead code, slides, confirm the CI build of the intended commit.

## 8. Testing

Test-first per behavior, through **one seam: the extension's SQL surface**. Each test is a sqllogictest that runs statements with recycling on, compares answers against vanilla, and asserts the expected hits, misses, captures, aborted captures, evictions and invalidations through `waddle_recycling_stats()`. There is no separate C++ unit-test target: most recycling-key risk sits in the adapter's handling of optimized plans, which hand-built unit inputs would never exercise. ADR-0003's no-DuckDB-types boundary remains a design rule, not a test seam.

- **Recycling key** (highest value: a false match is a wrong answer): alias/whitespace hit; constant, type, join-kind and column-order miss; self-joins; NULL and string constants; the same breaker under different parents.
- **Cache:** a small `waddle_recycling_max_bytes` makes evictions and aborted captures observable; LRU order shows as which query hits after an eviction; writes invalidate.
- **Capture safety:** LIMIT above a breaker captures completely; an error inside the subtree publishes nothing; plain `EXPLAIN` never captures.
- **Semantics:** NULLs, duplicates, empty inputs, decimals, casts; volatile functions skipped; 1 vs. 6 threads; plain SQL and `EXPLAIN ANALYZE`.
- **Correctness gate:** the untouched `benchmark.test` (`IMDB_DATA=1 make test`).

```sh
make
IMDB_DATA=1 make test
make benchmark BENCH_ARGS="--runs 3 --slowest 20"
```

## 9. Experiments

Compare vanilla, extension disabled, and recycling enabled under grader settings, plus capture-without-replay overhead and per-feature ablations. Record commit, settings, per-query times, geomean, total, correctness, peak memory, hit rate and evictions. Report the worst per-query regressions. Flag a feature for removal when its gain does not beat noise relative to its complexity. Change the default feature set after the candidate passes the full vanilla-answer gate and a CI build; record the tradeoff when geomean and total time point in different directions.
