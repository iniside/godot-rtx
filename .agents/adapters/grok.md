# Grok Adapter

Read `AGENTS.md` first, then this adapter. Load only the authorities required
for the next action. `CLAUDE.md` is a compatibility loader; shared rules and
this adapter control Grok behavior.

Use only identifiers exposed by the live `spawn_subagent` schema. For schemas
that expose `grok-4.5` and `grok-4.6`, use 4.5 for mechanical, research, and
ordinary test-author work, and 4.6 for independent work, hostile review, proof
audit, and novel test harnesses. Pass the model explicitly and put the approved
or default effort in the prompt. Do not ask again after plan approval.

Map `[independent]` to `core-implementer`, `[mechanical]` to the general
implementation role, `[test-author]` to `test-author`, `[review]` to a fresh
`hostile-reviewer`, and proof audits to a fresh `proof-auditor`. Research is
read-only. Paste the C++ navigation chain into code-touching prompts.

Use `isolation: "none"`; this repository bans worktrees. Never resume a
reviewer. Reviewers need shell/git access to inspect the exact task commit and
cumulative task diff, but must not edit. Automated tests require an explicit
owner request. During plan mode use the Grok session plan artifact, then copy
the approved plan to the repository path required by shared rules. Use the
repository `<area>: <Imperative subject>` format and a truthful
executing-model trailer.
