# Codex Adapter

Read `AGENTS.md` first, then this adapter. Load the authorities for the next
concrete action only and reuse unchanged authorities already read. Shared rules
define behavior; this file only maps that behavior to Codex tools.

## Dispatch

Call `collaboration.spawn_agent` directly, outside `functions.exec`. Use only
fields and models exposed by the live schema. `task_name` labels the thread; it
does not select or load a role.

When the live schema exposes native custom-agent selection, use the role in
`.codex/agents/<role>.toml`. When it does not expose `agent_type`, read that
manifest and pass its complete `developer_instructions` verbatim in a
self-contained prompt with `fork_turns: "none"`, plus the explicit model and
effort below. Do not invent an `agent_type` field. This fallback carries the
role contract, but cannot apply the manifest's `sandbox_mode`; the spawned
agent inherits the live runtime permissions, so reviewer and proof-auditor
prompts must repeat that they are read-only and must never edit.

| Lane | Role/model | Effort |
| --- | --- | --- |
| inline | Main agent, only inside the shared threshold | current |
| independent | `core-implementer`, `gpt-5.6-sol` | high |
| mechanical | `core-implementer` contract, `gpt-5.6-luna` | medium |
| test-author | `test-author`, `gpt-5.6-luna` | medium |
| review | fresh `hostile-reviewer`, `gpt-5.6-sol` | high |
| proof audit | fresh `proof-auditor`, `gpt-5.6-sol` | high |
| research | fresh read-only prompt, `gpt-5.6-luna` | medium |

Pass `model` and `reasoning_effort` explicitly with `fork_turns: "none"`
when the live schema exposes those fields. An approved plan already authorizes
its lanes; use the listed default effort unless the owner chose another level.
Do not ask again after approval. Reviewers are always fresh and receive the
original request, approved plan when applicable, exact task commit, and the
cumulative task diff range.

Automated tests are owner-requested only. Test authoring follows the landed,
compiling implementation and visual inspection when the behavior is visual.
Proof audit applies to executable tests, fixtures, and verification changes,
not policy prose.

Use the repository `<area>: <Imperative subject>` commit format and a truthful
executing-agent/model trailer.
Use the active harness plan artifact before approval; after approval copy the
plan to the repository path required by shared rules before implementation.
