# Codex Adapter

Use `collaboration.spawn_agent` directly, outside `functions.exec`.
`task_name` labels a context; `agent_type` selects a role. Use only fields and
identifiers exposed by the live schema.

Apply Model Selection in `.agents/shared/planning-dispatch.md`:
small = `gpt-5.6-luna`, standard = `gpt-5.6-sol`, strongest = `gpt-6-astra`.
Pass the selected `model` and `reasoning_effort` explicitly with
`fork_turns: "none"` and a self-contained task prompt.

Use `.codex/agents/<role>.toml` through native role selection when it supports
the selected model. If the loaded role pins a different model or native role
selection is unavailable, use a general agent with the manifest's complete
`developer_instructions` and the selected model/effort. This does not apply the
manifest's sandbox; explicitly retain read-only restrictions for reviewers and
proof auditors. Do not silently downgrade to satisfy a role's model pin.

Use the active harness plan artifact before approval. Commit trailers use
`Executing-Agent: <role> (<actual model>)`.
