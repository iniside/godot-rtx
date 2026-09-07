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
| 1. Slang compiler, request, cache/export and distribution | Implemented; fresh review and proof audit in progress | `3a04df35e6`; [evidence](2026-09-07-1154-slang-compiler-step1-status.md) |
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

Retained PNGs/logs: [gallery](shader-unification-evidence/baseline-gallery.png),
[shadows](shader-unification-evidence/baseline-shadows.png),
[energy](shader-unification-evidence/baseline-energy.png), with the same-name
`.log` files and `baseline-binary.txt` in that directory. Original local files:
`%TEMP%/godot-shader-unification-rendering/baseline-*`.
The project includes the owner's pre-existing `project.godot` changes, preserved
as part of this comparison configuration. No performance improvement is inferred.

## Step 1 ordinary-editor rendering

The parent launched the ordinary rebuilt editor after `3a04df35e6` with the same
gallery capture arguments (without `--gpu-profile`). The actual executable hash
`566EB6333395F1F5E2D13AEC3ACCD8743177B7A5C3B56D7AFBF37BD401D2EB47` matches the
implementer's final-build record. Vulkan/RTX 4090 rendered the
[gallery](shader-unification-evidence/step1-gallery.png); the
[log](shader-unification-evidence/step1-gallery.log) records capture success at
frame 931 and the process exited zero. The image was visually compared with the
baseline: scene geometry, light distribution and visible shadow arrangement remain.
Existing unsupported-material/OpTypeForwardPointer diagnostics are unchanged.

This checks the retained renderer after compiler infrastructure changes. It does
not exercise migrated Slang material shaders, which do not exist until later steps.
No deterministic pixel equality or performance regression claim is made.
