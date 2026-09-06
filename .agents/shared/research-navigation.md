# Research And Navigation

## Project Knowledge First - RULE

Before project research, read `docs/reference/project-state.md` and the links
relevant to the question. Start from its source anchors and reuse unchanged
evidence. When freshness matters, inspect the scoped diff for affected paths,
including working changes since the entry's recorded revision. Reopen only
missing, stale, or contradictory gaps; a new session or HEAD alone does not
justify another whole-repository scan.

If the recorded baseline is unavailable, fall back to targeted source reads.
Current source is authoritative for implementation facts, while recorded owner
direction is authoritative for product decisions.

## Research / Search Mode - RULE

Before research, read `docs/reference/research-mode.md` and, only when
dispatching, `docs/reference/subagent-dispatch.md`. State the unresolved
question and give it one owner. Reuse completed research unless it is missing,
stale, contradictory, or insufficient. A handoff provides verified source
anchors, conclusions, open questions, and the method used for each answer.

Stop when the evidence supports the decision or change. Expand only for a
concrete dependency, contradiction, or failing case. If available methods
cannot resolve a remaining point, disclose that uncertainty instead of
repeating fruitless searches.

## Research Never Overrules a Decision - MANDATORY

Research supplies facts, never permission to reverse the owner's choice.
Preserve the chosen outcome, exclusions, and reasons in the owner's terms;
check each plan step against them. Do not narrow an objection, substitute a
rejected coupling, or treat cost or feasibility as grounds to override it. If
a step needs the rejected design, ask one blocking question instead of silently
revising the plan.

## C++ Code Navigation - RULE

Use this fallback chain in order:

1. LSP/clangd definition, references, hover, or workspace symbol against the
   repository-root `compile_commands.json`.
2. Read the declaration and implementation in this 4.8-dev tree; remembered
   signatures from another Godot release are not evidence.
3. For script-facing APIs, inspect `_bind_methods()` and
   `doc/classes/<Class>.xml`, including properties, enum constants, signals,
   defaults, and compatibility bindings.
4. Use `git log -p`, `git blame`, and the relevant upstream comparison to
   distinguish shipped upstream API from fork-local behavior.
5. Use `rg` only after those methods fail or for a bounded text/config sweep;
   label the result a lower bound. Once it provides a symbol anchor, return to
   clangd/source navigation.

For SCons/Python, GLSL include/build graphs, GDScript, and text scenes/resources,
use the relevant parser or targeted source/config reads rather than pretending
clangd covers them. Do not search generated build output, `.git`, import caches,
or `thirdparty/` for project API evidence. Paste this chain into every
code-touching handoff and require the method used in its report. Details:
`docs/reference/cpp-navigation.md`.
