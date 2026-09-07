# DDGI / PT / RR implementation status

Owner approval: "implementuj plan", 2026-09-07 UTC.
Approved [plan](../plans/2026-09-07-1635-ddgi-pt-rr-plan.md), committed separately
as `00f73fb82f52e6244124293b5061c7f36d035487`.
Whole-task baseline: `381c36eec7d7e0d5c9111be366a3a0ae78cdadd3`.
[Integration research](2026-09-07-1635-ddgi-pt-rr-integration-summary.md).

## Required outcome

Hybrid RTXDI + automatically created camera-following DDGI from its first
working version; shared native Slang hit materials and RT scene; optional true
camera-ray PT with raw progressive reference; NRD/RR selection and retained
DLSS SR; scene settings, resource/history ownership, real Vulkan validation and
DLSS model provenance. Preserve the full plan's exclusions and validation matrix.
Automated tests are not authorized. The full implementation is not complete.

## Sequence

| Step | Status | Commit / evidence |
|---|---|---|
| 1. Public settings and per-buffer ownership | Source review PASS | `8aa9a77747`; ordinary editor builds |
| 2. Common RT coordinates and temporal basis | Final round 2 source PASS and proof PASS | `eecc668b45` + correction `28dbaa3dd2` |
| 3. Shared native RT hit materials | Final round 2 REJECT; owner authorized remaining fix beyond cap | `d3936e37b7` + `faa1f8dcfe`; bounded proof audit running |
| 4. Pinned DDGI import and moving cascades | Pending | Depends on step 3 |
| 5. DDGI lighting and hybrid composition | Pending | Depends on step 4 |
| 6. True camera-ray PT | Pending | Depends on step 5 |
| 7. RR / DLSS lifecycle and composition | Pending | Depends on camera producers |
| 8. Integrated real-device validation and documentation | Pending | Full plan matrix |

## Step 1 evidence

Frozen `8aa9a77747340a8581c0b655e1d67d7eda1b6f2d`, baseline `00f73fb82f`:
21 source/XML files add ten Environment controls, enum/server/storage closure,
per-buffer history ownership and scenario-change render-buffer teardown.
Fresh `ddgi_step1_review_r1` returned PASS against the exact commit and both
step/whole cumulative ranges. Classes 1-9 were checked using clangd, source/XML,
history and bounded textual inventories. No concrete defect was found.

Ordinary editor/console builds completed with
`scons platform=windows target=editor accesskit=no d3d12=no -j16`.
Logs: `C:/Users/lukas/AppData/Local/Temp/ddgi-step1-editor-build.log` and
`ddgi-step1-editor-final-build.log` in the same directory; final elapsed 1:04.21.
Interactive save/reload, scenario switching and new GPU behavior were not proved.

## Step 2 implementation and review

Step baseline `8aa9a77747340a8581c0b655e1d67d7eda1b6f2d`.
Initial commit `eecc668b454a2239165a1273e78b0e6f260b08e0` added RT-relative
camera/AS/light/motion packing, previous MultiMesh positions and scene generations.
Parent documentation commit `7db5d592e0` was interposed without source changes.

Fresh round 1 source review rejected four concrete defects:

- Previous-position buffer capacity was coupled to the combined position/TBN
  capacity. A 48-to-72-byte previous-position requirement could retain 48 bytes
  while the combined allocation remained sufficient (`render_raytracing.cpp:2347`
  at the rejected commit; merge shader writes at `multimesh_merge.glsl:134`).
- Storage-wide material/texture generations invalidated unrelated worlds and
  static viewports (`render_raytracing.cpp:3218`).
- Blanket frame hashing invalidated frozen TextureRD/ViewportTexture producers
  and projector/sky inputs (`:3227`, `:3360`, `sky.cpp:1001`).
- Raster-only time metadata missed TIME used exclusively in an RT variant
  (`render_raytracing.cpp:2917`, `scene_shader_forward_clustered.cpp:333`).

Correction `28dbaa3dd258fc1b372d8fb83784988c8c27aa64` modifies 14 files;
cumulative Step 2 covers 28 source files. Independent previous-position capacity,
relevant material/uniform/texture snapshots, RDG backing-resource write stamps,
and selected RT TIME/PREV_TIME metadata replace the defective paths. Root aliases
share write identity; read/bind operations and descriptor rebuilds do not create
content changes. CPU origin subtraction precedes float packing; raster SceneData
and public shader world semantics remain intact. Ordinary camera motion preserves
DDGI history; basis shifts explicitly invalidate screen history.

Fresh final `ddgi_step2_review_r2` returned PASS, examining the correction,
initial commit, step cumulative and whole-task cumulative ranges. Classes 1-9
covered bindings, resource capacity/lifetime, RD command coverage, shader layout,
precision/build wiring, threads, compatibility and removed obsolete paths.
No concrete remaining source defect was found.

## Step 2 proof

Evidence directory:
`C:/Users/lukas/AppData/Local/Temp/godot-ddgi-step2-20260907/round1-fix`.
`provenance.json`, `correction.diff` and `step2-cumulative.diff` retain identity.
Fresh `ddgi_step2_proof_r2` returned PASS after matching all 28 frozen source
hashes, 93 artifact hashes, both diffs and generated shader inputs.

| Validation | Retained result |
|---|---|
| Ordinary editor, `scons platform=windows target=editor accesskit=no d3d12=no -j16` | Exit 0; SCons 41.40 s; `editor-single.log` and receipt |
| Same command with `precision=double` | Exit 0; SCons 44.13 s; `editor-double.log` and receipt |
| Sixteen DI pass/precision/radiance-layout permutations | Nonempty SPIR-V/container pairs; valid RTX 4090 Vulkan shader/pipeline RIDs; `shaders/run.log` |
| Sixteen DI SPIR-V validations | Individual commands/exits/stdout/stderr, all exit 0; `shaders/spirv-validation.json` |
| Indexed/nonindexed merged-MultiMesh compiler and validator | Four exact command receipts, all exit 0; `merge-receipts.json` |
| RT frame constant layout | All 16 artifacts: binding 45, set 0, offsets 0/48, float4[3] stride 16; CPU size 96/alignment 16 |

Before/after build hashes match final sources. The initial corrective C2662
const-getter failure remains archived separately and was fixed before successful
builds. The first proof audit rejected missing independent merge-validator
receipts; the fresh corrective receipts close that finding. The historical
standalone-container exit alone is insufficient: actual nonempty artifacts and
separate validator receipts were checked. Existing re-spirv unsupported-operation
and material-envelope messages remain recorded; no error-free image is claimed.

## Step 3 evidence awaiting review

Frozen `d3936e37b7f8e3c900416da0a2e70ee40096fa4d`, baseline `28dbaa3dd2`:
31 source/shader files, +1958/-791. Fresh `ddgi_step3_review_r1` returned REJECT
after examining the exact commit and both cumulative ranges. A separate proof-auditor spawn hit the
harness thread limit, including a retry after the writer completed. Neither
review nor proof PASS is claimed. Correction `faa1f8dcfe` is committed;
fresh final `ddgi_step3_review_r2` returned REJECT. The post-correction
proof-auditor spawn hit the harness limit while source review was active, then
`ddgi_step3_proof_final` started successfully after the reviewer completed.

Final round 2 confirms the initial three findings corrected but rejects one
remaining LOD defect: `rt_hit_context_inc.slang:236,248` clamps the raw footprint
before `shader_compiler.cpp:772` applies bias. Raw LOD 10, last mip 8 and bias -2
therefore select mip 6 instead of 8. `shader_compiler.cpp:724` similarly returns
`textureQueryLod=(8,8)` instead of clamped/unclamped `(8,10)`. Recommended fix:
preserve raw footprint LOD through bias and the unclamped query component, then
clamp only final sampled/clamped-query values. The owner explicitly overrode the
review-round cap with "ani sie waz naprawiaj, z goal mozesz isc ponad limit" and
directed the remaining fix and continued goal execution. Additional correction
and fresh review are authorized. Resuming the original writer currently hits the
harness thread limit while the independent proof audit is active. That audit
examines retained compilation/pipeline-creation claims, not overall correctness.

Confirmed round 1 defects:

1. `texture_storage.cpp:4384` seeds each RT decal snapshot with storage-global
   generation, so an unrelated scenario's decal motion resets PT history even
   in a target scenario with no decals.
2. `rt_hit_context_inc.slang:319` retains a cross-reconstructed merged-MultiMesh
   binormal after nonuniform scaling. Scale (2,1,1), tangent normalize(1,1,0)
   yields the wrong bitangent direction versus raster/separate geometry.
3. `shader_compiler.cpp:794` propagates only UV/UV2 basis selection, while
   `rt_texture_lod` ignores coordinate expression scaling. `UV * 64` therefore
   receives the same implicit mip as UV. The correction must propagate the
   sampled expression footprint, including intermediate expressions, while
   retaining explicit Grad/Lod operands.

Corrective research, 2026-09-07: the [pinned Slang auto-diff guide](https://github.com/shader-slang/slang/blob/84792eb15/docs/user-guide/07-autodiff.md)
documents forward differentiation, control-flow support and custom derivative
functions. This lookup was not compiler/runtime proof. The final correction
instead propagates central and two neighboring values through the existing
ShaderCompiler lowering, including user functions, arrays and structs.

### Corrective evidence

Frozen `faa1f8dcfecc25a77ddd97381f7fd8d718ea4746`: nine files, +466/-152.
`round1-final-handoff.json` and `round1-staged.patch` in the evidence root below
record source identity, exact commands and artifacts. The author reports:

- Snapshot identity uses admitted packed decals and relevant source/atlas
  texture content, without storage-global decal generation.
- Merged geometry retains an original vertex-buffer address in existing ABI
  padding and registers the buffer in geometry dependencies; native hits use
  original local TBN before instance transforms.
- Implicit sampling uses propagated central/neighbor coordinates. Explicit
  Grad/Lod operands remain intact; control flow uses the central evaluation.
- Final ordinary/double/template receipts (`round1-editor-final-2`,
  `round1-double-final-1`, `round1-template-final-1`) report exit 0 and the same
  35 stable source hashes matching committed inputs.
- `hit-shaders/{ordinary,double}-round1-final-2` records all eight native stage
  compilations and validators passing after the any-hit signature correction.
- `round1-coordinate-final-1` reports exit 0 and five pipeline/SBT creations
  across source reload, VisualShader edit and restore; `round1-gallery-final-1`
  reports exit 0, three programs and 15 geometry records. These final runs have
  no ERROR lines in stderr.

The parent checked successful build logs and the frozen commit. These results
await the independent source verdict and proof audit. They do not establish
native ray dispatch/readback, output mip selection, or GPU-observed TBN/history
behavior. The following original-step evidence remains historical at `d3936e37b7`.

The implementation adds native Slang material programs, pipeline-local SBTs,
shared material packing/classification, geometry hit inputs, resident decal
snapshots and shared shader operations. The author reports explicit screen-input
diagnostics, UV2 footprint selection, TBN parity and reload cleanup corrections.
These remain subject to the independent source review.

Evidence root: `C:/Users/lukas/AppData/Local/Temp/godot-ddgi-step3-20260907`.
`final-handoff.json` identifies the frozen commit, artifact hashes and final
receipts; `staged-provenance.json` maps the 31 committed files to build inputs.
The following are recorded results, pending independent proof audit:

| Evidence | Recorded result |
|---|---|
| `editor-final-3`, `double-final-2`, `template-final-2` | Exit 0; stable before/after source manifests and executable hashes |
| `material-final-3` on RTX 4090 Vulkan | Exit 0; source/VisualShader reload recreates pipeline/SBT; no ERROR lines |
| `gallery-final-1` on the same editor binary | Exit 0; positive creation record for 3 hit programs and 15 geometry records |
| `hit-shaders/ordinary-closure-1` and `double-closure-final` | Author reports 8/8 native stage compilation/validation passes; exact final-source coverage needs audit |

Early C++/link errors, reload duplicate-release errors, `editor-final-1`
access-denied failure and stale `material-final-1` run remain archived.
`template-final-1` compiled but its sources changed during the build; it is
intermediate evidence. None replaces the final receipts above.

Native stage diagnostics use simple callbacks. Actual generated-material Vulkan
pipeline creation is recorded, but `trace_material_rays` awaits consumers in
Steps 4/6: native hit payload readback and DDGI/PT image correctness are unproven.
Arbitrary texture-coordinate expressions retain a basis approximation; explicit
Grad/Lod operands are preserved according to the author. Detailed skin/TBN,
array/default packing and decal-hit output lack separate runtime readback.

## Remaining boundaries

Compiler diagnostics and pipeline creation do not establish dispatched images,
moving-origin history, translated scenes near 1e8, the MultiMesh transition on
GPU, lifecycle execution, template/export or final DDGI/PT/RR behavior. These
remain obligations in Step 8. Native writes outside every RD command remain
outside the observable resource-generation boundary; no per-frame fallback exists.

Initial machine inspection: RTX 4090, driver 616.64, 24,564 MiB VRAM. No driver
or NVIDIA App changes were made. Official DLSS delivery findings and the SR/RR
model distinction are in the integration research; exact shipping model/runtime
selection must still be verified during the later integration.

## Worktree preservation

Work remains on the current branch. Pre-existing changes in the demonstration
project, project-state/direction documents, older research and untracked game
assets remain separate. Each source writer stages only owned files; source
staging was empty after both completed steps. No worktree, stash, discard/reset,
generated cache or unrelated content staging is authorized.
