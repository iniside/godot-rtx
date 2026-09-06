# Claude Adapter

Read `AGENTS.md` first, then this adapter. Load the authorities for the next
concrete action only and reuse unchanged authorities already read. Shared rules
define behavior; this file maps them to Claude Code.

During plan mode edit only `C:\Users\lukas\.claude\plans\<slug>.md`. After
approval, copy the plan to the repository path required by shared rules before
implementation. The approved dispatch tags authorize their lanes; use the
defaults below unless the owner already chose another effort. Do not ask again
after approval.

Use project roles from `.claude/agents/` and pass `model` explicitly on every
`Task` or `Agent` call. Effort does not inherit, so include the approved or
default level in the prompt. Paste the C++ navigation chain from
`docs/reference/cpp-navigation.md` into every code-touching prompt.

- `[inline]`: current Claude context, only inside the shared threshold.
- `[independent]`: `core-implementer`, `model: "opus"`, high effort.
- `[mechanical]`: `core-implementer`, `model: "sonnet"`, medium effort.
- `[test-author]`: `test-author`, `model: "sonnet"`, medium effort; Opus only
  for a novel harness or topology. Tests require an explicit owner request.
- `[review]`: a fresh `hostile-reviewer`, model at least the author's tier,
  high effort.
- Proof audit: a fresh `proof-auditor`, model at least the author's tier, high
  effort.
- Research: read-only `Explore`/`general-purpose`, Sonnet and medium effort;
  Haiku may be used for listing-only work.

Review the exact task commit and cumulative diff from the task-start commit to
that captured commit SHA. Never resume or message a previous reviewer. Use the
repository `<area>: <Imperative subject>` format and a truthful Claude
executing-model trailer. Project skills live in
`.agents/skills/`; `.claude/skills/` contains compatibility redirects.
