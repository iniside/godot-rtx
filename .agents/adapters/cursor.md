# Cursor Adapter

Read `AGENTS.md` first, then this adapter. Load only the authorities required
for the next action. `CLAUDE.md` is a Claude compatibility loader; shared rules
and this adapter control Cursor behavior.

Use only model and task identifiers exposed by the live Cursor schema. Prefer
the non-fast Composer 2.5 family for mechanical, research, and ordinary
test-author work, and the non-fast Grok 4.6 family for independent work,
hostile review, proof audit, and novel test harnesses. Never use `inherit`, a
Claude model slug, or an invented Cursor slug. Pass the model explicitly.

Project custom agents are loaded from `.claude/agents/`. Map `[independent]`
to `core-implementer`, `[mechanical]` to the schema's general implementation
role, `[test-author]` to `test-author`, `[review]` to a fresh
`hostile-reviewer`, and proof audits to a fresh `proof-auditor`. Research is
read-only. State the approved or default effort in each prompt; do not ask again
after plan approval. Paste the C++ navigation chain into code-touching prompts.

Never create a worktree or resume a reviewer. A reviewer must inspect the exact
task commit and cumulative task diff with shell/git while remaining read-only.
Automated tests require an explicit owner request. Use the current Cursor plan
artifact before approval, then copy the approved plan to the repository path
required by shared rules. Use the repository
`<area>: <Imperative subject>` format and a truthful executing-model trailer.
