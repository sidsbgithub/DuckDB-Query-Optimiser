---

# DuckDB database code reviewing criteria

Please read you new code / PR through the lense of this reviewer's criteria
for high-quality code. This list was extracted from thousands of interactions 
of a senior reviewer in a database company. Read your code with this list
in mind before you mark it ready for review (even if it passes all tests).

---

## 1. Justify every new thing. Expect the "why?"

Before you add **anything** - a parameter, a field, a helper method, a branch, 
a config option, an atomic, a copy, a log line, a try/catch - ask whether it 
earns its place. If the answer is "just in case", delete it.

Watch out for:

- **New helper methods that add no behavior.** "I don't see the need to put
  an extra method here. Why do [you] pass it with `X` further below?"
- **New fields that duplicate existing ones.** "Even a temp catalog has a
  CatalogRef... I think having both fields is weird."
- **Extra "has_XYZ" booleans next to optional pointers** e.g. when the
  serialization framework already has `ReadOptionalPtr`/`WriteOptionalPtr`.
- **Fallbacks and defaults specified in multiple places.** "Too many places
  where we have defaults specified. Why do we need this?"
- **Unused return values** from newly-introduced methods. "Is the return
  type used somewhere? Can this method be inlined?"

If the answer to any "why is this here?" is "for future flexibility", cut
it. Reintroduce when there's a concrete need.

## 2. Don't special-case through every caller. Centralize the branching.

When a new variant appears (e.g. a "temp catalog" alongside regular
catalogs, a new operator type), the temptation is to add `if (special) ...` 
at each site that touches it.  You should however fold that logic into a
shared method.

- "Should we just reuse `AddCatalogTransaction`, and then check in that
  method if `client_db_name == TEMP_CATALOG`? Otherwise we will have to
  special-case every caller."
- "I've made the comment before: Let's just use a single method
  (`AddCatalog`), and do the if/then there."
- "Maybe add a `UseDuckLakeMaintenance()` function instead, fully
  encapsulating the logic here."

Corollary: if the new variant doesn't need to flow through a given layer at
all, **don't register it there.** 
Then we don't need to make these adaptions in this class here."

## 3. Avoid copies. Period.

Copying is rarely flagged as "nit" - it's flagged as a defect:

- "can we avoid copying this map?"
- "creates another copy :/"
- "This creates a full copy of the map (and we do this for every config
  option that we iterate over). That can't be the solution."
- "rather than steal here, can we just copy / leave the original intact?"
  (when the original was fine).

Pass by const-reference; prefer `optional_ptr`/`string_view` at interfaces;
check that iteration uses `auto &` not `auto`; audit any `GetX()` that
returns a container by value.

## 4. Be rigorous about concurrency. Don't hide it with `atomic`.

- **Public fields without synchronization** are a code smell. If you see
  one, either document the invariant ("only accessed on thread X") or
  protect it. "Public field, no synchronization?" will come up.
- **`std::atomic` is not a substitute for thinking.** "Why do all these
  fields need to be atomic? Can't we just have some const fields in here?
  Only `is_streaming` and `last_chunk_time_ms` are late-set." Fields that
  are set once at construction should be `const`, not atomic.
- **Pass `lock_guard` as proof-of-lock** on internal methods that assume
  the caller already holds the lock. Don't grab the lock twice; don't hope
  the caller remembered.
- **Single-threaded code is fine, but say so.** If a method is only ever
  called from one background cron task, leave a comment that says exactly
  that; don't lock defensively and don't atomics defensively.
- **Capture timestamps/metrics outside the lock** where possible: "nit:
  let's capture this outside of lock already, or even pass it into the
  method from external (makes unit testing easier)."

## 5. Tests are part of the change, not a nice-to-have.

- **Behavior change without a test gets blocked.** "Is there a test you
  could add?" "Without test, makes it impossible to tell." Don't claim
  something works if nothing exercises it.
- **Tests must be deterministic.** "I'm concerned adding this test if we
  don't understand what's going on here. In particular the test might be
  timing-dependent / brittle." If you need a sleep, you don't understand
  the system well enough yet.
- **`REQUIRE` vs `CHECK`** matters. "why CHECK instead of REQUIRE?" - if
  the test can't meaningfully continue past the assertion, use REQUIRE.
- **Test names must match what they test.** "this uses either shutdown
  normal or shutdown terminate, so test name not right."
- **Cover the scenarios your PR claims to handle.** "First temp catalog
  usage, then subsequent ones. Then single-statement and multi-stmt txs..."
- **Generalize beyond the immediate trigger.** If you added support for
  pivot, ask whether CALL / DESCRIBE / SHOW need the same treatment - and
  if so, test them too.
- **Don't rely on replanning / retries to make a test pass.** Add a test
  that fails without the retry, then one that confirms the retry path.

## 6. Scope discipline. One PR = one change.

Push back on every refactor that snuck in unrelated features:

- "these are just headers, not sure I see the need to fully refactor this
  universe too in this PR."
- "I'm inclined not to add a second method here, can we instead transform
  `get_default_config_options` to `get_default_config`, i.e. return the
  `duckdb::DBConfig` object here? That results in less changes and the
  diff becomes much easier to read (just straightforward transform)."
- "Why is this PR already adapting the execution model? I thought we wanted
  to get this first step in..."

Before you submit, diff against what the PR description says the change
is. If the diff does more, split.

## 7. Reuse DuckDB primitives; match existing conventions.

- Reuse, don't reimplement: "we can possibly even just reuse that method,
  i.e. `function.supports_pushdown_extract = TableScanFunction::GetFunction().supports_pushdown_extract`".
- Mirror DuckDB naming conventions: "In the duck-server universe we
  typically don't use `_` suffix for field names (as DuckDB does not do
  this either)."
- Mirror DuckDB structure: "Add a comment that this method here mirrors
  `TableSupportsPushdownExtract` from DuckDB's table_scan?"
- DuckDB has query callbacks, session state, `ClientContextState`,
  `ScopedTransaction`, `optional_ptr`, `ReadOptionalPtr`, `FV()` for
  structured logs, etc. Use them instead of inventing parallel machinery.
- Follow the precedent of the surrounding file - if other similar
  serializers do it one way, match them.

## 8. Fix root causes, not symptoms.

When something breaks, the temptation is to compensate at the next layer
up. A senior reviewer will call that out:

- "`DuckLakeMaintenance::ExecuteDuckLakeMaintenanceCommand` should not
  have to compensate for that (it did not have the failing query IIUC).
  ... I think we should rethink / rework the transaction management in the
  ducklake maintenance code."
- "rather than retroactively fix this here, can we move the StartActivity
  call into the corresponding methods and make the determination there
  once we have the connection object?"

If you find yourself writing "recovery" or "reset" logic on a boundary,
ask whether the thing on the other side of the boundary should have
cleaned up after itself.

## 9. Don't build bespoke state machines when a small loop suffices.

- "There is a lot ongoing in this method, I'm not sure if we need that.
  Couldn't we just iterate over the registered secrets, and check if they
  are ducklake ones that are close to expiration, and just refresh that?
  ... No need for all these lookups here and trying to reassess the
  current state of the universe."

Default to the simplest thing that could work. Add machinery only when you
can point to the concrete case it solves.

## 10. Catch `std::exception`, never `...`.

- "`catch (std::exception &)` - catch-all would hide potentially nasty
  bugs (we should never throw objects other than exception)."

Also: move `BeginTransaction()` *inside* the try - it can throw too, if
the connection has a lingering TX. Don't assume prior state.

## 11. Naming is load-bearing. Pick the precise word.

care about naming and push back repeatedly until it's right:

- `GetClientBGActivityShutdownExtension`, not `GetBGShutdownExtension`
- `GetInitialCooldownPeriod`, not `GetCooldownPeriod`
- `ShutdownNormal` to mirror `ShutdownTerminate` (not `GracefulShutdown`)
- `is_temp_catalog`, not `temp_catalog`
- `IsTempCatalog()` method on the ref type, so call sites read right.

Before submitting, scan every public identifier you introduced and ask:
*does the name tell a caller exactly when to use it and when not to?*

## 12. Comments explain WHY, and only when the WHY is non-obvious.

only require comments in exactly two situations:

1. **The code mirrors something elsewhere and you'd never know**: "Add a
   comment that this mirrors `DuckCatalog::Initialize`."
2. **The code has a non-obvious invariant or constraint**: "add a comment
   here saying that adding anything new here requires the action/rpc to be
   idempotent", "what does this mean? Can you add a comment?"

Otherwise, don't narrate what the code does. Never leave `// TODO comment
back in when X` style commented-out code - either delete or fix.

## 13. Assert invariants. Don't defend against impossible states.

He likes `D_ASSERT` for actual invariants - "Let's assert that
`D_ASSERT(state->current_catalog_version.server_uuid != 0);`"

He dislikes defensive checks for things that can't happen. If your guard's
only job is "peace of mind that clients won't segfault randomly", you
don't understand the code path; figure out when the pointer is actually
null and remove the guard.

## 14. Don't let existing-but-bad patterns spread.

If you notice that a class elsewhere does something wrong (copies a map
on a hot path, writes a log line at the wrong level, exposes a public
field without a mutex), don't copy it into your new code with a "for
consistency" justification. Either fix the old one or do the right thing
in the new one and file a follow-up.

"Note that DuckDB's `TableScanBindData` forgets to copy `order_options`
when using its `Copy()` method (just a bug waiting to happen)."

## 15. Consider adjacent cases that share the invariant.

Every time you touch "tables", ask about schemas, indices, views,
sequences. 
Every time you change "the read path", ask about the write path.

- "Also check if we need to do the same for the schema (not just tables),
  and possibly other schema entries (I think temp tables also support
  indices). Can you also add a test that covers indices on temp table?"
- "do we want ducklake databases to be able to opt-out of maintenance?"

## 16. If you don't understand *why* your fix works, stop.

This is the single hardest gate:

- "I'm not sure I fully understand the problem here yet. The
  `InternalException` also suggests wrong API usage, so I'm not sure
  where we're doing something wrong here. ... I don't have a problem with
  changing the logic here to manually look up the tables, but I don't see
  what problem it exactly solves, or why this new method is avoiding that
  problem."
- "For some reason, the following sequence needs to be run twice to work."
  -> "I'm concerned adding this test if we don't understand what's going
  on here."

If your PR description contains a phrase like "for some reason", "mysteriously",
"this seems to fix it", or "works on my machine" - investigate until it
doesn't. Understanding is the deliverable; the code change is a byproduct.

## 17. Prefer obvious diffs over clever ones.

Given two implementations that achieve the same thing, choose the one that
makes the diff smaller and more mechanical. "That results in less changes
and the diff becomes much easier to read (just straightforward transform)."

Reviewers (including him) scale better when diffs tell a linear story.

## 18. Errors and messages reach humans. Treat them that way.

- "Let's avoid newlines in error messages."
- "The PR description says 'launched', the code says 'running for'. Also
  the code is currently not compiling." (Yes, he reads PR descriptions
  against the code.)
- Typos in log messages (`PRCs / retried`, `allowedal`, `etried`) will be
  flagged.

Proofread your strings. They are production UI.

---

## The five-minute self-review before you ask for review

Run this pass on your own diff, out loud, before requesting review:

1. **Why does this PR exist?** State it in one sentence. Every file in
   the diff should serve that sentence. Files that don't: split.
2. **Point at each new parameter, field, method, atomic, try/catch.** Can
   you justify it without "just in case"? If not, delete.
3. **Point at every copy** (`auto x = container`, pass-by-value of a
   `map`/`vector`/`string`). Must it copy? If not, fix.
4. **Point at every `if (special_case)`.** Is the branching at the right
   layer, or are you special-casing every caller?
5. **Tests**: does each observable behavior change have a test? Is any
   test timing-dependent, sleepy, or reliant on retries to pass?
6. **Concurrency**: are new public fields synchronized or justified? Are
   `atomic`s really needed, or should the field be `const`?
7. **Names**: does every new public name say precisely when to use it?
8. **Logs**: right level, structured (`FV`), not spammy at steady state?
9. **Do you understand why your change works**, including the weird
    edge case you almost hand-waved away?

If you can't answer any of these crisply, he will notice. Fix before
you post. When you do post, open with "Thanks for reviewing" - he does,
and it reminds everyone that code review is collaboration, not combat.
