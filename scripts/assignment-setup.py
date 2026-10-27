"""Register this repository and your team with the assignment grader.

    python3 ./scripts/assignment-setup.py

Re-run it any time: the answers already in team.json come back as the defaults.
"""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
import urllib.error
import urllib.request
import webbrowser
from datetime import datetime, timezone
from pathlib import Path

APP_URL = "https://github.com/apps/dsa-evaluator-assignment-2"
INSTALL_URL = f"{APP_URL}/installations/new"
TEAM_FILE = "team.json"
SCHEMA = 2
TEMPLATE_REMOTES = {"cwida/dsa-assignment-2-template"}

WORKFLOW = Path(".github/workflows/MainDistributionPipeline.yml")
DOWNLOAD = Path("scripts/download-imdb.py")
DB = Path("data/imdb.duckdb")
BUILD_MINUTES = 33
FREE_MINUTES = 2000
PUSH_TRIGGER = "  push:\n    branches: [ main ]\n"

API = "https://api.github.com"
TIMEOUT = 10.0
SLUG = re.compile(r"^(?:https://|git@|ssh://git@)github\.com[:/]([^/]+?)/(.+?)(?:\.git)?/?$")


def die(msg: str, *hints: str) -> None:
    print(f"\nerror: {msg}", file=sys.stderr)
    for hint in hints:
        print(f"       {hint}", file=sys.stderr)
    raise SystemExit(1)


def section(title: str) -> None:
    print(f"\n{title}\n{'-' * len(title)}")


def git(*args: str, check: bool = True) -> str:
    p = subprocess.run(("git", *args), capture_output=True, text=True)
    if check and p.returncode:
        die(f"git {' '.join(args)} failed", p.stderr.strip() or "no output")
    return p.stdout.strip()


def ask(question: str, default: str = "") -> str:
    try:
        return input(f"{question}{f' [{default}]' if default else ''}: ").strip() or default
    except EOFError:
        raise SystemExit("\naborted")


def confirm(question: str, default: bool = True) -> bool:
    try:
        answer = input(f"{question} [{'Y/n' if default else 'y/N'}] ").strip().lower()
    except EOFError:
        raise SystemExit("\naborted")
    return default if not answer else answer.startswith("y")


def origin_slug() -> str:
    url = git("remote", "get-url", "origin", check=False)
    if not url:
        die("this repository has no 'origin' remote",
            'create your own repo with "Use this template" -> Private, then:',
            "git remote add origin git@github.com:<you>/<repo>.git")
    match = SLUG.match(url)
    if not match:
        die(f"origin is not a GitHub remote: {url}")
    slug = f"{match.group(1)}/{match.group(2)}"
    if slug in TEMPLATE_REMOTES:
        die(f"origin still points at the template ({slug})",
            'use "Use this template" -> Private to create your own repo first')
    return slug


def probe(slug: str) -> tuple[str, dict]:
    """('public', data) | ('hidden', {}) | ('unknown', {}). Unauthenticated, so
    a 404 cannot tell a private repo from one that does not exist yet."""
    request = urllib.request.Request(
        f"{API}/repos/{slug}",
        headers={"Accept": "application/vnd.github+json", "User-Agent": "dsa-setup"})
    try:
        with urllib.request.urlopen(request, timeout=TIMEOUT) as response:
            return "public", json.load(response)
    except urllib.error.HTTPError as e:
        return ("hidden", {}) if e.code == 404 else ("unknown", {})
    except OSError:
        return "unknown", {}


def make_private(slug: str) -> bool:
    if not shutil.which("gh") or not confirm("  Make it private now with the gh CLI?"):
        return False
    edited = subprocess.run(
        ("gh", "repo", "edit", slug, "--visibility", "private",
         "--accept-visibility-change-consequences"), capture_output=True, text=True)
    if edited.returncode:
        print(f"  ! gh failed: {(edited.stderr.strip().splitlines() or [''])[-1]}")
        return False
    return probe(slug)[0] == "hidden"


def ensure_private(slug: str) -> None:
    """A public solution is a plagiarism vector, so this is a hard requirement.
    Only a definite 200 is a failure; 404 and an unreachable API both pass."""
    state, data = probe(slug)
    if state == "hidden":
        print("  private (or not pushed yet)")
        return
    if state == "unknown":
        print(f"  ! GitHub unreachable; check yourself that {slug} is Private")
        return
    print("  ! this repository is PUBLIC - anyone can read your solution")
    if data.get("fork"):
        die("a fork cannot be made private",
            'delete this repo, then use "Use this template" -> Private')
    if make_private(slug):
        print("  now private")
        return
    die("this repository must be Private before it can be graded",
        f"{data.get('html_url', f'https://github.com/{slug}')}/settings",
        "-> General -> Danger Zone -> Change visibility -> Make private")


def load(root: Path) -> dict:
    try:
        previous = json.loads((root / TEAM_FILE).read_text())
    except (OSError, ValueError):
        return {}
    return previous if isinstance(previous, dict) else {}


def choose(question: str, *options: str, default: str) -> str:
    while True:
        answer = ask(f"{question} ({'/'.join(options)})", default).lower()
        for option in options:
            if option.startswith(answer):
                return option
        print(f"  ! choose one of: {', '.join(options)}")


def team_name(default: str, registered: str, notice: bool) -> str:
    if notice:
        print("  Your team name is shown publicly on the leaderboard. If you would")
        print("  rather not be identifiable, pick one that does not give you away.")
        print("  Please do not change it after your first submission.\n")
    while True:
        name = ask("Team name", default)
        if not name:
            continue
        if not registered or name == registered:
            return name
        print(f"  ! this renames the team from '{registered}', which the leaderboard")
        print("    has been showing since your first submission")
        if confirm("  Rename anyway?", default=False):
            return name


def canvas_group(default: str) -> str:
    """The group number is the whole roster: Canvas already knows who is in it,
    so the team's student numbers are never asked for."""
    while True:
        number = ask("Your group number in Canvas", default)
        if number.isdigit():
            return number
        print("  ! enter the number of the group you signed up to in Canvas")


def write(root: Path, previous: dict, slug: str, team: str, group: str) -> Path:
    """Keep the original timestamp when nothing else changed, so a re-run is a
    no-op rather than a fresh commit."""
    payload = {"schema": SCHEMA, "team": team, "canvas_group": group,
               "repository": slug,
               "registered_at": datetime.now(timezone.utc)
                                        .isoformat(timespec="seconds")
                                        .replace("+00:00", "Z")}
    if all(previous.get(k) == payload[k] for k in payload if k != "registered_at"):
        payload["registered_at"] = previous.get("registered_at", payload["registered_at"])
    path = root / TEAM_FILE
    path.write_text(json.dumps(payload, indent=2) + "\n")
    return path


def publish(paths: list[Path], team: str) -> None:
    names = [str(p) for p in paths]
    git("add", "--", *names)
    if not subprocess.run(("git", "diff", "--cached", "--quiet", "--", *names)).returncode:
        print("  already committed and unchanged")
        return
    git("commit", "-m", f"Register team {team}", "--", *names)
    pushed = subprocess.run(("git", "push", "-u", "origin", "HEAD"),
                            capture_output=True, text=True)
    if pushed.returncode:
        print(f"  ! push failed: {(pushed.stderr.strip().splitlines() or ['?'])[-1]}")
        print("    committed locally; run 'git push -u origin HEAD' yourself")
    else:
        print("  pushed")


def build_policy(slug: str) -> bool:
    """A build is the submission, so this asks when one should happen."""
    print("  For grading we need the GitHub Actions workflow in your repository to run,")
    print("  but the free Actions minutes are limited. The workflow is already configured;")
    print("  the only question is how often it runs.")
    print(f"  A build costs ~{BUILD_MINUTES} of the {FREE_MINUTES} free Actions minutes a")
    print(f"  private repo gets each month - about {FREE_MINUTES // BUILD_MINUTES} builds.")
    print("  By default it only runs when you start it yourself:")
    print(f"    https://github.com/{slug}/actions/workflows/{WORKFLOW.name}")
    print("    -> Run workflow")
    return confirm("\n  Do you also want to build on every push to main?", default=False)


def set_autobuild(root: Path, on: bool) -> Path | None:
    """Add or remove the push trigger. Left alone if the file was hand-edited."""
    path = root / WORKFLOW
    if not path.exists():
        return None
    text = path.read_text()
    if (PUSH_TRIGGER in text) == on:
        return None
    edited = (text.replace("  workflow_dispatch:\n",
                           PUSH_TRIGGER + "  workflow_dispatch:\n", 1) if on
              else text.replace(PUSH_TRIGGER, "", 1))
    if edited == text:
        return None
    path.write_text(edited)
    return path


def install(slug: str) -> None:
    print(f"  Grant the App access to exactly one repository: {slug}")
    print(f"  {INSTALL_URL}")
    print("  (opened in your browser)" if webbrowser.open(INSTALL_URL)
          else "  (open that URL yourself)")
    if not confirm(f"Did you install the App and select {slug}?", default=False):
        print("  Nothing is submitted until you do. Re-run this script any time.")


def offer_dataset(root: Path) -> None:
    """The benchmark is useless without the data, but it is a long download, so
    it is a question here rather than something registration does to you."""
    script = root / DOWNLOAD
    if not script.exists():
        return
    if (root / DB).exists():
        print(f"  {DB.as_posix()} is already there")
        return
    print("  The queries in benchmark/ run against the IMDB dataset, which is not")
    print("  in the repository. It is a ~1.3 GB download that becomes a 2.5 GB")
    print(f"  {DB.as_posix()}.")
    print(f"  Any time later: python3 ./{DOWNLOAD.as_posix()}")
    if not confirm("\n  Download it now?"):
        return
    if subprocess.run([sys.executable, str(script)], cwd=str(root)).returncode:
        print(f"\n  ! that did not finish; run ./{DOWNLOAD.as_posix()} again yourself")


def main() -> None:
    root = Path(git("rev-parse", "--show-toplevel"))
    slug = origin_slug()
    registered = load(root)

    section("Repository")
    print(f"  {slug}")
    ensure_private(slug)
    if registered.get("repository") not in (None, slug):
        print(f"  ! {TEAM_FILE} was registered for {registered['repository']}")

    section("Team")
    if registered:
        print(f"  {TEAM_FILE} found, registered {registered.get('registered_at', '?')}")
        print("  press Enter to keep a value\n")

    draft, notice = dict(registered), True
    while True:
        team = team_name(draft.get("team", ""), registered.get("team", ""), notice)
        group = canvas_group(draft.get("canvas_group", ""))

        section("Review")
        print(f"  team        {team}")
        print(f"  canvas      group {group}")
        print(f"  repository  {slug}")
        action = choose(f"\nCommit and push {TEAM_FILE}?",
                        "push", "restart", "cancel", default="push")
        if action == "cancel":
            raise SystemExit("cancelled, nothing written")
        if action == "push":
            break
        draft, notice = {**draft, "team": team, "canvas_group": group}, False
        section("Team")

    section("Being graded")
    auto = build_policy(slug)

    section("Committing")
    changed = [write(root, registered, slug, team, group)]
    for edited in (set_autobuild(root, auto),):
        if edited and edited not in changed:
            changed.append(edited)
    print(f"  {', '.join(p.name for p in changed)}")
    publish(changed, team)

    section("Installing the grader App")
    install(slug)

    section("The IMDB database")
    offer_dataset(root)

    section("Done")
    print(f"  {team}: Canvas group {group} -> {slug}")
    print(f"  builds {'on every push to main' if auto else 'only when you run them'}"
          f", graded nightly")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        raise SystemExit("\naborted")
