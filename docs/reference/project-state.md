# Project State

Read this compact index before project research. Follow the linked source or
canonical decision document when detail matters; do not treat this page as an
implementation plan or runtime proof.

## Agent Workflow

Owner decision, 2026-09-07, baseline `e03dca8e0a`: select delegated models by
whole-task difficulty and uncertainty, with Sol as the Codex default, Luna for
simple work, and Astra for complex work. The canonical policy is
[Model Selection](../../.agents/shared/planning-dispatch.md#model-selection).
Roles and plans do not pin models. Verified by policy/config inspection only;
already-loaded role definitions may retain old pins, handled by the Codex adapter.

## Owner Direction

The target renderer is hybrid rasterization with ReSTIR DI and DDGI. It keeps
an automatically simplified cluster DAG, excludes manual object LODs, uses
real near-field foliage geometry without alpha-cutout silhouettes, and moves
distant foliage to a voxel representation. The decisions, reasons, and open
research questions live only in [rendering-direction.md](rendering-direction.md).

Evidence: owner decision recorded 2026-09-06. This describes direction, not
landed implementation.

## Source State

Verified 2026-09-06 at source baseline `ca35348c11660535467348349d7a257a44c61e9a`
by targeted reads of this checkout:

- Static CLAS population enters
  `RenderRaytracing::_populate_cluster_blas()` in
  [render_raytracing.cpp](../../servers/rendering/renderer_rd/forward_clustered/render_raytracing.cpp).
  The Vulkan backend records
  `vkCmdBuildClusterAccelerationStructureIndirectNV` in
  [rendering_device_driver_vulkan.cpp](../../drivers/vulkan/rendering_device_driver_vulkan.cpp).
- D3D12 ray-tracing and CLAS methods remain unsupported stubs in
  [rendering_device_driver_d3d12.cpp](../../drivers/d3d12/rendering_device_driver_d3d12.cpp).

Method limits: repository-root `compile_commands.json` was absent, so clangd
navigation was unavailable. Git history identified the listed renderer work as
fork-local.

Replacement evidence, 2026-09-06 at `ee7ae4e52888eba32b1e2b62c93cb64bab4aebaa`:
the selected PT subclass and its output/guide consumers are removed.
[RenderForwardClustered](../../servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp)
owns the retained RT geometry/material service. Forward+ selects it after
[Vulkan RT preflight](../../servers/rendering/renderer_rd/renderer_compositor_rd.cpp).
[RenderingServerDefault](../../servers/rendering/rendering_server_default.cpp)
propagates initialization failure and distinguishes partial teardown. Retained
geometry-cache settings now use `rendering/raytracing/*`. Source and editor
compilation establish these changes; review found missing bindless descriptor
capability preflight, fixed in `de61204e58` with fresh round 2 review PASS. Real-device
behavior is not yet verified. A root compile database is now available.

Bounded inventory evidence: on 2026-09-06 at revision
`6ec2368d7e705103397177b73aa3585d08df2c15`, case-insensitive
`rg -n -i 'rtxgi|ddgi|restir|\bnrc\b'` returned no matching identifiers under
`servers/rendering/renderer_rd/forward_clustered` and
`servers/rendering/renderer_rd/shaders/raytracing`. This is a textual lower
bound, not proof that the features are absent elsewhere or under other names.

## Known Gaps

Shader unification research, 2026-09-07 at `a89cd8a3b0`, recommends Slang direct
SPIR-V for the spatial/surface/DI/NRD-HDR core while retaining public gdshader and
distinct canvas/UI/effect consumers. The mapped scope includes material codegen,
ShaderRD cache/export identity, common shading, and removing old PT readiness
gates that still control live TLAS geometry. Selected CLAS/RTXDI/NRD/DDGI shader
compiler diagnostics passed with Slang; this is not full renderer/runtime proof.
See the [research](../research/2026-09-07-1112-shader-unification-summary.md).
The [implementation plan](../plans/2026-09-07-1126-shader-unification-plan.md)
received fresh hostile review PASS and owner approval on 2026-09-07. It landed
separately in `b2d04129cb`. Compiler integration passed source/proof review at
`836f8c7e77`; spatial Slang migration passed final source review and bounded proof
audits at `1a5faf5c60` on 2026-09-07. Editor/template compilation, native shader
diagnostics and ordinary Vulkan rendering are recorded; complete exported-project,
double-runtime and material/topology validation remain final-stage work. Shared
shading/DI replacement passed source review and bounded proof audit at
`38884029bf`: sixteen native variants create Vulkan compute pipelines, with no DI
dispatch claim. NRD/HDR and dormant PT removal are in progress. Evidence is in the
[migration status](../research/2026-09-07-1126-shader-unification-status.md).

Active work is tracked in the [RTXDI implementation status](../research/2026-09-06-1904-rtxdi-implementation-status.md).
Approval/plan commit: `e02ea8c87d`. Dependency integration landed in `89b35e1d1c`
with shader/host compile evidence and a fresh hostile review PASS. RT ownership
and API replacement landed in `ee7ae4e528`, with editor compile evidence and fresh
review PASS after descriptor-indexing fix `de61204e58`. Scene/light registry
replacement landed in `afbe198fef`, with scoped clangd/diff checks. Fresh review
required four shader/analytic-light fixes, landed in `1c31998c6f` with shader
compile diagnostics and fresh round 2 review PASS. Surface/history replacement
landed in `c52c951925` with C++/ABI/structural checks. Review requires previous
depth, camera-cut and material-provenance fixes; `4f340213e8` implements them with
clangd checks. Round 2 rejected missing GLES3/dummy overrides for the shared camera
RID signature. The owner authorized the remaining correction; `826dc09a5a` fixes
the three override sites, with MSVC syntax checks passed for GLES3 and dummy and
fresh bounded review PASS at `6e539007f8`. The reported override defect is closed.
Step 5 DI landed in `5f9177d425`, with four GLSL variants and the Windows editor
compiled/linked. Fresh review returned REJECT for triangle geometric normal,
material coverage sampler and temporal jitter defects; exact anchors and proposed
corrections are in the status document. On 2026-09-07 the owner resumed work with
"popraw i kontynuuj": corrective commit `5a532d08b3` passed four DI and four
surface shader compile diagnostics plus the editor build. Fresh final round 2
closed the original findings but rejected a remaining shadow-facing defect:
ordinary single-sided casters act double-sided. The owner subsequently authorized
"no to popraw te cienie"; bounded correction `29a810ad37` has shader and owned
C++ object compile evidence and fresh bounded review PASS on 2026-09-07.
The reported shadow defect is closed. NRD/HDR landed in `a661887676` with final
editor/console compilation and 40 DI/frame/fog shader variant diagnostics passed;
fresh Stage 6 review requires sky-light exposure and PRE_OPAQUE guide-ordering
corrections. They landed in `217396c25c` with editor and template builds passed;
fresh final round 2 review returned PASS on 2026-09-07. Stage 7 landed in
`c8ba1736e1` with editor/template builds and real RTX 4090 Vulkan manual rendering
evidence: corrected culling, mixed energy, raster/CLAS geometry, motion history,
resource/thread teardown and material diagnostics. See the
[Stage 7 evidence](../research/2026-09-07-0848-rtxdi-stage7-rendering-status.md).
Fresh Stage 7 review resumed on 2026-09-07 at frozen `c8ba1736e1` and cumulative
`217396c25c..c8ba1736e1`. Round 1 rejected the directional-light registry inheriting
the raster/fog limit of eight. Correction `73616056ee` has editor-build and real
Vulkan nine/sixteen-light energy evidence; fresh final round 2 returned PASS
at `73616056ee`, and its bounded proof audit returned PASS on the ordinary-editor
Vulkan path. Fixed-template and separate-thread axes were not rerun for the fix.
See the [fix evidence](../research/2026-09-07-0959-rtxdi-directional-registry-fix.md).
The independent proof audit returned PASS for the retained source/binary hashes,
Vulkan logs, images and manual scenarios within the documented limits.

The owner resumed migration of `gi_demo/test.tscn` into `demos/rtxdi_manual`
from `b01c5849a8` and reported empty editor scenes. Migration `b32e62a7a3` replaces
runtime-only construction with serialized, editable hierarchies; `edffcf48db`
restores the private deformer mesh release case. All seven scenes load and
instantiate. After the owner re-enabled driver cache, the current ordinary editor
rendered the migrated map and gallery on Vulkan without environment overrides.
The current editor shows the authored scene tree and geometry; F5/Delete/F12/Escape
were exercised. Final evidence and SVG import settings landed in `6e03ebe1be`.
No cache or engine workaround was added. Fresh source review returned PASS at
`edffcf48db`, also examining the final evidence/import follow-on. The separate
proof-auditor spawn is blocked by the harness agent limit. See the [migration status](../research/2026-09-07-1034-rtxdi-demo-migration-status.md).

The [approved RTXDI replacement plan](../plans/2026-09-06-1854-rtxdi-renderer-plan.md)
received a fresh hostile plan-review PASS and owner approval on 2026-09-06 at
`33b555c7`. The owner explicitly
allows broken intermediate builds/rendering; the final stage requires real
Vulkan rendering. Implementation is authorized and has started.

The [concrete RTXDI design](../research/2026-09-06-1840-rtxdi-renderer-design-summary.md)
records the 2026-09-06 RT-only/no-compatibility owner override, at Godot `33b555c7`.
It maps surface export, RT ownership, shader includes, reservoir rotation and
NRD 4.17.1 integration. Fog still consumes shadow maps. This is source-backed
design research, not a reviewed execution plan or runtime proof.

The [RTXDI integration research](../research/2026-09-06-1823-rtxdi-integration-summary.md)
was recorded on 2026-09-06 at Godot `33b555c7`, SDK/sample `a6efab96` and
runtime `f12037fa`. It covers shared direct lighting, GLSL integration,
sample boundaries and history/resource costs. RTXDI-first ordering is recorded
in the owner direction document. No compilation or runtime evidence.

The [id Tech 8 research](../research/2026-09-06-1815-idtech8-rendering-summary.md)
was recorded on 2026-09-06 at project `33b555c7`, using primary SIGGRAPH/GPC
2025 materials and an NVIDIA interview. It separates baseline hybrid GI from
optional PT and records implications for DDGI/cache budgeting. External
architecture evidence only; no local performance or new owner decision.

RTXGI-DDGI integration was researched on 2026-09-06 at Godot `33b555c7` and
NVIDIA SDK `f33e496c` (1.3.6), using pinned SDK/sample source and targeted local
reads. See the [integration report](../research/2026-09-06-1748-rtxgi-ddgi-integration-summary.md)
for PT-only scene activation, probe culling coverage, RD/HLSL integration options,
sample reuse boundaries, and open design questions. No build or runtime proof.

No runtime performance was verified. The active build, enabled features, and
hardware behavior are unknown. The older
[Mega Geometry research](../research/2026-09-02-1904-megageometry-camera-relative-research.md)
predates landed CLAS work and is historical evidence only.
