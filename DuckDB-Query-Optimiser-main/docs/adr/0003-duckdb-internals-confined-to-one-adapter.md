# DuckDB internals are confined to one adapter module

Only the adapter module includes DuckDB planner and execution headers. It hooks the optimizer, replays a recycled result through a table function (a normal scan in the plan), and captures through a materializing extension operator: a pipeline breaker that stores its input and publishes the recycled result in its sink `Finalize`, which DuckDB calls only after every thread has sunk and combined successfully. The recycling key, cache and admission/eviction modules work on plain types and never touch DuckDB plan objects, so they can be unit-tested and explained without DuckDB internals, and an engine upgrade touches one file. The cache is owned by the database instance (DuckDB's `ObjectCache`), not by a static, because recycled results hold buffer-manager blocks that must be freed before the database shuts down.

Verified on DuckDB v1.5.5 by the throwaway spike on branch `prototype/recycling-hooks` (`docs/prototype-recycling-hooks-findings.md`).

## Considered Options

- Pass-through (streaming) capture operator: rejected after the spike. A streaming operator cannot tell a fully consumed input from a pipeline cut short by a `LIMIT` above it: DuckDB still finalizes the operator, so it published a partial (empty) recycled result and a later query answered 0 instead of 50000.
- Custom physical operators for both capture and replay: more internals code for no measured gain.
- Structural hash plus recursive `Equals` on DuckDB objects instead of a text recycling key: couples matching to engine classes and cannot be printed or pasted into a test.

## Consequences

- Capture materializes the whole output of the pipeline breaker even when a `LIMIT` above it reads only a few rows. This is correct, but it is paid on every miss.
- The capture operator and the replay scan are not serializable: the replay's recycled result exists only in this process's memory, and the adapter registers no deserializer for the capture. Both live only between planning and executing one statement, so both opt out of plan verification's serialization round trip (`SupportSerialization`, `verify_serialization`), as DuckDB allows for operators it cannot serialize. The rest of `PRAGMA enable_verification` still applies: debug builds check the rewritten plan's column bindings right after the hook, and each statement is re-planned several ways with every answer compared (`test/sql/recycling_verification.test`).
- The optimizer hook runs after DuckDB's own optimizers, so the recycling key is built from optimized plans: aggregate outputs pruned per parent, dynamic filters pushed into scans, and compressed-materialization projections all appear in the subtree.

Amended by ADR-0007: the adapter is now `src/recycling_adapter.cpp` and the files in `src/adapter/`; an engine upgrade touches only those.
