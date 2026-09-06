# Core Rules

## Owning Mistakes - RULE

If you ignore a rule or fabricate an API, path, behavior, or claimed work,
name the mistake, state the correction, and fix it without excuses or praise.
Use two sentences; for a MANDATORY violation name the section and the mismatch.
Do not create memory copies of rules; propose a hook when enforcement is needed.

## Plans And Status Docs - RULE

Keep engineering documents in the repository:

- Plans: `docs/plans/YYYY-MM-DD-HHMM-<kebab-topic>-plan.md`.
- Status/progress/fix/summary:
  `docs/<subdir>/YYYY-MM-DD-HHMM-<kebab-topic>-<status|progress|fix|summary>.md`.
- Durable prose references: `docs/reference/<topic>.md`.

Use UTC and include `HHMM` so listings sort chronologically. Do not put these
documents in the repository root or on a scratch drive. `doc/classes/` holds
upstream Godot class-reference XML; it is not a home for prose.

## Project Knowledge Maintenance - RULE

After meaningful research, implementation, or an owner decision, update the
affected canonical entry linked from `docs/reference/project-state.md` before
handoff. Record the date, revision, evidence source, and actual verification
limits for only the facts checked. Preserve separate evidence stamps when other
entries were not reverified. Replace stale facts instead of appending session
logs, and make no update when the work produced no new durable information.

Work on the current checked-out branch. During plan mode edit only the active
adapter's harness plan artifact. After approval and before implementation, copy
the plan to `docs/plans/`, make links repository-relative, and commit the plan
separately unless the owner requested an uncommitted result.

## Git Safety - MANDATORY

Never create a worktree, stash, restore/checkout working files, clean the tree,
or otherwise discard or overwrite uncommitted changes. Read historical contents
with `git show <sha>:<path>`. Unknown commits and working changes are legitimate.
Never rewrite history, reset other people's commits, delete `.git/index`, or
hand-edit git internals.

The only permitted reset is `git reset --soft HEAD~1` for a commit you created
this turn, provided nobody has committed since. Prefer a follow-up commit.

Before every add or commit, inspect the staged set and stage only owned paths.
Godot scenes, resources, imported source assets, and project files follow this
repository's normal git policy. Never stage generated caches, build output, an
import cache, or unrelated game content.

## Commit Message Format - RULE

Use the repository's `<area>: <Imperative subject>` format. The area names the
touched subsystem; fork-wide feature commits may retain the established
`NVIDIA: <subject>` form. Use the active adapter for any co-author trailer.
Details: `docs/reference/commit-format.md`.

## Comments - MANDATORY

Default: no comment. Allow one present-tense line only for a non-obvious
invariant, driver/hardware gotcha, workaround reason, or intent invisible in
the code.

Ban changelog prose, code paraphrases, multi-line body summaries, false behavior
claims, and exclusivity claims. Engine-behavior claims require reading that
function this session and citing its source anchor in the handoff or review.

Delete comments flagged by review rather than rewriting them. Comment-only
findings end the round; fix inline, never spawn a prose-fixing agent. Each
implementation handoff says `comments: default NONE` and names at most one or
two details deserving a line.

## Compatibility - MANDATORY

For fork-local behavior and game content, delete obsolete APIs and data paths
instead of adding migrations, version branches, deprecation shims, compatibility
fields, toggles, bridges, or a second implementation. Replace the behavior at
its authority and remove the old path in the same step. If continued use is
unclear, ask one question instead of preserving both paths.

An API shipped by an upstream stable Godot release retains upstream compatibility
requirements for user projects and GDExtension. Determine whether an API is
upstream or fork-local from this tree and its history before removing or
re-signing it; apply the upstream binding/XML compatibility mechanism where
required.
