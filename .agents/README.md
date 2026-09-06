# Instruction Map

`AGENTS.md` is the mandatory core and action router. Read it plus the active
runtime adapter first, then load only the authorities for the next concrete
action. If that action is unclear, keep it read-only and use the research row.
Reuse already-read authorities while they remain unchanged.

- `shared/core-rules.md`: documents, git, commits, comments, and compatibility.
- `shared/research-navigation.md`: research boundaries and C++ navigation.
- `shared/planning-dispatch.md`: planning, implementation routing, and review.
- `shared/godot-rules.md`: Godot C++/SCons, APIs, assets, rendering, and builds.
- `adapters/<runtime>.md`: runtime tools, models, effort, trailers, and artifacts.
- `docs/reference/project-state.md`: maintained project facts, source anchors,
  decision links, evidence limits, and known gaps.
- `docs/reference/*.md`: full action-specific contracts linked by the router.

Shared authorities define behavior. Adapters translate runtime mechanics and
must not weaken the shared rules.
