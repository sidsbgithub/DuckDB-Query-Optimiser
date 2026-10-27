# Testing

```bash
make test        # release build
make test_debug  # debug build
```

## `sql/benchmark.test`

Correctness check for the benchmark. It loads your extension, attaches `data/imdb.duckdb` read-only, applies the leaderboard settings, and runs all 149 public queries, comparing each result against what vanilla DuckDB returned - the expected values were recorded with no extension loaded.

### How results are asserted

A query is asserted in one of three ways:

**Results written out in full.** Inline values go through `CompareValues`, which for numeric columns falls back to `Value::ValuesAreEqual` -> `ApproxEqual`, a **1% relative tolerance**. Every query whose result has FLOAT/DOUBLE columns is written out in full. At `threads = 6`, parallel aggregation sums in a nondeterministic order, so `avg(...)` drifts in the last few digits between runs. Small results are written out too, so most failures come with a readable diff.

**`N values hashing to <md5>`.** Used for the larger results, where writing every row would bloat the file. A hash bypasses `CompareValues`, so it is exact - which is why no query with float columns is hashed.

Six queries have more than one legal answer, so only their cardinality is a fact about them:

| Query | Why |
|---|---|
| `q0016` | `LIMIT 10`, no `ORDER BY` - 2,078 rows qualify, 10 come back arbitrarily |
| `q0122` | `LIMIT 30`, no `ORDER BY` - 521 rows qualify |
| `q0123` | `ORDER BY count(*) DESC LIMIT 40` - a 40-row tie group straddles position 40; 24 are dropped arbitrarily |
| `q0225` | `ORDER BY c DESC LIMIT 30` - a 29-row tie group straddles position 30 |
| `q0298` | `ORDER BY cnt DESC LIMIT 20` - a 3-row tie group straddles position 20 |
| `q0011` | `ORDER BY num_movies DESC LIMIT 20` - a 2-row tie group straddles position 20 |

Dropping the `LIMIT` + adding `ORDER BY ALL` removes the ambiguity and fixes the order. For `q0011` we use row count alone: its deterministic form is 32.9M rows (98,766,957 values).

### Regenerating it

`sql/benchmark.test` is generated. If the query set or the dataset changes:

```sh
python3 ./scripts/gen-benchmark-test.py
```
