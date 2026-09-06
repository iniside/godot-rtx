# Grok Adapter

Use only identifiers and fields exposed by the live `spawn_subagent` schema.
Apply shared Model Selection to available models; do not hardcode versions or
inherit the main model as the default. Pass the selected model and supported
effort explicitly; prompt wording alone does not configure runtime effort.

Use the requested implementation, test, review, or proof role when available;
otherwise pass its contract from `.claude/agents/` in a general agent prompt,
retaining reviewer/proof-auditor read-only restrictions. Use `isolation: "none"`
when the schema exposes it, consistent with the repository's worktree ban.
Use the Grok session plan artifact before approval.
Commit trailers use `Executing-Agent: <role> (<actual model>)`.
