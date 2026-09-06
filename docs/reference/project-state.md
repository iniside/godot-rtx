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
Step 5 DI
code is preserved uncommitted, with four GLSL variants compiled but its host not
compiled or wired. The status document records the exact fix and resume point.
Steps 3–4 have no full renderer build or GPU
validation yet.
No RTXDI frame dispatch or visual proof exists yet.

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
