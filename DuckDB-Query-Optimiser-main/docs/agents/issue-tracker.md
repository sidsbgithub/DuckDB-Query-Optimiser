# Issue tracker: GitHub

Issues and specs live in GitHub Issues for
Mustafa-Yusa-Babademez/dsa-assignment-2. Use the `gh` CLI
from this repository.

## Conventions

- Create: `gh issue create --title "..." --body-file <file>`
- Read: `gh issue view <number> --comments`
- List: `gh issue list --state open --json number,title,body,labels,comments`
- Comment: `gh issue comment <number> --body-file <file>`
- Add labels: `gh issue edit <number> --add-label "..."`
- Remove labels: `gh issue edit <number> --remove-label "..."`
- Close: `gh issue close <number>`

For multiline bodies, write the exact Markdown to a temporary
file and pass it with `--body-file`.

When a skill says "publish to the issue tracker", create a
GitHub issue. When it says "fetch the relevant ticket", read
the issue and its comments.

## Pull requests as a triage surface

PRs as a request surface: no.

## Wayfinding operations

Use a single issue labelled `wayfinder:map` for the map.
Link child tickets using GitHub sub-issues when available;
otherwise use a task list in the map and a `Part of #<map>`
line in each child.

Label children `wayfinder:<type>`, where type is research,
prototype, grilling, or task.

Record blockers with native GitHub issue dependencies when
available; otherwise use a `Blocked by: #<number>` line.
A ticket is unblocked when every blocker is closed.

Select the first open, unassigned, unblocked child in map
order. Claim it with `gh issue edit <number> --add-assignee @me`.

When resolved, comment with the result, close the ticket,
and add a summary and link to the map's Decisions-so-far.
