# godot-rtx Agent Core

Read this file and the active runtime adapter in `.agents/adapters/` first.
Before each concrete action, load every authority named by its matching row.
Classify only the next action; if unclear, keep it read-only and use the research
row. Reuse unchanged authorities already read. `.agents/README.md` maps the files.
Shared behavior rules win; adapters translate runtime mechanics only.

## Always

- Preserve the owner's requested shape, exclusions, and authorized scope.
  Replace fork-local/game behavior at its authority and remove the old path in
  the same step. Do not introduce unrequested bridges or compatibility paths.
  Shipped upstream Godot APIs retain upstream compatibility requirements.
- Never use worktrees, stash, discard/overwrite working changes, rewrite
  history, reset others' commits, or edit git internals. Work on the current
  branch. Inspect the staged set before every commit; stage only owned changes.
- Godot scenes/resources and source assets use this repository's git policy.
  Do not stage generated caches, build output, or unrelated game content.
- Automated tests are owner-requested only. Comments default to none.
- Inline is limited to comments, strings, include sweeps, literals, typos, a
  one-file rename, or a few-line fix inside one existing function in one file.
  Delegate whole features, new functions/types/bindings/settings, public API or
  cross-file behavior, refactors, and threading/RID/resource-lifecycle work to
  a separate context. Never split a feature to evade this boundary.
  Below-threshold work gets no subagent or review.

## Read Before The Action

| Action | Required authority |
| --- | --- |
| Edit source/config/docs; add or commit | `.agents/shared/core-rules.md` |
| Research APIs/usages/data flow/overlap | `.agents/shared/research-navigation.md`; `docs/reference/research-mode.md`; for C++, `docs/reference/cpp-navigation.md` |
| Write/review a plan | `.agents/shared/research-navigation.md`; `.agents/shared/planning-dispatch.md`; `docs/reference/research-mode.md`; `docs/reference/plan-writing-workflow.md` |
| Implement/delegate a step or review a diff/commit | `.agents/shared/core-rules.md`; `.agents/shared/planning-dispatch.md`; `docs/reference/implementation-mode.md`; `docs/reference/subagent-dispatch.md`; for code, `docs/reference/godot-failure-taxonomy.md` |
| Touch engine C++, GDScript, shaders, scenes/resources, or SCons | `.agents/shared/godot-rules.md`; relevant `docs/reference/godot-failure-taxonomy.md` classes |
| Build or launch an editor/project | `.agents/shared/godot-rules.md` |
| Write/update/run tests | explicit owner request first; `.agents/shared/godot-rules.md`; `docs/reference/testing.md`; relevant failure taxonomy classes |
| Audit existing/requested proof | `.agents/shared/planning-dispatch.md`; `.agents/shared/godot-rules.md`; `docs/reference/testing.md`; relevant failure taxonomy classes |

Engineering references live in `docs/reference/`; workflow policy lives in
`.agents/`. Do not invent APIs, tools, models, paths, or validation results.
