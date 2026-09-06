# Cursor Adapter

Use only task and model identifiers exposed by the live Cursor schema.
Apply shared Model Selection to the available models; do not hardcode versions
or inherit the main model as the default. Pass the selected model and supported
effort explicitly; prompt wording alone does not configure runtime effort.

Project role contracts live in `.claude/agents/`. Use native roles when they
support the selected model; otherwise pass the relevant contract in a general
agent prompt, retaining reviewer/proof-auditor read-only restrictions.
Use the current Cursor plan artifact before approval.
Commit trailers use `Executing-Agent: <role> (<actual model>)`.
