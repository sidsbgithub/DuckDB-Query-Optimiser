#!/usr/bin/env python3
"""Where recycling hits and misses on a query stream, printed as Markdown.

    python3 ./scripts/workload_analysis.py                 # all of benchmark/
    python3 ./scripts/workload_analysis.py --out analysis  # also keep the raw runs

Runs the stream under the grader's settings, each run in its own process over
one copy-on-write clone of data/imdb.duckdb, every statement under EXPLAIN
(ANALYZE, FORMAT JSON): a discarded warm-up run, then

1. vanilla (`waddle_recycling = false`), for per-query latencies and the full
   physical plans;
2. recycling with `waddle_recycling_diagnostics` on, logging to files, and
   `waddle_recycling_stats()` read after each statement.

It then reports (a) each query's hits, misses and skips, (b) the dominant miss
and skip reasons, and (c) how much of the vanilla time sits in physical
subtrees that an earlier query already computed, per pipeline-breaker kind,
for exact matches and for matches that differ only in constants.

The subtree analysis is an estimate from vanilla plans, not from recycling
keys: it compares plan shapes (physical operators and their printed details),
and credits a repeated subtree with its share of the query's operator time.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
import subprocess
import sys
import tempfile
from collections import Counter, defaultdict
from pathlib import Path
from typing import NamedTuple

BINARY = Path("build") / "release" / "duckdb"
DB = Path("data") / "imdb.duckdb"
QUERIES = Path("benchmark")
SETTINGS = ["SET threads = 6;", "SET memory_limit = '8000MB';", "SET preserve_insertion_order = false;"]
FLOOR_S = 1e-4

# Physical operators that root a pipeline-breaker subtree, by kind.
AGGREGATES = {"HASH_GROUP_BY", "PERFECT_HASH_GROUP_BY", "UNGROUPED_AGGREGATE", "SIMPLE_AGGREGATE"}
# Details that depend on the run or on the rest of the query rather than on what the subtree computes.
IGNORED_DETAILS = {"Estimated Cardinality", "CTE Name", "Table Index", "Dynamic Filters"}
CORRELATED = "aggregate over a correlated subquery"
BUILD_SIDE = "hash-join build side"
CTE_DEFINITION = "shared CTE definition"
# A physical HASH_GROUP_BY without aggregates is a DISTINCT or an aggregate whose outputs were all pruned.
KINDS = ("aggregate", CORRELATED, "DISTINCT or group-only aggregate", BUILD_SIDE, CTE_DEFINITION)
CONSTANT = re.compile(r"'(?:[^']|'')*'|\b\d+(?:\.\d+)?\b")
# A constant with the comparison in front of it, as scan filters print them (`production_year>=2008`).
BOUND = re.compile(r"(>=|<=|!=|=|>|<)?\s*('(?:[^']|'')*'|\b\d+(?:\.\d+)?\b)")


# ---------------------------------------------------------------------------
# Running the stream


def collect(directory: Path) -> list[tuple[str, str]]:
    files = sorted(directory.glob("*.sql"))
    return [(f.stem, re.sub(r"[;\s]+$", "", f.read_text())) for f in files]


def run_stream(queries, db: Path, out: Path, recycling: bool) -> None:
    """One process over the whole stream; profiles (and stats) land in `out`."""
    out.mkdir(parents=True, exist_ok=True)
    lines = [".mode json", *SETTINGS, f"SET waddle_recycling = {str(recycling).lower()};"]
    if recycling:
        logs = out / "logs"
        logs.mkdir(exist_ok=True)
        lines += ["SET waddle_recycling_diagnostics = true;",
                  f"CALL enable_logging(['WaddleRecycling', 'QueryLog'], storage = 'file', "
                  f"storage_path = '{logs}');"]
    for name, sql in queries:
        lines += [f".once {out / (name + '.profile')}", f"EXPLAIN (ANALYZE, FORMAT JSON) {sql};"]
        if recycling:
            lines += [f".once {out / (name + '.stats')}", "SELECT * FROM waddle_recycling_stats();"]
    script = out / "script.sql"
    script.write_text("\n".join(lines) + "\n")
    with script.open() as stdin, (out / "stderr.log").open("w") as err:
        subprocess.run([str(BINARY), "-init", "/dev/null", "-batch", str(db)], stdin=stdin,
                       stdout=subprocess.DEVNULL, stderr=err, check=True)


def read_profile(path: Path) -> dict:
    text = path.read_text()
    return json.JSONDecoder().raw_decode(text[text.index("{"):])[0]


def read_stats(path: Path) -> dict[str, int]:
    return {k: int(v) for k, v in json.loads(path.read_text())[0].items()}


class Event(NamedTuple):
    """One diagnostics line: `hit|miss breaker=<kind> key=<key>` or `skip reason=<r> breaker=<kind> detail=<d>`."""
    outcome: str
    breaker: str
    reason: str = ""
    detail: str = ""
    key: str = ""


def parse_event(message: str) -> Event:
    outcome, rest = message.split(" ", 1)
    if outcome == "skip":
        fields = re.match(r"reason=(\w+) breaker=(\w+) detail=(.*)$", rest, re.S)
        return Event(outcome, fields.group(2), reason=fields.group(1), detail=fields.group(3))
    fields = re.match(r"breaker=(\w+) key=(.*)$", rest, re.S)
    return Event(outcome, fields.group(1), key=fields.group(2))


def read_events(logs: Path, queries) -> dict[str, list[Event]]:
    """Diagnostics events per query: QueryLog ties each log context to its statement."""
    context_statement: dict[str, str] = {}
    events: dict[str, list[Event]] = defaultdict(list)
    with (logs / "duckdb_log_entries.csv").open() as f:
        rows = list(csv.DictReader(f))
    explains = iter(name for name, _ in queries)
    for row in rows:
        if row["type"] == "QueryLog":
            if row["message"].startswith("EXPLAIN (ANALYZE"):
                context_statement[row["context_id"]] = next(explains)
        elif row["context_id"] in context_statement:
            events[context_statement[row["context_id"]]].append(parse_event(row["message"]))
    return events


# ---------------------------------------------------------------------------
# Recycling keys: the length-prefixed text form written by src/recycling_key.cpp


def key_parts(key: str) -> list[str]:
    parts, i = [], 0
    while i < len(key):
        colon = key.index(":", i)
        size = int(key[i:colon])
        parts.append(key[colon + 1:colon + 1 + size])
        i = colon + 1 + size
    return parts


def without_constants(key: str) -> str:
    """The key with every typed constant and scan-filter literal replaced by `?`."""
    parts = key_parts(key)
    count = int(parts[0].split(":")[1])
    children = [without_constants(child) for child in parts[1:1 + count]]
    rest = parts[1 + count:]
    out, filter_part = [], None
    for i, part in enumerate(rest):
        if part == "scan_filters":
            filter_part = 0
        elif part == "filter_projection":
            filter_part = None
        elif filter_part is not None:
            filter_part += 1
            if filter_part % 2 == 0:  # (column, filter text) pairs
                part = CONSTANT.sub("?", part)
        elif i >= 2 and rest[i - 2] == "75":  # ExpressionType::VALUE_CONSTANT, its type, then its value
            part = "?"
        out.append(part)
    return "|".join([parts[0], *children, *out])


def without_root_outputs(key: str) -> str:
    """The key without the root's aggregate expressions and output types: its input and groups."""
    parts = key_parts(key)
    count = int(parts[0].split(":")[1])
    rest = parts[1 + count:]
    cut = rest.index("expression") if "expression" in rest else rest.index("output_types")
    return "|".join(parts[:1 + count] + rest[:cut])


# ---------------------------------------------------------------------------
# Physical plans


def plan_shape(node: dict, ctes: dict[str, str], constants: bool = True) -> str:
    """Operator names and details of the subtree; CTE scans spell out the definition they read."""
    info = node.get("extra_info", {})
    if node["operator_name"] == "CTE_SCAN":
        return "CTE_SCAN(" + ctes.get(info.get("CTE Index", ""), "?") + ")"
    details = []
    for k in sorted(info):
        if k in IGNORED_DETAILS or "Dynamic Filter" in k:
            continue
        value = json.dumps(info[k])
        details.append(f"{k}={value if constants else CONSTANT.sub('?', value)}")
    children = ",".join(plan_shape(c, ctes, constants) for c in node.get("children", []))
    return f"{node['operator_name']}[{';'.join(details)}]({children})"


def operator_time(node: dict) -> float:
    return node.get("operator_timing", 0.0) + sum(operator_time(c) for c in node.get("children", []))


def contains(node: dict, operator: str) -> bool:
    return operator in node["operator_name"] or any(contains(c, operator) for c in node.get("children", []))


def reads_outer_delim_scan(node: dict) -> bool:
    """A DELIM_SCAN reads the duplicate-eliminated rows of a DELIM_JOIN; outside its join it depends on context."""
    if node["operator_name"].endswith("DELIM_JOIN"):
        return False
    return node["operator_name"] == "DELIM_SCAN" or any(reads_outer_delim_scan(c) for c in node.get("children", []))


def is_group_only(node: dict) -> bool:
    return node["operator_name"] == "HASH_GROUP_BY" and not node.get("extra_info", {}).get("Aggregates")


def breakers(root: dict):
    """(kind, subtree root, shape, constant-free shape) for every context-free pipeline-breaker subtree,
    outermost first.

    Skipped: the duplicate-eliminating aggregate a DELIM_JOIN keeps as its own child, and subtrees that read a
    DELIM_SCAN of a join outside them (both belong to a correlated subquery, not to a subtree of their own).
    """
    ctes: dict[str, str] = {}

    def visit(node):
        name = node["operator_name"]
        children = node.get("children", [])
        if name == "CTE":
            definition = children[0]
            yield CTE_DEFINITION, definition
            ctes[node["extra_info"].get("Table Index", "")] = plan_shape(definition, ctes)
        elif name in AGGREGATES:
            if is_group_only(node):
                yield KINDS[2], node
            elif contains(node, "DELIM_JOIN"):
                yield CORRELATED, node
            else:
                yield "aggregate", node
        elif name == "HASH_JOIN":
            yield BUILD_SIDE, children[1]
        for child in children:
            if not (name.endswith("DELIM_JOIN") and is_group_only(child)):
                yield from visit(child)

    # Shapes must be taken while `ctes` holds the definitions in scope, so take them as the walk goes.
    for kind, node in visit(root):
        if not reads_outer_delim_scan(node):
            yield kind, node, plan_shape(node, ctes), plan_shape(node, ctes, constants=False)


def subtree_ids(node: dict) -> set[int]:
    ids = {id(node)}
    for child in node.get("children", []):
        ids |= subtree_ids(child)
    return ids


def is_narrower(earlier_shape: str, shape: str) -> bool:
    """True if every changed constant is a range bound that moved inward, so the new subtree reads a subset."""
    changed = [(op, a, b) for (op, a), (_, b) in zip(BOUND.findall(earlier_shape), BOUND.findall(shape)) if a != b]
    for op, a, b in changed:
        if a.startswith("'") or op not in (">=", ">", "<=", "<"):
            return False
        if (op in (">=", ">") and float(b) < float(a)) or (op in ("<=", "<") and float(b) > float(a)):
            return False
    return bool(changed)


class Repeat(NamedTuple):
    """A pipeline-breaker subtree of one query and whether an earlier query had it."""
    kind: str
    node: dict
    saving: float  # the subtree's share of the query's operator time, times the query's vanilla latency
    exact: bool  # an earlier query had the same plan shape
    modulo_constants: bool  # an earlier query had the same plan shape once constants are ignored
    earlier: str = ""  # for a repeat only modulo constants: the earlier query it is compared with
    changed: str = ""  # ... and the constants that changed
    narrower: bool = False  # ... and whether some earlier query read a superset


def find_repeats(names, plans, latency) -> dict[str, list[Repeat]]:
    seen: set[str] = set()
    earlier_shapes: dict[str, list[tuple[str, str]]] = defaultdict(list)  # constant-free shape -> (query, shape)
    repeats: dict[str, list[Repeat]] = {}
    for n in names:
        plan = plans[n]["children"][0]
        total = operator_time(plan) or 1.0
        found = list(breakers(plan))
        repeats[n] = []
        for kind, node, shape, constant_free in found:
            saving = operator_time(node) / total * latency[n]
            exact, candidates = shape in seen, earlier_shapes.get(constant_free, [])
            if exact or not candidates:
                repeats[n].append(Repeat(kind, node, saving, exact, bool(candidates)))
                continue
            narrower = [c for c in candidates if is_narrower(c[1], shape)]
            earlier, earlier_shape = (narrower or candidates)[0]
            changed = [f"{a} → {b}" for a, b in zip(CONSTANT.findall(earlier_shape), CONSTANT.findall(shape)) if a != b]
            repeats[n].append(Repeat(kind, node, saving, False, True, earlier, ", ".join(dict.fromkeys(changed)),
                                     bool(narrower)))
        for kind, node, shape, constant_free in found:
            seen.add(shape)
            earlier_shapes[constant_free].append((n, shape))
    return repeats


def savings(repeats: dict[str, list[Repeat]], kinds, served) -> dict[str, float]:
    """Per query, the time in repeated subtrees of `kinds` that `served` accepts; a subtree inside another counted
    one is not counted again."""
    per_query = {}
    for n, subtrees in repeats.items():
        covered: set[int] = set()
        total = 0.0
        for repeat in subtrees:  # outermost first
            if repeat.kind in kinds and served(repeat) and id(repeat.node) not in covered:
                covered |= subtree_ids(repeat.node)
                total += repeat.saving
        if total:
            per_query[n] = total
    return per_query


# ---------------------------------------------------------------------------
# Report


def ms(seconds: float) -> str:
    return f"{seconds * 1000:.1f}"


def geomean(values) -> float:
    values = [max(v, FLOOR_S) for v in values]
    return math.exp(sum(math.log(v) for v in values) / len(values))


def event_summary(events: list[Event]) -> str:
    return "; ".join(f"skip {e.breaker} ({e.reason})" if e.outcome == "skip" else f"{e.outcome} {e.breaker}"
                     for e in events) or "-"


def skip_cause(event: Event) -> str:
    """The skip reason plus the stable head of its detail (names, constants and expressions dropped)."""
    detail = event.detail
    for prefix in ("function bind data", "subquery", "dynamic filter from outside the subtree", "join form",
                   "mutable scan filter", "operator", "correlated column", "column bound outside the subtree",
                   "order-sensitive DISTINCT ON", "grouping sets", "nested column scan", "scan option"):
        if detail.startswith(prefix):
            if prefix in ("join form", "operator"):
                return f"{event.reason}: {detail}"
            if prefix == "function bind data":
                return f"{event.reason}: {prefix} ({detail.split('(')[0].split()[-1]})"
            return f"{event.reason}: {prefix}"
    return f"{event.reason}: {detail}"


class Runs(NamedTuple):
    """What the vanilla and recycling runs recorded, per query name."""
    names: list[str]
    plans: dict[str, dict]  # vanilla profiles: physical plan and latency
    vanilla_s: dict[str, float]
    recycling_s: dict[str, float]
    events: dict[str, list[Event]]
    outcomes: dict[str, set[str]]
    hit_queries: set[str]
    final_stats: dict[str, int]  # waddle_recycling_stats() after the last query

    def geomean_after(self, per_query: dict[str, float]) -> float:
        """The vanilla geomean if each query saved `per_query` seconds."""
        return geomean(max(self.vanilla_s[n] - per_query.get(n, 0.0), 0.0) for n in self.names)


def read_runs(queries, vanilla: Path, recycled: Path) -> Runs:
    names = [n for n, _ in queries]
    plans = {n: read_profile(vanilla / f"{n}.profile") for n in names}
    events = read_events(recycled / "logs", queries)
    outcomes = {n: {e.outcome for e in events[n]} for n in names}
    return Runs(names=names, plans=plans,
                vanilla_s={n: plans[n]["latency"] for n in names},
                recycling_s={n: read_profile(recycled / f"{n}.profile")["latency"] for n in names},
                events=events, outcomes=outcomes,
                hit_queries={n for n in names if "hit" in outcomes[n]},
                final_stats=read_stats(recycled / f"{names[-1]}.stats"))


def report_totals(runs: Runs) -> None:
    print("## Totals\n")
    print(f"- Queries: {len(runs.names)}")
    print(f"- Geometric mean: vanilla {ms(geomean(runs.vanilla_s.values()))} ms, "
          f"recycling {ms(geomean(runs.recycling_s.values()))} ms")
    print(f"- Total: vanilla {ms(sum(runs.vanilla_s.values()))} ms, recycling {ms(sum(runs.recycling_s.values()))} ms")
    print("- Final stats: " + ", ".join(f"{k}={v}" for k, v in runs.final_stats.items() if v))


def report_per_query(runs: Runs) -> None:
    print("\n## Per query\n")
    print("| Query | Vanilla ms | Recycling ms | Events |")
    print("|---|---:|---:|---|")
    for n in runs.names:
        print(f"| {n} | {ms(runs.vanilla_s[n])} | {ms(runs.recycling_s[n])} | {event_summary(runs.events[n])} |")


def report_breakers_by_kind(runs: Runs) -> None:
    print("\n## Examined breakers by kind\n")
    by_kind: dict[str, Counter] = defaultdict(Counter)
    for n in runs.names:
        for e in runs.events[n]:
            by_kind[e.breaker][e.outcome] += 1
    print("| Breaker | Hits | Misses | Skips |")
    print("|---|---:|---:|---:|")
    for kind, count in sorted(by_kind.items()):
        print(f"| {kind} | {count['hit']} | {count['miss']} | {count['skip']} |")


def report_effect_by_event(runs: Runs) -> None:
    print("\n## Effect by event\n")
    print("Single samples; queries without events show the run-to-run noise.\n")
    groups: dict[str, list[str]] = defaultdict(list)
    for n in runs.names:
        kinds = runs.outcomes[n]
        groups["hit" if "hit" in kinds else "miss" if "miss" in kinds else "skip only" if kinds else "no event"].append(n)
    print("| Queries with | Count | Vanilla geomean ms | Recycling geomean ms | Ratio | Vanilla total ms "
          "| Recycling total ms |")
    print("|---|---:|---:|---:|---:|---:|---:|")
    for group in ("hit", "miss", "skip only", "no event"):
        members = groups[group]
        before = geomean(runs.vanilla_s[n] for n in members)
        after = geomean(runs.recycling_s[n] for n in members)
        print(f"| {group} | {len(members)} | {ms(before)} | {ms(after)} | {after / before:.2f} "
              f"| {ms(sum(runs.vanilla_s[n] for n in members))} | {ms(sum(runs.recycling_s[n] for n in members))} |")


def report_miss_reasons(runs: Runs) -> None:
    print("\n## Miss reasons\n")
    seen_constant_free, seen_inputs = set(), set()
    misses: dict[str, list[str]] = defaultdict(list)
    for n in runs.names:
        for e in runs.events[n]:
            if not e.key:
                continue
            constant_free, inputs = without_constants(e.key), without_root_outputs(e.key)
            if e.outcome == "miss":
                if constant_free in seen_constant_free:
                    misses["differing constants"].append(n)
                elif inputs in seen_inputs:
                    misses["same input and groups, different aggregate outputs"].append(n)
                else:
                    misses["first occurrence"].append(n)
            seen_constant_free.add(constant_free)
            seen_inputs.add(inputs)
    print("| Reason | Misses | Queries |")
    print("|---|---:|---|")
    for reason, members in sorted(misses.items(), key=lambda kv: -len(kv[1])):
        print(f"| {reason} | {len(members)} | {', '.join(members)} |")


def report_skip_causes(runs: Runs) -> None:
    print("\n## Skip causes\n")
    causes: dict[str, list[str]] = defaultdict(list)
    for n in runs.names:
        for e in runs.events[n]:
            if e.outcome == "skip":
                causes[skip_cause(e)].append(n)
    print("| Cause | Skips | Queries |")
    print("|---|---:|---|")
    for cause, members in sorted(causes.items(), key=lambda kv: -len(kv[1])):
        print(f"| {cause} | {len(members)} | {', '.join(sorted(set(members)))} |")
    print(f"\nQueries without any pipeline-breaker event (no aggregate or DISTINCT): "
          f"{sum(1 for n in runs.names if not runs.events[n])}")


def exact(repeat: Repeat) -> bool:
    return repeat.exact


def report_repeats_by_kind(runs: Runs, repeats: dict[str, list[Repeat]]) -> None:
    print("\n## Repeated physical subtrees by breaker kind (vanilla plans)\n")
    print("A subtree *repeats* when an earlier query had one with the same plan shape. Saving estimate: the "
          "subtree's share of its query's operator time, times the query's vanilla latency; a repeated subtree "
          "inside another counted one is not counted again.\n")
    print("| Breaker kind | Subtrees | Exact repeats | Exact repeats ≥ 1 ms | Repeats only modulo constants "
          "| Est. saving exact (ms) | Est. geomean exact (ms) | Of which in queries without a hit (ms) "
          "| Est. saving modulo constants (ms) | Est. geomean modulo constants (ms) |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for kind in KINDS:
        of_kind = [r for subtrees in repeats.values() for r in subtrees if r.kind == kind]
        exact_saving = savings(repeats, {kind}, exact)
        loose_saving = savings(repeats, {kind}, lambda r: r.modulo_constants)
        cells = [kind, len(of_kind), sum(r.exact for r in of_kind), sum(r.exact and r.saving >= 1e-3 for r in of_kind),
                 sum(r.modulo_constants and not r.exact for r in of_kind),
                 ms(sum(exact_saving.values())), ms(runs.geomean_after(exact_saving)),
                 ms(sum(v for n, v in exact_saving.items() if n not in runs.hit_queries)),
                 ms(sum(loose_saving.values())), ms(runs.geomean_after(loose_saving))]
        print("| " + " | ".join(str(c) for c in cells) + " |")
    print(f"\nVanilla geomean for comparison: {ms(geomean(runs.vanilla_s.values()))} ms.")


def report_kinds_combined(runs: Runs, repeats: dict[str, list[Repeat]]) -> None:
    print("\n### Kinds combined (exact repeats; overlapping subtrees counted once)\n")
    print("| Kinds | Est. saving (ms) | Of which in queries without a hit (ms) | Est. geomean (ms) |")
    print("|---|---:|---:|---:|")
    for label, kinds in (("correlated-subquery aggregates + build sides", {CORRELATED, BUILD_SIDE}),
                         ("correlated-subquery aggregates + build sides + CTE definitions",
                          {CORRELATED, BUILD_SIDE, CTE_DEFINITION}),
                         ("all kinds", set(KINDS))):
        per_query = savings(repeats, kinds, exact)
        print(f"| {label} | {ms(sum(per_query.values()))} "
              f"| {ms(sum(v for n, v in per_query.items() if n not in runs.hit_queries))} "
              f"| {ms(runs.geomean_after(per_query))} |")
    build_alone = savings(repeats, {BUILD_SIDE}, exact)
    with_correlated = savings(repeats, {CORRELATED, BUILD_SIDE}, exact)
    correlated_alone = savings(repeats, {CORRELATED}, exact)
    print(f"\nBuild sides add {ms(sum(with_correlated.values()) - sum(correlated_alone.values()))} ms on top of "
          f"correlated-subquery aggregates (alone: {ms(sum(build_alone.values()))} ms).")


def report_largest_savings(runs: Runs, repeats: dict[str, list[Repeat]]) -> None:
    print("\n### Largest estimated savings per kind (exact repeats, queries without a hit)\n")
    for kind in KINDS:
        per_query = savings(repeats, {kind}, exact)
        top = sorted(((n, v) for n, v in per_query.items() if n not in runs.hit_queries), key=lambda kv: -kv[1])[:8]
        print(f"- {kind}: " + (", ".join(f"{n} {ms(v)}/{ms(runs.vanilla_s[n])} ms" for n, v in top) or "none"))


def report_constant_only_repeats(runs: Runs, repeats: dict[str, list[Repeat]]) -> None:
    print("\n### Subtrees that repeat only modulo constants\n")
    print("*Narrower*: every changed constant is a range bound that moved inward against some earlier query, so the "
          "subtree reads a subset of what that query read; only these could be served by predicate containment.\n")
    constant_only = sorted(((r, n) for n, subtrees in repeats.items() for r in subtrees
                            if r.modulo_constants and not r.exact), key=lambda rn: -rn[0].saving)
    print("| Query | Kind | Est. subtree ms | Earlier query | Constants (earlier → this) | Narrower |")
    print("|---|---|---:|---|---|---|")
    for r, n in constant_only:
        print(f"| {n} | {r.kind} | {ms(r.saving)} | {r.earlier} | {r.changed[:120]} | {'yes' if r.narrower else 'no'} |")
    narrower = savings(repeats, set(KINDS), lambda r: r.narrower)
    print(f"\nNarrower: {sum(r.narrower for r, _ in constant_only)} of {len(constant_only)} subtrees, in "
          f"{len(narrower)} queries ({', '.join(sorted(narrower))}); est. saving {ms(sum(narrower.values()))} ms if "
          f"they were free; est. geomean {ms(runs.geomean_after(narrower))} ms.")


def report(queries, vanilla: Path, recycled: Path) -> None:
    runs = read_runs(queries, vanilla, recycled)
    report_totals(runs)
    report_per_query(runs)
    report_breakers_by_kind(runs)
    report_effect_by_event(runs)
    report_miss_reasons(runs)
    report_skip_causes(runs)
    repeats = find_repeats(runs.names, runs.plans, runs.vanilla_s)
    report_repeats_by_kind(runs, repeats)
    report_kinds_combined(runs, repeats)
    report_largest_savings(runs, repeats)
    report_constant_only_repeats(runs, repeats)


def clone_database(into: Path) -> Path:
    """A copy-on-write clone (APFS `cp -c`, Linux reflink), so the runs cannot dirty data/imdb.duckdb."""
    clone = into / DB.name
    for command in (["cp", "-c", str(DB), str(clone)], ["cp", "--reflink=always", str(DB), str(clone)]):
        if subprocess.run(command, capture_output=True).returncode == 0:
            return clone
    sys.exit(f"could not clone {DB} copy-on-write into {into}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("queries", nargs="?", default=str(QUERIES))
    parser.add_argument("--out", help="keep profiles, stats and logs here (default: a temporary directory)")
    parser.add_argument("--reuse", action="store_true", help="report from the runs already in --out")
    args = parser.parse_args()
    if args.reuse and not args.out:
        parser.error("--reuse needs --out")
    queries = collect(Path(args.queries))
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(args.out) if args.out else Path(tmp)
        if not args.reuse:
            clone = clone_database(Path(tmp))
            print(f"running {len(queries)} queries: warm-up, vanilla, recycling ...", file=sys.stderr)
            run_stream(queries, clone, out / "warmup", recycling=False)
            run_stream(queries, clone, out / "vanilla", recycling=False)
            run_stream(queries, clone, out / "recycling", recycling=True)
        report(queries, out / "vanilla", out / "recycling")


if __name__ == "__main__":
    main()
