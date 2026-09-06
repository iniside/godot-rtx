---
name: research-plan-present
description: Research a non-trivial godot-rtx change, write a concrete implementation plan, and present it for approval before editing. Use for new APIs/types/settings, rendering or shader work, cross-file behavior, refactors, and other work above the repository's inline threshold.
---

# Research, plan, present

Read `AGENTS.md` and the active runtime adapter. Then follow these authorities
instead of duplicating their policy here:

1. Research with `.agents/shared/research-navigation.md`,
   `docs/reference/research-mode.md`, and for C++
   `docs/reference/cpp-navigation.md`. Read APIs from this Godot 4.8-dev fork;
   report the navigation method and treat grep as a lower bound.
2. Plan with `.agents/shared/planning-dispatch.md` and
   `docs/reference/plan-writing-workflow.md`. Preserve the owner's requested
   shape and exclusions, map overlapping APIs and consumers, name exact files
   and symbols, order dependencies, and assign each whole step a dispatch lane.
3. Present the reviewed plan for approval before implementation. After
   approval, follow `docs/reference/implementation-mode.md` and
   `docs/reference/subagent-dispatch.md`; do not ask again for approved lanes
   or model/effort selected by shared Model Selection.

Include tests only when the owner explicitly requests them. Keep the harness
plan artifact until approval, then copy the approved plan to the repository
path required by shared rules.
