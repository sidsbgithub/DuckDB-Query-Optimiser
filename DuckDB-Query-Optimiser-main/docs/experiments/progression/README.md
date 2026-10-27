# Progression: one benchmark of the milestone versions

This measures the version at the end of each part of the approach, against stock DuckDB, all in one session on one
machine (8-core Mac, 16 GB) on 2026-10-16.

## Method

- Each version was built from its commit with the pinned DuckDB v1.5.5 (`GEN=ninja make release`).
  - Stock DuckDB was built from the `duckdb/` submodule alone.
- `scripts/benchmark.py --runs 1` ran each binary in three rounds. The order rotated and reversed between rounds, so
  machine drift is spread across the versions.
- Each benchmark call clones the database, runs its own warm-up process, then one timed process: 149 public queries,
  six threads, 8000 MB.
- **Score** is the grader's: the geomean of the fastest whole run, with a 0.1 ms floor per query.
  - "Per-query min" is the geomean of each query's fastest time over the three runs.
  - The total, median and buckets come from the fastest run.
- Answers were not re-checked here. Each commit passed `IMDB_DATA=1 make test` when it was made.

## Results

| Version | Commit | Score, ms | Three runs, ms | Per-query min, ms | Total, s | Median, ms | < 1 ms | < 10 ms | ≥ 100 ms | Peak RSS, GiB |
| --- | --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Stock DuckDB | v1.5.5 | 42.11 | 42.97 / 46.19 / 42.11 | 39.57 | 35.05 | 52.4 | 13 | 33 | 51 | 4.60 |
| Pipeline breakers (spec #1) | `929a68d` | 33.69 | 33.73 / 33.69 / 36.41 | 31.18 | 33.95 | 43.1 | 17 | 41 | 51 | 5.07 |
| + Whole-query results, drill-downs, pages | `1227be4` | 18.52 | 18.52 / 19.13 / 21.11 | 17.58 | 24.85 | 25.8 | 29 | 56 | 42 | 4.49 |
| + Blocks and range subsumption | `584c815` | 6.11 | 6.11 / 6.67 / 6.78 | 5.88 | 4.78 | 6.7 | 33 | 86 | 12 | 2.95 |
| Today's main | `6baa6d3` | **5.86** | 5.86 / 6.48 / 6.00 | 5.65 | 4.16 | 6.7 | 34 | 86 | 11 | 2.68 |

- The milestones are stock DuckDB and the end of each of the three parts.
- The last milestone is today's main. It is `584c815` plus:
  - capturing a block only after counting it;
  - remembering size refusals;
  - the outside-filter fix (`429dc81`);
  - the refactors.
- `584c815` is an intermediate step. Today's main was faster in every round, but by less than the
  spread between rounds.
- Every version is faster than the one before it in every round. Today's main is 7.2× faster than stock DuckDB.
  - Its fastest run's total is 4.2 s, against 35.1 s for stock DuckDB.
- No query failed in any run.

The files are named `<version>-r<round>.json` and hold every query's time. `summary.json` holds the numbers in the
table.
