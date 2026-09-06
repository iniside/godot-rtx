# Claude Adapter

Use project roles from `.claude/agents/` through the live `Task` or `Agent`
schema. Apply shared Model Selection: Haiku for simple bounded work, Sonnet as
the standard model, Opus for the strongest tier. Pass the selected supported
`model` explicitly. Set effort through a runtime field when available;
otherwise state the requested level in the prompt without claiming it configures
the runtime. Research uses a read-only role or prompt.

During plan mode use `C:\Users\lukas\.claude\plans\<slug>.md`.
Project skills live in `.agents/skills/`; `.claude/skills/` contains redirects.
Commit trailers use `Executing-Agent: <role> (<actual model>)`.
