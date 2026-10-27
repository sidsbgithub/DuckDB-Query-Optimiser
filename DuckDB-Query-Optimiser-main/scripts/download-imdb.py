#!/usr/bin/env python3
"""Download the IMDB (JOB) dataset and build data/imdb.duckdb.

    python3 ./scripts/download-imdb.py

The queries in benchmark/ run against this database. This fetches the CWI dump
(~1.3 GB), loads its CSVs through scripts/imdb_schema.sql with a DuckDB CLI at
the pinned version, checks the canonical row counts, and deletes everything it
downloaded except the database itself.

Needs about 10 GB free while it runs and leaves 2.5 GB behind. Re-running is a
no-op unless you pass --force.
"""

from __future__ import annotations

import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request
import zipfile
from pathlib import Path

URL = "https://event.cwi.nl/da/job/imdb.tgz"
DB = Path("data") / "imdb.duckdb"
SCHEMA = Path(__file__).resolve().with_name("imdb_schema.sql")
WORKFLOW = Path(".github/workflows/MainDistributionPipeline.yml")
FALLBACK_TAG = "v1.5.5"
RELEASE = "https://github.com/duckdb/duckdb/releases/download/{tag}/{asset}"

# The csv dump is quoted and backslash-escaped, and writes empty for NULL.
COPY_OPTS = r"""(FORMAT csv, HEADER false, QUOTE '"', ESCAPE '\', NULL '')"""

# canonical row counts of the JOB dump (spot checks)
EXPECTED = {"cast_info": 36244344, "title": 2528312, "movie_info": 14835720}

PEAK_GB = 10


def die(msg: str, *hints: str) -> None:
    print(f"\nerror: {msg}", file=sys.stderr)
    for hint in hints:
        print(f"       {hint}", file=sys.stderr)
    raise SystemExit(1)


def section(title: str) -> None:
    print(f"\n{title}\n{'-' * len(title)}")


def human(n: float) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024
    raise AssertionError("unreachable")


def download(url: str, dest: Path) -> None:
    """urlretrieve with a progress line. A terminal gets one line that rewrites
    itself; anything else - a log, a pipe - gets a line every 30 seconds."""
    tty = sys.stdout.isatty()
    every = 0.5 if tty else 30.0
    start = time.time()
    last = [0.0]

    def hook(blocks: int, block_size: int, total: int) -> None:
        now = time.time()
        got = blocks * block_size
        done = 0 < total <= got
        if now - last[0] < every and not done:
            return
        last[0] = now
        seen = min(got, total) if total > 0 else got
        of = f" / {human(total)}  {seen * 100 // total:3d}%" if total > 0 else ""
        rate = human(seen / max(now - start, 1e-3))
        sys.stdout.write(f"\r  {human(seen)}{of}  at {rate}/s   " if tty
                         else f"  {human(seen)}{of}  at {rate}/s\n")
        sys.stdout.flush()

    try:
        urllib.request.urlretrieve(url, str(dest), hook)
    except OSError as e:
        if tty:
            sys.stdout.write("\n")
        die(f"downloading {url} failed: {e}",
            "check your connection and run this script again")
    if tty:
        sys.stdout.write("\n")


# --- the DuckDB CLI that writes the database ---------------------------------
#
# The storage format belongs to a DuckDB version, so a database written by some
# other CLI on the PATH may not open in the build the assignment is pinned to.

def pinned_tag(root: Path) -> str:
    try:
        text = (root / WORKFLOW).read_text()
    except OSError:
        return FALLBACK_TAG
    match = re.search(r"^\s*duckdb_version:\s*(\S+)", text, re.MULTILINE)
    return match.group(1) if match else FALLBACK_TAG


def cli_version(exe: Path) -> str | None:
    """The CLI's own version string, or None if it is not a usable CLI."""
    try:
        p = subprocess.run([str(exe), "-noheader", "-list", "-c", "SELECT version()"],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           universal_newlines=True, timeout=120)
    except (OSError, subprocess.SubprocessError):
        return None
    return p.stdout.strip() if p.returncode == 0 else None


def asset_name() -> str:
    system = platform.system()
    arm = platform.machine().lower() in ("arm64", "aarch64")
    if system == "Darwin":
        return "duckdb_cli-osx-universal.zip"
    if system == "Linux":
        return f"duckdb_cli-linux-{'arm64' if arm else 'amd64'}.zip"
    if system == "Windows":
        return f"duckdb_cli-windows-{'arm64' if arm else 'amd64'}.zip"
    die(f"no official DuckDB CLI for {system}",
        "build one yourself and pass it with --duckdb")
    raise AssertionError("unreachable")


def fetch_cli(tag: str, into: Path) -> Path:
    """The official CLI at the pin, ~20 MB, thrown away with the scratch dir."""
    asset = asset_name()
    archive = into / asset
    print(f"  no {tag} CLI found, downloading the official one")
    print(f"  {RELEASE.format(tag=tag, asset=asset)}")
    download(RELEASE.format(tag=tag, asset=asset), archive)
    with zipfile.ZipFile(str(archive)) as z:
        z.extractall(str(into))
    archive.unlink()
    exe = into / ("duckdb.exe" if os.name == "nt" else "duckdb")
    if not exe.exists():
        die(f"{asset} did not contain a duckdb binary")
    exe.chmod(0o755)
    if cli_version(exe) is None:
        die(f"the downloaded CLI does not run on this machine ({platform.machine()})",
            "pass one that does with --duckdb")
    return exe


def local_clis(root: Path) -> list:
    """The CLIs this machine already has, most specific first."""
    exe = "duckdb.exe" if os.name == "nt" else "duckdb"
    return [c for c in (os.environ.get("DUCKDB"),
                        root / "build" / "release" / exe,
                        shutil.which("duckdb")) if c]


def find_cli(root: Path, tag: str, override: str | None, scratch: Path) -> Path:
    if override:
        version = cli_version(Path(override))
        if version is None:
            die(f"{override} is not a working DuckDB CLI")
        if version != tag:
            print(f"  ! {override} is {version}, not the pinned {tag};"
                  " the database may not open in your build")
        print(f"  {override} ({version})")
        return Path(override)

    for candidate in local_clis(root):
        version = cli_version(Path(candidate))
        if version is None:
            continue
        if version != tag:
            print(f"  skipping {candidate}: {version}, not the pinned {tag}")
            continue
        print(f"  {candidate} ({version})")
        return Path(candidate)
    return fetch_cli(tag, scratch)


def sql(cli: Path, db: Path, script: str, read_only: bool = False):
    """Feed a script to the CLI on stdin. `.bail on` makes the first error fatal.

    A read-only query gets its stdout captured and leaves no write-ahead log
    behind; a write streams stdout to the terminal, so a long load shows the
    table it is on. stderr is always captured, to be the message when it fails.
    """
    argv = [str(cli), "-batch", "-noheader", "-list"]
    argv += ["-readonly", str(db)] if read_only else [str(db)]
    return subprocess.run(argv, input=".bail on\n" + script,
                          stdout=subprocess.PIPE if read_only else None,
                          stderr=subprocess.PIPE, universal_newlines=True)


def run_sql(cli: Path, db: Path, script: str, read_only: bool = False) -> str:
    p = sql(cli, db, script, read_only)
    if p.returncode:
        stderr = p.stderr or ""
        hints = stderr.strip().splitlines()[-4:] or ["no output"]
        if "Conflicting lock" in stderr:
            hints.append("close the DuckDB that has it open, then run this again")
        die("DuckDB refused the script", *hints)
    return p.stdout or ""


# --- the dump ----------------------------------------------------------------

def extract(tgz: Path, into: Path) -> None:
    print("  extracting ...")
    with tarfile.open(str(tgz)) as tf:
        if sys.version_info >= (3, 12):
            tf.extractall(str(into), filter="data")
        else:
            tf.extractall(str(into))


def csv_for(table: str, root: Path) -> Path:
    direct = root / f"{table}.csv"
    if direct.exists():
        return direct
    found = sorted(root.glob(f"**/{table}.csv"))
    if not found:
        die(f"{table}.csv is not in the dump", f"looked under {root}")
    return found[0]


def load(cli: Path, db: Path, csv_root: Path) -> list:
    ddl = SCHEMA.read_text()
    tables = re.findall(r"CREATE TABLE (\w+)", ddl)
    if not tables:
        die(f"no CREATE TABLE in {SCHEMA}")

    script = [ddl]
    for table in tables:
        path = str(csv_for(table, csv_root)).replace("'", "''")
        script.append(f".print   {table}")
        script.append(f"COPY {table} FROM '{path}' {COPY_OPTS};")
    script.append("CHECKPOINT;")
    print(f"  loading {len(tables)} tables (a few minutes)")
    run_sql(cli, db, "\n".join(script) + "\n")

    counts = "\nUNION ALL\n".join(
        f"SELECT '{t}' AS t, count(*) AS n FROM {t}" for t in tables)
    rows = []
    for line in run_sql(cli, db, f"{counts}\nORDER BY 1;\n", read_only=True).splitlines():
        name, _, n = line.partition("|")
        if n.strip().isdigit():
            rows.append((name.strip(), int(n)))
    return rows


def verify(rows: list) -> None:
    seen = dict(rows)
    for table, n in rows:
        print(f"  {table:18s} {n:>12,}"
              f"{' !' if table in EXPECTED and n != EXPECTED[table] else ''}")
    wrong = [f"{t}: {seen.get(t, 0):,} rows, expected {n:,}"
             for t, n in sorted(EXPECTED.items()) if seen.get(t) != n]
    if wrong:
        die("the loaded data does not match the canonical JOB dump", *wrong,
            "nothing was replaced; the download was probably truncated, so run"
            " this script again")


def check_space(where: Path) -> None:
    try:
        free = shutil.disk_usage(str(where)).free
    except OSError:
        return
    if free < PEAK_GB * 1024 ** 3:
        print(f"  ! only {human(free)} free on {where}; this needs about {PEAK_GB} GB"
              " while it runs")


def already_there(cli: Path, db: Path) -> bool:
    """Tolerant on purpose: a file this CLI cannot even open is one to rebuild."""
    p = sql(cli, db, "SELECT count(*) FROM cast_info;\n", read_only=True)
    out = (p.stdout or "").strip()
    return p.returncode == 0 and out.isdigit() and int(out) == EXPECTED["cast_info"]


def looks_right(root: Path, db: Path) -> bool | None:
    """already_there, answered with a CLI this machine already has, so the
    common 'nothing to do' re-run does not download one to say so. None when
    there is no CLI here yet and the question cannot be answered.

    Any version will do: it only has to read the file, not write it.
    """
    for candidate in local_clis(root):
        if cli_version(Path(candidate)) is not None:
            return already_there(Path(candidate), db)
    return None


def nothing_to_do(db: Path) -> None:
    print(f"  {db} is already there and looks right - nothing to do")
    print("  (pass --force to rebuild it)")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--force", action="store_true",
                    help="rebuild even if the database is already there")
    ap.add_argument("--duckdb", metavar="PATH",
                    help="the DuckDB CLI to build the database with")
    ap.add_argument("--tgz", metavar="PATH",
                    help="keep the download at this path and reuse it on a re-run")
    args = ap.parse_args()

    root = Path(__file__).resolve().parents[1]
    db = root / DB
    if not SCHEMA.exists():
        die(f"{SCHEMA} is missing", "it ships next to this script")

    section("Database")
    known = looks_right(root, db) if db.exists() and not args.force else False
    if known:
        nothing_to_do(db)
        return
    if known is None:
        print(f"  {db} is there, but there is no DuckDB here yet to check it with")
    elif db.exists():
        print(f"  {db} is there but incomplete, rebuilding")
    else:
        print(f"  building {db}")

    section("DuckDB")
    tag = pinned_tag(root)
    db.parent.mkdir(parents=True, exist_ok=True)
    scratch = Path(tempfile.mkdtemp(prefix="imdb-", dir=str(db.parent)))
    try:
        cli = find_cli(root, tag, args.duckdb, scratch)
        if known is None and already_there(cli, db):
            nothing_to_do(db)
            return
        check_space(db.parent)

        section("Downloading")
        tgz = Path(args.tgz).resolve() if args.tgz else scratch / "imdb.tgz"
        if tgz.exists():
            print(f"  reusing {tgz} ({human(tgz.stat().st_size)})")
        else:
            print(f"  {URL}")
            tgz.parent.mkdir(parents=True, exist_ok=True)
            download(URL, tgz)
        extract(tgz, scratch)

        # Built beside the real one and moved into place only once the row
        # counts agree, so a load that dies halfway - or one racing another
        # copy of this script - cannot leave you without a database.
        section("Loading")
        staged = scratch / db.name
        rows = load(cli, staged, scratch)

        section("Row counts")
        verify(rows)
        for stale in (db, db.with_name(db.name + ".wal")):
            if stale.exists():
                stale.unlink()
        os.replace(str(staged), str(db))

        section("Done")
        print(f"  {db}  ({human(db.stat().st_size)})")
        print(f"  duckdb {db} < benchmark/q0000.sql")
    finally:
        if args.tgz and Path(args.tgz).exists():
            print(f"\n  the download is still at {args.tgz}; delete it when you are done")
        shutil.rmtree(str(scratch), ignore_errors=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        raise SystemExit("\naborted")
