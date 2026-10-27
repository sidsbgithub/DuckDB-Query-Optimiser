# The adapter is split by mechanism behind one front door

ADR-0003 confines DuckDB's planner, optimizer and execution internals to one adapter. By October that adapter was one
2,856-line file with four recycling mechanisms and their shared infrastructure in it. It is now the front door and
nine header/source pairs in `src/adapter/`, and they are still the only DuckDB-facing code.
`src/recycling_adapter.cpp` is the front door: the only file the extension calls. It registers the hook, the settings
and the callbacks, recycles whole query results and pages (ADR-0004), and its two hook functions run the passes in a
fixed order. `src/adapter/` holds the rest:

- shared: `recycling_state` (the database-wide state and the vocabulary of recycled results), `recycling_settings`
  (each setting's name, default and accessor), `client_state` (invalidation and re-planning per connection),
  `operators` (`WADDLE_CAPTURE`, `WADDLE_BLOCK`, the replay scan, and the plan edits that put them in place) and
  `plan_keys` (eligibility and keys from DuckDB's serialization);
- one module per mechanism: `pipeline_breakers` (ADR-0002, with its `KeyNode` key), `blocks` (ADR-0005, with its
  block key, and `block_ranges` for the range algebra only it uses) and `plan_rewrites` (ADR-0006).

A mechanism never includes another mechanism's header, and shared modules never include a mechanism's. Internal
headers sit next to their source files, off the public include path; internal names live in `duckdb::recycling`,
and a class two files share is never in an anonymous namespace, because `dynamic_cast` across files would then fail
and every hit would become a miss. Only the front door knows the order in which the passes run.

The split moved code and changed nothing else: a script checks that every line of the old file is in exactly one new
file in its old order, and answers, recycling counters and recycling keys were compared against the previous build.
Dead code and duplicated sequences were removed afterwards in separate commits, each merged sequence into the module
that owns it.

## Considered Options

- Keep one file: rejected. A reader had to find one mechanism among 122 definitions, and every change touched the
  same file.
- Split by technical layer (state, policy, execution, analysis, passes): rejected. A change to one mechanism would
  touch up to seven files, and the layer order (policy below execution) needs its own explanation.
- One module per mechanism with a thin hook and private key modules (thirteen modules): deferred. It needs
  control-flow changes in the pre-optimize hook and a new key API; this split gets the same locality with code moved
  as it is.

## Consequences

- An engine upgrade touches `src/recycling_adapter.cpp` and `src/adapter/`, not one file. `recycling_cache` and
  `recycling_key` stay DuckDB-free; only `pipeline_breakers.cpp` uses `recycling_key`.
- A new adapter file must be added to `EXTENSION_SOURCES`.
- The extension compiles as C++11 (DuckDB's cached `CMAKE_CXX_STANDARD` wins): `RecyclingLogType`'s static members
  keep one out-of-line definition, in `recycling_state.cpp`.
- Calls across files are not inlined. Only planning pays for this; per-row code stays within `operators.cpp`.
