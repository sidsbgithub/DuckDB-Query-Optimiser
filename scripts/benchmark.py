#!/usr/bin/env python3
"""Time the benchmark/ queries in your DuckDB build and print the result.

    python3 ./scripts/benchmark.py                       # all of benchmark/, twice
    python3 ./scripts/benchmark.py --runs 3
    python3 ./scripts/benchmark.py benchmark/q0001.sql benchmark/q0002.sql

This is the same measurement the leaderboard uses, printed instead of stored:
build/release/duckdb - which has your extension linked into it - runs the whole
set in one CLI process, so your cache keeps its session state across queries;
each query runs under EXPLAIN (ANALYZE, FORMAT JSON), so its result is never
materialised into the terminal; a throwaway warm-up pass runs first, so no timed
run pays for reading the database off disk; and the score is the geometric mean
of the query times of the *fastest whole run*.

It does not check your answers. A fast wrong answer scores nothing on the
leaderboard, so use `make test` for correctness.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

try:
    import psutil
except ImportError:
    psutil = None

BINARY = Path("build") / "release" / "duckdb"
DB = Path("data") / "imdb.duckdb"
QUERIES = Path("benchmark")

# The budget the leaderboard measures under; see the README.
RUNS = 2
THREADS = 6
MEMORY_MB = 8000

# A query faster than this is below what the timer can tell apart, and the log
# of a zero is not a number: the score floors it, as the grader's does.
FLOOR_S = 1e-4

# Wrapper nodes EXPLAIN ANALYZE puts above the plan.
WRAPPERS = {"EXPLAIN_ANALYZE", "RESULT_COLLECTOR"}
ERROR_START = re.compile(r"^\w[\w ]*Error: ", re.MULTILINE)


def die(msg: str, *hints: str) -> None:
    print(f"\nerror: {msg}", file=sys.stderr)
    for hint in hints:
        print(f"       {hint}", file=sys.stderr)
    raise SystemExit(1)


def section(title: str) -> None:
    print(f"\n{title}\n{'-' * len(title)}")


def collect(paths: list[str]) -> list[tuple[str, str]]:
    """[(name, sql)] - one .sql file is one query, in filename order."""
    files: list[Path] = []
    for arg in paths:
        path = Path(arg)
        files += sorted(path.glob("**/*.sql")) if path.is_dir() else [path]
    if not files:
        die(f"no .sql files in {', '.join(paths)}")
    out = []
    for f in files:
        if not f.exists():
            die(f"{f} does not exist")
        out.append((f.name, re.sub(r"[;\s]+$", "", f.read_text())))
    return out


def build_script(queries: list[tuple[str, str]], profiles: Path, tmp_dir: Path,
                 args) -> str:
    lines = [".bail off",
             f"SET threads = {args.threads};",
             f"SET memory_limit = '{args.max_memory_mb}MB';",
             "SET preserve_insertion_order = false;",
             f"SET temp_directory = '{tmp_dir}';"]
    if args.setup_sql:
        lines.append(args.setup_sql.rstrip().rstrip(";") + ";")
    for seq, (name, sql) in enumerate(queries):
        lines += [f".once {profiles / ('s%04d.json' % seq)}",
                  f"EXPLAIN (ANALYZE, FORMAT JSON) {sql};"]
    if args.stats:
        lines += [".mode csv", ".headers on", f".once {profiles.parent / 'stats.csv'}",
                  "SELECT * FROM waddle_recycling_stats();"]
    return "\n".join(lines) + "\n"


def fresh_db(db: Path, into: Path) -> Path:
    """A copy-on-write clone of the database, so a run cannot dirty yours.

    `cp -c` is the APFS clonefile and `--reflink=auto` its Linux equivalent, so
    2.6 GB costs milliseconds. A filesystem with neither would pay for the full
    copy on every benchmark, which is not worth it: fall back to the database
    itself and say so.
    """
    clone = into / db.name
    for cmd in (["cp", "-c", str(db), str(clone)],
                ["cp", "--reflink=always", str(db), str(clone)]):
        if subprocess.run(cmd, capture_output=True).returncode == 0:
            return clone
    print(f"  note: {db} could not be cloned cheaply, so the runs open it "
          f"directly and whatever they write to it stays")
    return db


def run_cli(binary: str, db: Path, script: Path, run_dir: Path, profiles: Path,
            total: int) -> tuple[float, int | None]:
    """Pipe the whole script into one CLI process; return wall time and sampled peak RSS.

    One process, so session state - and therefore any cache the extension keeps
    - lives across the queries. Its own cwd, which is also its TMPDIR, so a
    cache file written to a relative or system-temp path dies with the run.
    """
    cmd = [binary, "-init", "/dev/null", "-batch", str(db)]
    cwd = run_dir / "cwd"
    cwd.mkdir(parents=True, exist_ok=True)
    env = {**os.environ, "TMPDIR": str(cwd)}
    t0 = said = time.perf_counter()
    peak_rss = None
    with script.open() as stdin, (run_dir / "stdout.log").open("w") as out, \
            (run_dir / "stderr.log").open("w") as err:
        p = subprocess.Popen(cmd, stdin=stdin, stdout=out, stderr=err,
                             cwd=str(cwd), env=env)
        try:
            process = psutil.Process(p.pid) if psutil else None
        except psutil.Error:
            process = None
        while True:
            if process:
                try:
                    rss = process.memory_info().rss
                    peak_rss = max(peak_rss or 0, rss)
                except psutil.Error:
                    pass
            try:
                p.wait(timeout=0.1 if process else 2)
                break
            except subprocess.TimeoutExpired:
                if time.perf_counter() - said >= 15:
                    said = time.perf_counter()
                    done = sum(1 for _ in profiles.iterdir())
                    print(f"    {done}/{total} queries, {said - t0:.0f}s")
    return time.perf_counter() - t0, peak_rss


def latency(path: Path) -> float:
    """DuckDB's own measure of one query's execution, from its profile.

    Raises ValueError if the profile is not one: an empty file is a query that
    failed, and a statement DuckDB answers without a plan has no timing.
    """
    text = path.read_text()
    if not text.strip():
        raise ValueError("the query failed")
    try:
        profile = json.JSONDecoder().raw_decode(text[text.index("{"):])[0]
    except (ValueError, json.JSONDecodeError) as e:
        raise ValueError(f"unparseable profile: {e}") from e
    # v1.5.x keeps the totals on the root; 1.6.0-dev moved them into `query`
    if isinstance(profile.get("operator"), list) and profile["operator"]:
        seconds = (profile.get("query") or {}).get("total_time")
    elif profile.get("children"):
        seconds = profile.get("latency")
    else:
        raise ValueError("EXPLAIN ANALYZE returned no plan for this statement")
    if seconds is None:
        raise ValueError("the profile carries no timing")
    return float(seconds)


def measure(queries: list[tuple[str, str]], profiles: Path,
            stderr: str) -> tuple[dict[str, float], list[tuple[str, str]]]:
    """({query: seconds}, [(query, why it is missing)]) for one run."""
    times, failed, empty = {}, [], []
    for seq, (name, _) in enumerate(queries):
        path = profiles / ("s%04d.json" % seq)
        if not path.exists():
            failed.append((name, "never ran: the process stopped before it"))
            continue
        try:
            times[name] = latency(path)
        except ValueError as e:
            failed.append((name, str(e)))
            if path.stat().st_size == 0:
                empty.append(len(failed) - 1)
    # DuckDB reports a failure on stderr and nowhere else, in query order, so
    # the messages line up with the queries whose profile came out empty
    starts = [m.start() for m in ERROR_START.finditer(stderr)]
    messages = [stderr[a:b].strip().splitlines()[0][:200]
                for a, b in zip(starts, starts[1:] + [len(stderr)])]
    for i, message in zip(empty, messages):
        failed[i] = (failed[i][0], message)
    return times, failed


def one_run(run_dir: Path, queries: list[tuple[str, str]], binary: str, db: Path,
            args) -> tuple[dict[str, float], list[tuple[str, str]], dict | None, int | None]:
    """One pass over the whole set in a process of its own.

    Nothing carries over from the run before it: a new process is a new buffer
    pool and no session state, so whatever the extension cached is gone, and the
    cwd and temp directory it is given are its own and empty. The database is
    the exception - one copy is shared by every run, as the grader shares one.
    """
    profiles = run_dir / "profiles"
    tmp_dir = run_dir / "tmp"
    for d in (profiles, tmp_dir):
        d.mkdir(parents=True, exist_ok=True)
    script = run_dir / "script.sql"
    script.write_text(build_script(queries, profiles.resolve(),
                                   tmp_dir.resolve(), args))
    _, peak_rss = run_cli(binary, db.resolve(), script, run_dir, profiles, len(queries))
    out = measure(queries, profiles, (run_dir / "stderr.log").read_text())
    stats = None
    if args.stats and (run_dir / "stats.csv").exists():
        with (run_dir / "stats.csv").open(newline="") as stream:
            rows = list(csv.DictReader(stream))
        if len(rows) == 1:
            stats = {key: int(value) for key, value in rows[0].items()}
    if args.stats and stats is None:
        die(f"could not read recycling stats from {run_dir / 'stats.csv'}",
            f"see {run_dir / 'stderr.log'}")
    shutil.rmtree(str(tmp_dir), ignore_errors=True)
    return *out, stats, peak_rss


def geomean(seconds: list[float]) -> float:
    return math.exp(sum(math.log(max(s, FLOOR_S)) for s in seconds) / len(seconds))


def report(runs: list[dict[str, float]], failed: list[tuple[str, str]],
           slowest: int) -> None:
    # Scored over the queries that ran in every run, so the runs are comparable
    # and a query that fell over once cannot buy a better score.
    scored = sorted(set.intersection(*(set(r) for r in runs)))
    if not scored:
        die("no query produced a timing in every run",
            "re-run with --keep to read the CLI's stdout.log and stderr.log")

    # The geomean of the fastest run is the number the leaderboard ranks.
    section(f"Runs ({len(scored)} queries)")
    scores = [geomean([run[q] for q in scored]) for run in runs]
    best = min(range(len(runs)), key=lambda i: scores[i])
    for i, (run, score) in enumerate(zip(runs, scores)):
        print(f"  run {i}   {sum(run[q] for q in scored):7.2f} s total   "
              f"geomean {1000 * score:7.2f} ms"
              + ("   <- fastest" if i == best else ""))

    if slowest:
        section(f"Slowest {slowest} queries (fastest run)")
        order = sorted(scored, key=lambda q: runs[best][q], reverse=True)
        share = sum(runs[best][q] for q in scored)
        for q in order[:slowest]:
            seconds = runs[best][q]
            print(f"  {q:14} {seconds:8.3f} s   {100 * seconds / share:5.1f} % "
                  f"of the total")

    if failed:
        section(f"{len(failed)} queries produced no timing")
        for name, why in failed[:10]:
            print(f"  {name:14} {why}")
        if len(failed) > 10:
            print(f"  ... and {len(failed) - 10} more")
        print("\n  they are left out of the score. A query that fails here is "
              "wrong on the leaderboard")


def main() -> None:
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        epilog="\n".join(__doc__.splitlines()[1:]),
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("queries", nargs="*", default=[str(QUERIES)],
                    help=f".sql files, or directories of them (default: {QUERIES})")
    ap.add_argument("--db", type=Path, default=DB, help=f"database (default: {DB})")
    ap.add_argument("--runs", type=int, default=RUNS,
                    help=f"times to run the whole set (default: {RUNS})")
    ap.add_argument("--threads", type=int, default=THREADS,
                    help=f"SET threads (default: {THREADS})")
    ap.add_argument("--max-memory-mb", type=int, default=MEMORY_MB,
                    help=f"SET memory_limit, in MB (default: {MEMORY_MB})")
    ap.add_argument("--slowest", type=int, default=10, metavar="N",
                    help="how many of the slowest queries to print (default: 10)")
    ap.add_argument("--keep", action="store_true",
                    help="keep the profiles and logs instead of deleting them")
    ap.add_argument("--binary", type=Path, default=BINARY,
                    help=f"DuckDB CLI to run (default: {BINARY})")
    ap.add_argument("--setup-sql", default="",
                    help="SQL setting(s) applied before each query stream")
    ap.add_argument("--stats", action="store_true",
                    help="read waddle_recycling_stats() after each run")
    ap.add_argument("--json-out", type=Path,
                    help="write all per-query timings and run stats as JSON")
    args = ap.parse_args()

    # Resolved, because Popen resolves a relative executable against the child's
    # cwd and every run gets a directory of its own.
    binary = str(args.binary.resolve())
    version = subprocess.run([binary, "--version"], capture_output=True,
                             text=True).stdout.strip() if args.binary.exists() else ""
    if not version:
        die(f"{args.binary} is not there, or is not a duckdb CLI", "run `make` first")
    if not args.db.exists():
        die(f"{args.db} does not exist", "python3 ./scripts/download-imdb.py")
    if args.runs < 1:
        die("--runs has to be at least 1")

    queries = collect(args.queries)
    out = Path(tempfile.mkdtemp(prefix="benchmark-"))
    section("Benchmark")
    print(f"  binary    {binary} ({version})")
    print(f"  database  {args.db}")
    print(f"  queries   {len(queries)} x {args.runs} run(s)")
    print(f"  budget    {args.threads} threads, {args.max_memory_mb} MB memory")
    print()

    try:
        # One clone for the whole benchmark, as the grader does. The runs are
        # therefore not isolated from each other through the database: what one
        # writes into it, the next inherits.
        db = fresh_db(args.db, out)

        # The grader's runs are hot: its correctness pass reads the whole
        # database before the clock starts, so no timed run pays for pulling
        # 2.6 GB off disk. This is that pass - its own process, so nothing but
        # the OS page cache survives it, and its timings are thrown away.
        print("  warm-up ...")
        one_run(out / "warmup", queries, binary, db, args)

        runs, failed, run_details = [], [], []
        for i in range(args.runs):
            print(f"  run {i} ...")
            times, bad, stats, peak_rss = one_run(out / f"run{i}", queries, binary, db, args)
            runs.append(times)
            run_details.append({"query_seconds": times, "failed": bad,
                                "stats": stats, "peak_rss_bytes": peak_rss})
            # Reported once, from the first run that saw them: the same query
            # fails the same way in every run.
            failed = failed or bad
        report(runs, failed, args.slowest)
        if args.json_out:
            result = {"binary": binary, "version": version, "database": str(args.db),
                      "queries": [name for name, _ in queries], "threads": args.threads,
                      "memory_limit_mb": args.max_memory_mb, "setup_sql": args.setup_sql,
                      "runs": run_details}
            args.json_out.parent.mkdir(parents=True, exist_ok=True)
            args.json_out.write_text(json.dumps(result, indent=2) + "\n")
            print(f"\n  results written to {args.json_out}")
    finally:
        if args.keep:
            print(f"\n  profiles and logs kept in {out}")
        else:
            shutil.rmtree(str(out), ignore_errors=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        raise SystemExit("\naborted")
