# Recyclable subtrees are rooted at pipeline breakers

Recyclable subtrees are rooted only at pipeline breakers (aggregates, DISTINCT, hash-join build sides, materialized CTE definitions, and the query root), not at scans, filters or projections in isolation. A materialized CTE definition is eligible as a whole because DuckDB consumes it before evaluating its references; a subtree containing only a CTE reference is not eligible. The public workload's reuse is dominated by shared CTE-shaped subtrees (e.g. a DISTINCT set of US movies) rather than repeated scans; DuckDB already builds a pipeline breaker's output in memory, so capture is cheap; and replaying a filtered scan is barely faster than rescanning with zonemaps while adding copy overhead that the geometric-mean score punishes on cheap queries.

## Considered Options

- Scan/filter/projection subtrees (the original plan's first slice): kept only as an ablation experiment.
- Whole-query results only: subsumed, since the query root is itself a recyclable subtree.
