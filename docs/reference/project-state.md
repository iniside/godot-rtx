# Project State

Entity scene model research (2026-09-09, source baseline `999bbd18c`): owner
selects Flecs for the complete 3D world scene in editor/runtime, without
per-entity Node/Object wrappers, before world streaming. Dense populations
should be flat; editor/streaming grouping does not imply transform parenting.
2D scenes are unsupported; game UI will use a separate HTML/CSS layer. The
[research](../research/2026-09-09-2023-flecs-scene-model-replacement-research.md)
records pinned Flecs contracts and Godot runtime/editor replacement boundaries.
Research only: no implementation, builds or performance claims. Upstream
compatibility and gameplay scripting contracts remain open before planning.


GPU-driven microgeometry implementation (2026-09-09, `bf2818e0ef`):
[authorized plan](../plans/2026-09-09-0900-gpu-microgeometry-plan.md) and
[execution evidence](../research/2026-09-09-0900-gpu-microgeometry-implementation-status.md).
Importer, paged storage, RD/GPU AS APIs and GPU selection/native raster passed
final source reviews; raster pinned compiler proof passed at `ec399801bf`.
Selected RT integration and simple error/offscreen controls are committed;
ordinary editor/console build and final compiler proof pass at `40650bd3bc`.
The owner renewed the two remaining corrections after the review-limit stop;
`bf2818e0ef` repairs the empty-to-nonempty BLAS build destination and signed
far-plane classification. Fresh source and bounded compiler-proof reviews pass;
ordinary editor/console build 08 and four refreshed shader variants pass.
Step 6 debug views and the visual scene in `demos/rtxdi_manual/microgeometry`
are checkpointed at `cab0af3d04` with Unicode correction `45f5eae299`;
their final review remains pending. The owner
stopped correctness/appearance verification and prioritized CPU/GPU performance.
[Initial measurements and active instrumentation](../research/2026-09-09-1456-microgeometry-performance-status.md)
show 38.210 ms GPU with live selection and 27.066 ms when frozen. Instrumented
Vulkan runs isolate GPU cost in RT cut preparation/publication and transform
updates, while native TLAS construction is small. Moving-scene RTXDI CPU
intervals are about 19 ms each versus 0.7-0.9 ms still; main profiling reports
render threading disabled. Two earlier moving profiles exhausted timestamp
capacity and are invalid; replacement profiles exit 0 without errors. The owner
requires no renderer execution on main, worker-based preparation/recording and
render thread as the synchronization/submission point. The
[approved corrective plan](../plans/2026-09-09-1535-rendering-performance-repair-plan.md)
orders CPU/GPU redundant-work removal before frame ownership and worker dispatch;
GPU async compute is a separate final extension. The owner approved execution;
the plan is committed at `02e21acb92`. Step 1 CPU dependency optimization
`2b2d8476f7` passes source review and the double build; moving dependency-union
CPU cost falls from 114.8 to 2.8 ms at approximately matched graph-usage counts.
Earlier overall GPU/frame comparisons lack controlled workload conditions.
Bounded native unload/reload, two-view, freeze and resize validation exits 0;
Step 1 artifact-audit documentation corrections pass fresh round 2.
Step 2 parallel GPU preparation is committed at `984dccbd32`; ordinary/double
builds and fresh source review pass. Single-view Vulkan captures after Steps 1-2
report GPU medians 2.96 ms still and 4.91 ms moving, versus 38.82/39.44 ms with
the retained pre-repair shader binary; see timing semantics and receipt limits
in the measurement document. Step 3 `0d1ed0abe9` passes source review and both
builds; single-view still capture stops repeated RT preparation and dependency
union builds. Moving capture is not an incremental performance-win claim.
The owner additionally requires real instancing/streaming for 5000 Lucy plus
5000 Thai instances. Current full-DAG-per-instance buffers cannot admit that
workload; the sparse selection/shared-cut extension passes plan review before
the threading steps. Dense fixture `c4633542e7` passes source/proof reviews,
normal import and headless population (5000+5000 nodes sharing two meshes).
SceneTree scalability fixes remain outside this task. Step 3A sparse selection
correction `571816b9f4` passes final source review, both builds and single-view
still/moving Vulkan captures. Step 3B shared RT cuts/BLAS and exact streaming
ownership pass final source and bounded proof reviews at `cfa892579a`. Native
Vulkan admits all 10000 instances with two shared assets and 2-3 cuts/BLAS;
corrected profiled shutdown exits 0 without errors. Native unload/reload releases
and restores resident pages and AS allocations; interrupted sessions are not
normal-exit proof. Gather-local material reuse `e319cca724` builds in both
precisions and lowers measured RT gather CPU from 50.269 to 6.809 ms on the dense
scene; fresh source review passes. These are reported window medians, not
main active time or isolated whole-GPU gains. Step 4 bounded main/render frame
ownership passes final source review and ordinary/double/template builds at
`9f42b239e3`. Final native editor, dense, SR/RR, bare-editor and no-draw runs
exit 0 without ERROR. Dense main transfer is 0.0225 ms and admission/callback
active work 0.0115 ms; admission wait is separately 27.1335 ms. These are
medians of reported window means. Fresh independent proof audit passes on
2026-09-10, with double/template compile-only and no final appearance claim.
Step 5 correction `9f6512180c` passes ordinary/double/template builds and
bounded dense/gallery Vulkan runs. Batch-local resource reuse reduces repeated
mesh-owner locking; conventional light/decal host preparation runs on workers.
Quiet matched dense FPS medians are 41 for Step 4 versus 37 for the correction;
the earlier Step 5 candidate reported 29. Final profiled RT gather is 5.3382 ms;
main transfer 0.020 ms, admission/callback active 0.0105 ms, wait 30.024 ms.
An intervening 25-FPS baseline run is confounded by external compiler load.
Final source round 2 REJECT finds synchronous RT decal snapshot preparation at
`render_raytracing.cpp:4502` / `texture_storage.cpp:4314`. Move this CPU work to
a joined worker before publication/uploads; it is separate from the corrected
conventional decal path and is not blamed for the decal-free dense regression.
The owner explicitly renews the closure on 2026-09-10 and requests finer
instrumentation. Measured correction `617a3abca7` eliminates repeated motion
traversal and empty canvas jobs, and batches dependent phases. Final correction
`030fa486b5` retains decal CPU preparation on a worker while resolving guarded
texture generations on the owner, also covering analytic projector/area inputs.
Ordinary/double/template builds and current dense/moving/gallery Vulkan runs
pass. Final renewed source round 2 and named refreshed proof audit PASS close
Step 5 CPU preparation. Nonempty textured-decal/projector runtime is unverified.
Historical local owner elapsed medians are 389.5 versus 90 us for the RT registry
and 380 versus 212.5 us for canvas batching, but partial telemetry/compiler
overlap prevents an isolated timing claim. Structural work/job reductions are
verified. Dense no-profile medians of control 34, candidate 38 and repeat 34 FPS
do not establish a durable whole-frame gain.
Dense HUD `1f339891f9` displays wall FPS/frame, CPU rendering including waits,
and completed GPU time; native PNG inspection and the independent HUD-specific
proof audit pass with the recorded single-frame and diagnostic-log limits.
Step 6 worker frontend/driver recording is delegated from `617a3abca7`.
Held compile06 source passes ordinary build and native dense600 with normal
shutdown after earlier crash/timeout corrections. Sampled frontend and driver
recording overlap on distinct workers. Step 6 is committed at `caa122b651`; ordinary/double/template builds pass,
while final source/proof acceptance is pending. Counter commit `bbf18999d0` passes source
review: native samples isolate 3.466 ms full-record hashing inside 5.853 ms RT
preparation and 4.222 ms raster preparation. These four sampled-frame medians
are diagnostic observations, not a matched whole-frame gain. See the
[preparation cost map](../research/2026-09-10-0706-microgeometry-prepare-cost-summary.md).
Conditional async compute has not started. On 2026-09-10 the owner requests a
replacement repair plan: remove recurring CPU microgeometry list/hash/task
reconstruction using existing persistent GPU data, then fan out all ready
independent CPU jobs with waits at consumers. No proof/auditor workflow; at most
a short code review, and acceptance is compilation, working game and actual
same-settings speedup. Latest diagnostic fixes `8c8833b87f` and limited RT hash
reuse `2108bbedf1` pass a combined ordinary build but remain runtime/performance
measured in one ordinary no-profile dense run: last-ten FPS median 43 versus
38 in the preceding matched-route run (single pair, no durable gain claim).
The owner approved the [replacement plan](../plans/2026-09-10-0807-gpu-microgeometry-cpu-removal-plan.md),
committed at `8e316e180d`. Step1 `dc7b4b2320` retains changed surface/task metadata
and removes per-task snapshots. One matched dense no-profile pair reports
43.20 ->47.85FPS and23.147 ->20.900ms wall frame; CPU samples21.576 ->19.194ms.
Both runs and moving/freeze exit0; final GI-pairing invalidation is compile-only.
Step2 `671aa6cfc4` retains raster/shadow inputs and removes eligible meshes from
the conventional CPU culling domain. GPU receives actual caster planes and
rejects invisible DAG work early; selector capacity maintenance is change-only.
Ordinary build and dense/moving native runs pass. Same-route no-profile wall
time24.343 ->22.227ms, CPU samples22.443 ->19.894ms in one comparison;
GPU samples6.351 ->6.816ms. Intermediate shadow/admission regression was fixed
before commit. Step3 `f60eafa5275` replaces recurring microgeometry RT gathering
and hashing with persistent membership and GPU-written geometry/material/motion/
TLAS records. Ordinary build and dense/moving native runs pass. Same-route
no-profile pair reports22.552 ->8.735ms wall,44.34 ->114.48FPS,
20.298 ->6.303ms CPU and6.607 ->4.352ms GPU samples;10000instances,8pages,
108CLAS remain. The owner observed approximately120FPS. Independent-job fan-out
is the remaining active implementation step; no final performance claim yet.
Actual dragon import/reimport passed in 82.66/67.04 seconds, yielding 17 DAG
levels and 9737 pages with byte-identical reused `.mgdata`; the diagnostic
reimport command had shutdown warnings. Import evidence uses an immutable
intermediate binary, not final GPU runtime proof. Selected runtime checks and ordinary/double/template builds passed on dirty
Step 6 sources; emissive publication, PCK runtime and final review remain open. Owner scope:
nondeforming geometry including rigid movement/instancing; preserve existing
deformations; exclude foliage/voxels, new reflections, split screen and XR/VR
support. Further manual performance validation uses one game viewport. RT simplification uses
simple error control plus an offscreen multiplier, without light/receiver/probe
importance algorithms. Implementation remains authorized and incomplete.

Owner renderer scope correction (2026-09-08): DLSS/RR, path tracing and
camera-following DDGI only; no VR/XR work or validation and no requirement for
DDGI to match path tracing visually. See the amended
[approved plan](../plans/2026-09-07-1635-ddgi-pt-rr-plan.md).

Latest renderer integration (2026-09-08): camera-following DDGI, shared native
camera PT, selectable NRD/RR/raw composition and official DLSS 4.5 preset-M
delivery are implemented. Actual GPU/build/export observations and unverified
cases are in [the runtime status](../research/2026-09-08-ddgi-pt-rr-runtime-status.md).
Earlier milestone limitations below describe their recorded snapshots.
Final executable source `968d4e9e8e` passes fresh source review. Editor,
double editor and template builds succeed; exported SR/RR render and exit 0.
Material/VisualShader/texture reload works in hybrid and PT, and saved settings
survive editor restart. The [closure status](../research/2026-09-08-0935-ddgi-pt-rr-closure-status.md)
records independent proof, exact artifacts and the original shutdown crash's
causal-evidence limit. Earlier status paragraphs below are historical snapshots.

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

Shader unification, completed 2026-09-07 through `c148e8fbb9`: the spatial/surface,
DI and NRD/HDR core uses pinned Slang directly to SPIR-V, preserving public
gdshader/VisualShader and distinct GLSL canvas/UI/effect consumers. Shared shading,
material metadata, geometry and light semantics replace the former adapters;
dormant PT compilation, bundles and TLAS readiness dependencies are removed.
The owner-approved [plan](../plans/2026-09-07-1126-shader-unification-plan.md)
landed separately in `b2d04129cb`. Steps 1–4 passed fresh source reviews and bounded
proof audits at `836f8c7e77`, `1a5faf5c60`, `38884029bf`, and `99c67e1f78`.
Final validation builds editor/template/double, renders material reload and
interactive topology on RTX 4090 Vulkan, and runs the exported baked package
outside VulkanSDK. Export uncovered the existing re-spirv caller-variable crash;
recorded patch `4c24eeefe0` passes source review, proof audit and actual export.
Final validation at `c148e8fbb9` passes fresh source review and independent proof
audit. Large-origin lighting near 1e8 still
exposes unchanged absolute-float light/AS precision limits; no broader coordinate
redesign or historical large-origin image equivalence is claimed. Exact provenance,
observations and remaining limits are in the
[migration status](../research/2026-09-07-1126-shader-unification-status.md) and
[final validation report](../research/2026-09-07-1518-shader-unification-step5-status.md).

Startup follow-up, 2026-09-07 at baseline `10b3db6873`: bare `-e` without a project
exposed the upstream project-manager OpenGL default, rejected by this Vulkan-only
fork. `Main::setup` now defaults that path to Vulkan/Forward+. Ordinary/double
editor builds passed; the rebuilt double runs `-e --verbose --quit-after 90` on
RTX 4090 Vulkan and exits 0 without the reported renderer/ObjectDB errors.
See the [bounded fix evidence](../research/2026-09-07-1558-project-manager-vulkan-fix.md).

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
