# Shader unification implementation status

Started 2026-09-07 11:26 UTC. Overall source baseline:
`a89cd8a3b064427e29a89af0f0b6823bfac21da7`.

Owner approved execution with "no to zaczynaj" after presentation of the reviewed
[plan](../plans/2026-09-07-1126-shader-unification-plan.md). The plan landed in its
own commit `b2d04129cb8f1ad0b0d81d6b112cbbc2be6a4272` before implementation.
Research and standalone compiler limitations are recorded in the
[research report](2026-09-07-1112-shader-unification-summary.md).

| Step | State | Evidence |
|---|---|---|
| 1. Slang compiler, request, cache/export and distribution | In progress | Separate core-implementer context; task baseline `b2d04129cb` |
| 2. Spatial frontend and surfaces | Pending step 1 | No implementation evidence |
| 3. Shared shading and DI replacement | Pending step 2 | No implementation evidence |
| 4. NRD/HDR and removal of PT dependency | Pending step 3 | No implementation evidence |
| 5. Final validation and documentation | Pending implementation | No new renderer/runtime proof |

The selected step-1 implementer is `core-implementer (gpt-6-astra)`, high effort,
because compiler ABI, concurrency, shader cache/export and RD integration cross
subsystem boundaries. Fresh exact-commit review follows the completed step before
dependent implementation. No automated tests are authorized.

Existing dirty scene/project/content files and pre-existing documentation edits
are preserved. Only task-owned paths/hunks may be staged. The completed research
document and this status are task-owned; canonical index/direction files contain
earlier edits and require scoped staging. No worktrees, stash or cache cleanup.

## Before-migration rendering reference

The existing editor rendered three unchanged manual scenes on Vulkan/RTX 4090:
`main.tscn`, `shadows_merged.tscn`, and `energy_directional.tscn`. Each used the
existing `--still --delay=8 --capture=...` demonstration command, saved its real
viewport image and exited zero. Images were visually inspected. No new fixture,
assertion, shader hook or automated test was introduced.

Binary SHA-256: `55967155821B21DFF843D2FEFA663B84160405167714EAE7C98764D43EA795A2`.
Its embedded revision is `b01c5849a8d10397be3e977347b0a49f45c90db5`; this is the
pre-existing editor, not a new build of the plan commit. These references establish
observed before-migration behavior, not new source-to-binary provenance. Existing
magenta unsupported-material and OpTypeForwardPointer diagnostics are retained.

Local PNGs and full logs: `%TEMP%/godot-shader-unification-rendering/baseline-*`.
The project includes the owner's pre-existing `project.godot` changes, preserved
as part of this comparison configuration. No performance improvement is inferred.
