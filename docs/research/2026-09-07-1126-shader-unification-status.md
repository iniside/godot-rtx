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
| 1. Slang compiler, request, cache/export and distribution | Source review and proof audit PASS | `3a04df35e6`, fix `836f8c7e77`; [evidence](2026-09-07-1154-slang-compiler-step1-status.md), [proof supplement](2026-09-07-1219-slang-step1-proof-supplement-status.md) |
| 2. Spatial frontend and surfaces | Final source review and bounded proof audits PASS | `675d3cbe7f`, fix `1a5faf5c60`; [evidence](2026-09-07-1305-shader-unification-step2-summary.md), [fix](2026-09-07-1323-shader-unification-step2-round1.md); task baseline `836f8c7e77` |
| 3. Shared shading and DI replacement | In progress | Released after step-2 PASS; task baseline `1a5faf5c60` |
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

## Step 1 review

Fresh round 1 reviewed exact `3a04df35e6` and cumulative `b2d04129cb..3a04df35e6`:
REJECT, one P1 build-graph defect. `glsl_builders.py:298-304` resolves SDK includes
relative to process cwd; SCons changes cwd for nested SCsub execution, so
`Rtxdi/DI/Reservoir.hlsli` fails during the synchronous dependency scan. Local
relative-include diagnostics did not cover this path. The correction must anchor
physical SDK paths at repository root and preserve stable virtual include identity.
Correction `836f8c7e77` passed actual nested SCons SDK/local include generation and
production ShaderRD/Vulkan creation. Fresh final round 2 returned PASS for exact
fix, original commit and cumulative `b2d04129cb..836f8c7e77`.

The independent proof audit returned PASS after the
[supplement](2026-09-07-1219-slang-step1-proof-supplement-status.md) supplied
fail-closed temporary diagnostic exits and exact launch/spirv-val receipts.
CLAS, ABI and nested ShaderRD creation exited zero; the intentional missing-DLL
compile failure exited one; both SPIR-V validator runs exited zero. Production
code remained unchanged. This proves compilation and Vulkan object creation,
not dispatch, matrix math or migrated rendering. Step 2 is released to build.

## Step 2 integration findings

Parent source inspection confirmed that legacy color lighting is unreachable by
the active scene draw. `RenderForwardClustered::_render_scene` fills and draws
`PASS_MODE_RTXDI_SURFACE`, including editor debug-material selection;
`_render_material` and `_render_uv2` use `PASS_MODE_DEPTH_MATERIAL`. History
`a661887676` removed opaque/motion/alpha color draws and rejected reflection
captures. Root clangd references, declarations/implementations and bounded text
inspection identified remaining color registration/prewarm consumers. The step-2
writers remove these together with the old shader body and prewarm the actual
six-attachment surface instead. Depth/shadow/material paths remain in scope.

The first real Vulkan launch exposed Slang's downstream optimizer eliminating
unused resource parameters despite PreserveParameters. Pinned SDK source and
same-source API diagnostics isolated O2 (3 descriptors) versus native O0 output
(31 descriptors). The internal Slang request now selects native O0 output;
Godot retains its existing final reflection/container and Vulkan performs final
driver compilation. No extra optimizer or dummy shader reads were introduced.
Pinned `Linkage::addTarget` also overwrites session FloatingPointMode from the
TargetDesc field; precise mode is now supplied at that target field. A narrow
vertex-position Invariant decoration closes the existing position contract.

The parent visually compared `step2-gallery-o0.png` from the ordinary Vulkan
editor with the retained baseline: visible geometry, materials and shadow/light
arrangement remain. This is not pixel equality or a performance claim. Final
step-2 proof/commit/review remains pending; the implementer retains detailed
commands, binary provenance and final shader diagnostics.

The proof auditor could not find a retained final vertex spirv-val invocation.
The parent reran only that validator on the unchanged retained artifact on
2026-09-07 13:15 UTC: Vulkan 1.3 validation exited zero with empty output.
The [receipt](shader-unification-evidence/step2-final-vertex-validation.json)
records exact argv, input/tool hashes, time and result. This supplements the
frozen evidence; it does not claim an earlier unretained invocation was verified.

Fresh source review round 1 returned REJECT with three P2 AST defects: unsigned
native results for signed findMSB/findLSB/bitCount need their actual AST result
types restored; roundEven must emit rint/RoundEven rather than round/Round; and
textureQueryLod must evaluate texture/coordinate arguments once. The original
implementer owns this named fix, followed by one fresh final round-2 review.

Fix `1a5faf5c60` received fresh final round-2 source review PASS and a bounded
proof audit PASS. All three reported defects are closed; actual AST-generated
numeric outputs and final query SPIR-V cover their documented failing branches.
The parent also visually inspected its
[ordinary-editor gallery capture](shader-unification-evidence/step2-gallery.png)
against the retained baseline: visible material/geometry/shadow arrangement
remains. The source-matched fix build and capture provenance are indexed in the
[fix report](2026-09-07-1323-shader-unification-step2-round1.md).

The independent proof audit returned PASS for the retained bounded observations.
It explicitly does not establish visible UBO hot-reload semantics (the rich probe
writes VERTEX and triggers unsupported-material routing), AST-generated numeric
matrix correctness, or final-source gallery equivalence. The numeric kernels
are hand-authored, and gallery preceded the final AST rebuild. Supported visible
uniform changes and nontrivial AST output remain step-5 checks. Diagnostic process
exit alone is not a gate: the auditor inspected actual artifacts/results, because
some temporary diagnostic helpers return normally even after failure. Dormant PT
hit-group dump warnings remain distinct from spatial compilation diagnostics.

Step-3 preparatory analysis also identified a direct DI shade dependency on the
handwritten NRD packing helpers. The final thin pinned-SDK wrapper prerequisite
moves into step 3 so native DI can compile; full NRD/HDR frame migration stays in
step 4. This changes dependency order within the approved scope. Replaced GLSL
helpers are removed in their replacement step, even if dormant PT/frame includes
remain temporarily broken until step 4; no temporary duplicate math is introduced.
