# Workload analysis: where recycling hits and misses

Issue #9. This report runs the public query stream on the IMDB database with diagnostics on, then ranks the remaining
coverage work by evidence. The raw tables, including every query's events, are in
[workload-analysis-data.md](workload-analysis-data.md).

## Method

`python3 scripts/workload_analysis.py` runs `benchmark/` (149 queries) with the grader's settings. Each run is its own
process, all runs share one copy-on-write clone of `data/imdb.duckdb`, and every statement runs under
`EXPLAIN (ANALYZE, FORMAT JSON)`:

1. a warm-up run, which is discarded;
2. a vanilla run (`waddle_recycling = false`), which provides latencies and full physical plans;
3. a recycling run with `waddle_recycling_diagnostics` on. It logs to files, and the script reads
   `waddle_recycling_stats()` after each statement. `QueryLog` entries tie each diagnostics line to its query.

The diagnostics now name the breaker kind (`hit breaker=aggregate key=…`, `skip reason=… breaker=distinct detail=…`).
Without that field, skips could not be counted per kind.

Hits, misses and skips come from the extension itself. The coverage estimates for breaker kinds that are not built
yet come from the **vanilla physical plans**. Each pipeline-breaker subtree gets a *plan shape*: its operators and
their printed details, without cardinality estimates and dynamic filters, and with CTE scans replaced by the definition
they read. A plan shape is coarser than a recycling key and is used only for this estimate. A subtree *repeats* when an
earlier query had the same plan shape. The subtree's estimated saving is its share of the query's operator time
multiplied by the query's latency. A repeated subtree inside another one that is already counted is not counted again.
Exact duplicate queries always produced identical plan shapes, which checks that the shapes are stable. Two kinds of subtree are left out because they do not
compute anything on their own: the duplicate-eliminating aggregate that each `DELIM_JOIN` keeps as a child, and
subtrees that read a `DELIM_SCAN` of a join outside them.

**Caveats.** Latencies are single samples. Two vanilla runs differed by 4% in geomean, and individual queries varied
by up to ±25% (some skip-only queries got slower although recycling did nothing for them). The savings are **upper
bounds**, because they assume a replay costs nothing. A replayed build side still builds its hash table, and replay
and capture have costs that #12 will measure. The cache never evicted in this run: peak usage was 105 MiB of the
128 MiB budget.

## Headline

| | Vanilla | Recycling |
|---|---:|---:|
| Geometric mean | 48.6 ms | 35.2 ms (−28%) |
| Total | 39.1 s | 33.1 s |

Of the 149 queries, 26 hit, 35 missed, 59 had only skips and 29 had no aggregate or DISTINCT at all. Queries with a
hit or a miss run at most one lookup, so there were 61 lookups (43% hits).

Not all of the −28% comes from recycling. The hit queries' geomean fell from 122.5 ms to 40.8 ms (0.33×), which
accounts for 60% of the geomean reduction. But queries that recycling never touched also ran faster in the later run:
0.79× for queries with no event and 0.86× for skip-only queries. Together they account for a third of the reduction.
That part is run-order noise. If only the hit queries had changed, the geomean would be **40.1 ms (−17%)**, which is
the better estimate of what recycling gains today.

## Hits, misses and skips per breaker kind

These are the breakers the hook examined:

| Breaker | Hits | Misses | Skips |
|---|---:|---:|---:|
| aggregate | 26 | 34 | 70 |
| distinct | 0 | 1 | 8 |
| hash-join build side | not implemented (#10) | | |
| shared CTE definition | not implemented (#11) | | |

At the analyzed commit the hook looked up **one** breaker per query: the first recyclable one it found, deepest first (since #1 it looks up nested subtrees outermost first; see [docs/experiments](experiments/README.md#follow-up-nested-recyclable-subtrees-1)). It also examines
nothing above a skipped breaker. Other breakers in the same query are therefore neither hits, misses nor skips. In the
physical plans a DISTINCT and an aggregate whose outputs were all pruned look alike, both being a `HASH_GROUP_BY`
without aggregates. The plan analysis therefore counts them together: 33 of them repeat exactly. Several are the
pruned `min()` subqueries that the hook counts as aggregate hits, as in q0082. Only 62 ms of their estimated saving
lies in queries that do not hit today, in q0054 and q0055 behind CTE-reference skips.

## Dominant miss reasons

| Reason | Misses | |
|---|---:|---|
| First occurrence (nothing similar seen before) | 34 | the capture that makes a later hit possible |
| Differing constants | 1 | q0091, the q0076 aggregate with `production_year BETWEEN 2006 AND 2006` in place of `2001 AND 2011` |
| Same input and groups, different aggregate outputs (pruned outputs) | 0 | |

Exact matching loses almost nothing on the aggregates it examines. Every miss except one is a first sighting, and the
feared cost of aggregate outputs pruned per parent does not appear in this stream. The losses are all in **skips**.

## Dominant skip reasons

| Cause | Skips | Queries |
|---|---:|---|
| Unsupported operator `DELIM_JOIN` (correlated `EXISTS` subquery) | 50 | q0226–q0293 |
| CTE reference | 13 | q0053–q0055 |
| Dynamic filter from outside the subtree | 9 | q0057, q0058, q0129, q0133, q0134, q0255, q0257, q0264, q0273 |
| Function bind data (`regexp_extract`) | 5 | q0125, q0126, q0129, q0133, q0134 |
| Unsupported operator `CHUNK_GET` (`VALUES`) | 1 | q0251 |

The `DELIM_JOIN` skips matter most. They hit the most expensive block of the stream: the 59 skip-only queries take
27.2 s of the 39.1 s vanilla total. The block contains exact duplicates that cannot hit today:

- q0248, q0284 and q0293 repeat q0240, and each takes 2.1–3.2 s;
- q0274 repeats q0265.

## Ranking of the remaining coverage work

Estimated from repeated physical subtrees, one breaker kind at a time (vanilla geomean 48.6 ms). The estimates are
upper bounds, and only their order of magnitude matters:

| Breaker kind | Subtrees | Exact repeats | … worth ≥ 1 ms | Est. saving | … in queries that do not hit today | Est. geomean if recycled |
|---|---:|---:|---:|---:|---:|---:|
| Aggregate over a correlated subquery (contains a `DELIM_JOIN`) | 50 | 4 | 4 | 7.9 s | 7.9 s | 38.1 ms (−22%) |
| Hash-join build side (#10) | 606 | 472 | 158 | 4.3 s | 3.5 s | 40.9 ms (−16%) |
| Aggregate (already recycled) | 73 | 13 | 12 | 0.9 s | 0.35 s | 36.2 ms |
| DISTINCT or group-only aggregate (already recycled) | 41 | 33 | 33 | 0.2 s | 0.06 s | 47.9 ms |
| Shared CTE definition (#11) | 12 | 7 | 6 | 0.07 s | 0.07 s | 48.3 ms (−0.6%) |

**Hash-join build sides are worth far more than shared CTE definitions.** 158 build sides worth at least 1 ms repeat
across the stream, and 3.5 s of their estimated saving lies in queries that do not hit today. Among those queries the
largest are q0240 (0.5 s), q0284, q0293, q0248 and q0235 (about 0.3 s each). The saving overlaps only a little with the
correlated-subquery aggregates below, which contain the build sides of q0248, q0284 and q0293. Build sides still add
3.4 s on top of them, and both together estimate a geomean of 32.2 ms. This is an upper bound: the join still builds its hash table
from the replayed rows, and the 314 repeats under 1 ms are small dimension scans (`kind_type`, `info_type`, …) that
replay overhead could make slower. #10 should therefore skip tiny build sides or show that replaying them costs
nothing.

Shared CTE definitions repeat only in q0053–q0055 and q0019. Their definitions are cheap (65 ms in total, at most
41 ms in q0055), because the expensive part of those queries is the outer query. The DISTINCTs inside those
definitions, which CTE-reference skips also block, add about 62 ms. #11 would lift the 13 CTE-reference
skips but change the geomean by under 1%, which is below the noise. It should come last, or be dropped if #12's
ablation confirms this.

**The largest single opportunity has no ticket yet**: recycling an aggregate whose subtree contains a whole
`DELIM_JOIN` together with its delim gets. Such a subtree is as context-free as any other, since the join owns its
`DELIM_GET`s. Only a `DELIM_GET` cut off from its join depends on context, and that stays ineligible. Adding it to the
recycling key would turn the four exact duplicates into hits (≈7.9 s, geomean −22%). It would also let the hook examine
the aggregates above these subqueries. After it, the next skip to fix is dynamic filters from outside the subtree
together with `regexp_extract` bind data: q0133 and q0134 alone account for about 0.33 s of repeated aggregate time.

Suggested order: `DELIM_JOIN` subtrees (new ticket) → hash-join build sides (#10) → dynamic-filter and bind-data
skips → shared CTE definitions (#11).

## Predicate containment: keep it deferred

Containment could only help subtrees that repeat *modulo constants*, and only when the new predicate selects a subset
of the old one and the recycled result keeps the filtered column.

- 78 subtrees repeat only modulo constants (1 aggregate, 27 aggregates over correlated subqueries and 50 build
  sides).
- Almost all change an equality constant, which gives disjoint results: `country_code` `'[gb]'` → `'[ca]'`, role
  `'actress'` → `'director'`/`'producer'`/`'writer'`, genre `'Action'` → `'Drama'`. Or they move a year range so that
  it widens or no longer overlaps: 2008–2013 → 2007–2013, 1998–2004, 1998–2003 or 2000–2000, and 1998–2003 →
  1998–2004. Containment serves none of these.
- The script marks a repeat *narrower* when every changed constant is a range bound that moved inward against some
  earlier query. Only **7 subtrees in 3 queries** qualify: q0091 (2001–2011 → 2006), q0234 (2008–2013 → 2010) and
  q0249 (a build side, 1998–2003 → 2000). Even if they cost nothing, they would save at most **253 ms** (0.6% of the
  stream's time) and bring the geomean to 44.6 ms.
- Neither of the two aggregates among them groups by `production_year` (q0076 groups by company name, q0227 by
  gender). Aggregate-level containment would therefore serve **0** of them. At most about 0.1 s of build sides
  (q0234, q0091, q0249) would remain, if their recycled results keep the year column.

In short, containment offers at most about 0.1 s and would need a subsumption checker, which is the riskiest code for
wrong answers. Exact matching of `DELIM_JOIN` subtrees and build sides offers about 11 s together (7.9 s and 3.4 s
more). Keep containment
deferred. Revisit it only if the private stream is known to narrow ranges on group keys.

## Other observations

- **Capture overhead is not measurable from one sample.** Queries that missed ran at 0.90× their vanilla time, against
  0.79× and 0.86× for untouched queries (see the headline). These numbers cannot separate capture cost from
  run-order noise. #12 must measure capture-without-replay directly.
- **Memory is not a constraint yet.** Without evictions, recycled results peaked at 105 MiB. Adding build sides will
  raise this, so #12's budget sweep should run after #10.
