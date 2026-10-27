# Public query stream experiments for issue #12

## Method

The 149 public statements ran in filename order on `data/imdb.duckdb`, using DuckDB v1.5.5, six threads, an 8000 MB memory limit, `preserve_insertion_order = false`, and `EXPLAIN (ANALYZE, FORMAT JSON)`. Each configuration had a separate warm-up process and two timed processes. Each process used a fresh in-memory recycling cache; all processes for a configuration shared one copy-on-write database clone. The score is the lower geometric mean of the two timed runs, with a 0.1 ms floor per query. The JSON files hold every query's latency and each run's cache counters and sampled peak process RSS. RSS is sampled at roughly 100 ms intervals, so a shorter peak can be missed.

The experiments above the follow-up section used the source at `384fc5b` plus the benchmark harness and compile-time ablation guards in this change. The vanilla binary was built from the pinned `duckdb/` v1.5.5 source with its default `core_functions` and `parquet` extensions, without `waddle`. The disabled configuration used the extension-linked binary with `SET waddle_recycling = false`. Capture-without-replay used `SET waddle_recycling_max_bytes = 0`: eligible subtrees are captured, but no recycled result can fit in the cache. Each breaker-kind ablation used the compile-time `WADDLE_ABLATION` option in `CMakeLists.txt`, rebuilding the extension-linked binary after selecting one kind.

Correctness was checked independently with `IMDB_DATA=1 make test`. Timings alone do not establish answer equivalence.

## Baselines and capture overhead

Each row's total, RSS and counters belong to the run with the lower geomean. All runs produced timings for all 149 queries, with no reported query failure. Values in the two geomean columns are in run order, in milliseconds.

| Configuration | Geomean runs, ms | Best-run total, s | Peak RSS, GiB | Hits / lookups | Cache peak, MiB |
| --- | ---: | ---: | ---: | ---: | ---: |
| [Vanilla](vanilla.json) | 44.18 / 43.16 | 37.81 | 4.69 | — | — |
| [Extension disabled](disabled.json) | 43.05 / 43.38 | 34.59 | 4.16 | 0 / 0 | 0 |
| [Capture without replay](capture-only.json) | 43.98 / 46.44 | 36.01 | 4.65 | 0 / 129 | 0 |
| [Recycling enabled, 128 MiB](enabled-128m.json) | 42.45 / 43.70 | 34.56 | 4.41 | 95 / 129 (73.6%) | 98.5 |

Capture without replay aborted all 129 captures because the budget was zero. Its faster geomean is 0.93 ms above the disabled extension, an estimate of capture overhead. Enabled recycling's faster geomean is 0.71 ms below vanilla and 0.60 ms below disabled. Those differences are smaller than the 1.02–1.25 ms between the two timed runs of vanilla and enabled, so the global gain is not established above run noise.

## Budget sweep and default

| Budget | Geomean runs, ms | Best-run total, s | Hits / lookups | Evictions | Aborted captures | Cache peak, MiB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| [8 MiB](budget-8m.json) | 44.62 / 43.57 | 36.14 | 91 / 129 | 16 | 5 | 8.0 |
| [32 MiB](budget-32m.json) | 45.23 / 42.12 | 32.58 | 94 / 129 | 8 | 2 | 32.0 |
| [64 MiB](budget-64m.json) | 45.22 / 42.55 | 34.02 | 95 / 129 | 10 | 0 | 64.0 |
| [128 MiB](enabled-128m.json) | 42.45 / 43.70 | 34.56 | 95 / 129 | 0 | 0 | 98.5 |
| [256 MiB](budget-256m.json) | 43.54 / 46.39 | 36.95 | 95 / 129 | 0 | 0 | 98.2 |

**Chosen default: 128 MiB** (`waddle_recycling_max_bytes = 134217728`). It is the smallest tested budget with no evictions or aborted captures. Raising it to 256 MiB retained no more hits. The 32 and 64 MiB best runs were close, but both evicted results and their run-to-run spread exceeds the difference from 128 MiB. The default was already 128 MiB, so no setting change is needed.

## Breaker-kind ablations

Each row disables one kind and leaves the others enabled. The effect includes which other subtree is selected when that kind is absent; it is a marginal, interacting effect, not an isolated operator microbenchmark.

| Configuration | Geomean runs, ms | Best-run total, s | Hits / lookups | Cache peak, MiB | Decision |
| --- | ---: | ---: | ---: | ---: | --- |
| [All kinds enabled](enabled-128m.json) | 42.45 / 43.70 | 34.56 | 95 / 129 | 98.5 | Reference |
| [No aggregate roots](ablate-aggregate.json) | 45.59 / 44.44 | 38.65 | 94 / 122 | 60.7 | Keep aggregate recycling; both runs regress |
| [No DISTINCT roots](ablate-distinct.json) | 43.73 / 43.60 | 34.59 | 95 / 129 | 97.5 | **Flag DISTINCT for removal review**: counters are unchanged and gain is below noise |
| [No hash-join build sides](ablate-join-build.json) | 36.44 / 38.98 | 35.60 | 28 / 64 | 106.7 | **Flag join-build recycling for removal or reprioritization**: both geomeans improve substantially |
| [No shared CTE definitions](ablate-cte.json) | 42.87 / 48.39 | 33.40 | 96 / 129 | 78.7 | **Flag CTE recycling for removal review**: one run is near the reference; gain does not beat noise |

Join-build recycling has a score tradeoff: without it, aggregate hits return in q0085, q0088 and q0090, dropping their median times from 76.4, 93.7 and 47.9 ms to 0.9, 1.0 and 1.3 ms. Yet the no-join build takes longer on some costly queries (for example q0293), so its best-run total is 1.04 s higher. The grader scores geomean, which favors the no-join build in both timed runs. This is the clearest feature to revisit before delivery. The ablation runs measured timing and cache events; their SQL answers were not separately checked against vanilla.

## Worst per-query regressions

These are the largest increases in the median of the two per-query timings with all kinds enabled versus vanilla. The [raw JSON](enabled-128m.json) preserves both timings for every query; these differences should be read as observations, not proof of causality.

| Query | Vanilla median, ms | Enabled median, ms | Increase |
| --- | ---: | ---: | ---: |
| q0231 | 2261.2 | 2567.8 | 306.6 ms (1.14×) |
| q0284 | 2804.4 | 3076.3 | 271.9 ms (1.10×) |
| q0239 | 779.9 | 942.8 | 163.0 ms (1.21×) |
| q0235 | 443.3 | 547.5 | 104.3 ms (1.24×) |
| q0011 | 620.3 | 710.6 | 90.3 ms (1.15×) |

## Follow-up: nested recyclable subtrees (#1)

The ablations above traced join-build recycling's cost to the hook's policy, not to build sides as such. The hook looked up one subtree per query, the deepest one, so a small build side inside an aggregate pre-empted the aggregate: q0085, q0088 and q0090 replayed only a dimension filter instead of their whole result. The hook now looks up recyclable subtrees **outermost first**. A hit replays the whole subtree and nothing inside it is looked up. A miss looks inside for smaller hits, and every subtree that missed is captured. An exact duplicate therefore replays its outermost breaker, and a query that shares only an inner aggregate or build side still hits it.

Same method as above, all configurations measured in one session on the same machine: vanilla, the previous policy (`a65aded`) and the nested policy at three budgets. Timings from different sessions are not compared: the vanilla geomean here is 2.8 ms above the earlier session's.

| Configuration | Geomean runs, ms | Best-run total, s | Peak RSS, GiB | Hits / lookups | Evictions | Aborted captures | Cache peak, MiB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| [Vanilla](nested-vanilla.json) | 48.17 / 45.97 | 36.68 | 4.61 | — | — | — | — |
| [Previous policy, 128 MiB](nested-old-policy.json) | 45.90 / 43.73 | 34.98 | 4.73 | 95 / 129 | 0 | 0 | 97.7 |
| [**Nested, 128 MiB**](nested-128m.json) | **36.12** / 36.44 | 36.15 | 4.69 | 193 / 310 | 64 | 1 | 128.0 |
| [Nested, 256 MiB](nested-256m.json) | 36.93 / 36.80 | 37.96 | 4.46 | 194 / 306 | 28 | 0 | 256.0 |
| [Nested, 512 MiB](nested-512m.json) | 35.56 / 36.25 | 36.64 | 4.69 | 194 / 306 | 0 | 0 | 478.7 |

- **Geomean:** the nested policy's faster run is 7.6 ms (17%) below the previous policy and 9.9 ms (21%) below vanilla. Both timed runs agree, and the gain is several times the run-to-run spread (0.1–2.2 ms). The aggregates that build sides used to pre-empt hit again: q0085, q0088 and q0090 drop from 77, 91 and 39 ms to about 1 ms each. That removes the reason join-build recycling was flagged, so it stays.
- **Total:** the best-run total is 1.2 s above the previous policy and 0.5 s below vanilla. The loss sits in the costly correlated-subquery block (q0222, q0231, q0232, q0242, q0284), where more subtrees are now captured; the geomean, which the grader scores, still favors the nested policy.
- **Budget:** 128 MiB stays the default. The cache now fills and evicts, but 512 MiB, with no evictions, keeps only one more hit, and the three budgets' geomeans lie within run noise of each other.
- **Correctness:** `IMDB_DATA=1 make test` passes with the nested policy.

Worst per-query regressions of the nested policy against vanilla (median of the two timed runs): q0284 +93 ms (1.03×), q0222 +87 ms (1.19×), q0232 +69 ms (1.07×), q0291 +61 ms (1.12×), q0280 +42 ms (1.18×). The largest gains: q0293 −681 ms, q0231 −510 ms, q0248 −384 ms, q0132 −316 ms, q0240 −290 ms.

The breaker-kind ablations above predate this policy and were not repeated. DISTINCT and shared CTE definitions remain flagged for removal review; they are kept because the spec asks for them and each adds only a few lines to the adapter.

## Reproduce

```sh
cmake -S duckdb -B /tmp/duckdb-vanilla-155 -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_UNITTESTS=OFF
cmake --build /tmp/duckdb-vanilla-155 --target duckdb
make release
IMDB_DATA=1 make test
python3 scripts/benchmark.py --binary /tmp/duckdb-vanilla-155/duckdb --runs 2 --json-out docs/experiments/vanilla.json --slowest 0
python3 scripts/benchmark.py --runs 2 --setup-sql 'SET waddle_recycling = false' --stats --json-out docs/experiments/disabled.json --slowest 0
python3 scripts/benchmark.py --runs 2 --setup-sql 'SET waddle_recycling_max_bytes = 0' --stats --json-out docs/experiments/capture-only.json --slowest 0
python3 scripts/benchmark.py --runs 2 --stats --json-out docs/experiments/enabled-128m.json --slowest 0
```

To build a breaker-kind ablation, run `cmake -S duckdb -B build/release -DWADDLE_ABLATION=AGGREGATE` (or `DISTINCT`, `JOIN_BUILD`, `CTE`), then `cmake --build build/release --target duckdb` and the benchmark command with `--stats`. Reconfigure with `-DWADDLE_ABLATION=` and rebuild before testing or producing the final binary. The runner samples peak RSS when Python's `psutil` package is available; without it, `peak_rss_bytes` is null.
