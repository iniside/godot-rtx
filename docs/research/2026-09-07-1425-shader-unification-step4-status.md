# Shader unification STEP4: NRD/HDR and dormant PT removal

2026-09-07 14:25 UTC. Task baseline:
`38884029bf5654e0974ee6360e464185ea9b5c5c`. Authority: approved
[shader unification plan](../plans/2026-09-07-1126-shader-unification-plan.md),
STEP4. Executing agent: core-implementer, `gpt-6-astra`, high effort, selected for
material/GPU lifetime and cross-system shader work. Comments: default NONE.

## Changed authority and minimal closure

`servers/rendering/renderer_rd/shaders/effects/rtxdi_frame.slang` replaces the GLSL
frame effect. It consumes the shared `SurfaceData` decode and STEP3's final thin
NRD include. Normal/roughness preparation calls pinned
`NRD_FrontEnd_PackNormalAndRoughness`; material factors call `NRD_MaterialFactors`
through the existing shared wrapper. Pinned NRD normal encoding 2 and linear
roughness encoding 1 agree with the existing host library check. The NRD library's
embedded internal SPIR-V, algorithms, resource pools and dispatch sequence are
unchanged.

The native frame wrapper owns its stage IO, motion/sky reprojection and fixed-fog
composition. It retains the existing directional-light uniform layout for the fog
boundary, separate from the DI light registry. The retained GLSL fog consumers
remain outside the migration; they were not replaced or redirected. Native
row-major SceneData storage uses explicit vector/matrix multiplication matching
the existing CPU packing and STEP3 camera convention, including double-precision
inverse-view translation reconstruction.

`effects/nrd_effect.cpp:157,324` changes frame descriptors to separate textures and
the environment sampler, sets the complete native ShaderRD request, and guards
against a missing frame shader/pipeline. Formats, 48-byte push constants, context
ownership, resize/destruction, history reset, NRD inputs and composition ordering
are retained. Surface guides still precede PRE_OPAQUE; radiance demodulation and
remodulation occur once through the shared factors, incident light receives exposure
once, and emission, sky/fog energy and separate specular retain their prior roles.

`forward_clustered/render_raytracing.cpp:1625` now reads material metadata from the
existing spatial ShaderData, selecting RTClassification only when present. It
retains uniform offsets, global indices, texture hints/color selection and the
bindless-index upload tail without compiling shader source. The first hint-alpha
texture is still mirrored to the material alpha texture index; duplicate hint-alpha
diagnostics are retained. Array/global storage extents are derived from the stored
declarations so appended indices do not overlap those uniform spans. This does not
add arbitrary custom-hit evaluation or change the existing custom-material support
and diagnostic rules.

The existing RT material cache additionally compares both source-hash halves. This
replaces the old hit-group re-registration that detected source reload independently
of the material parameter counter. ShaderData version/source validity guards avoid
using retained texture metadata after failed or empty compilation. Successful uploads
publish the hashes; failed dedicated-buffer allocation leaves owned resources in
place and permits retry. Existing pool/dedicated-buffer ownership remains here, with
explicit release when a material no longer has an uploaded layout. RD retains its
deferred GPU release and command ordering contracts.

`RenderRaytracing::build_tlas:2529` retains mesh/CLAS, deformation, MultiMesh,
procedural AABB data, masks, culling and registry/snapshot ownership, removing
pipeline-bundle and hit-group readiness gates. Existing RT pass classification
continues to exclude alpha-overlay surfaces. Unsupported surface diagnostics are
not converted into a blanket TLAS exclusion. Procedural AABBs require current spatial
material data; compute queries still handle only triangle candidates, with no new
intersection execution.

Removed `SceneShaderRaytracing` completely: singleton/init/destruction, duplicate
material frontend, source slots, workers, bundles, cache/getters, pipeline/SBT
creation and free paths, plus the raygen/hit template and its eight exclusively
consumed GLSL includes. Removed all per-material/per-frame SBT offsets. The retained
shared RD TLAS contract requires a nonzero encoded range for a valid BLAS:
`render_raytracing.cpp:2088` supplies count one and offset zero (`1ULL << 32`), with
no SBT allocation. RD/backend/public API signatures are unchanged.

Direct closure includes `render_forward_clustered.cpp:1963` and its header's obsolete
friend/declaration. Wildcard C++/shader registration follows deleted files; the
effects SCsub's obsolete explicit GLSL dependency is removed. The existing RD_SLANG
builder and recursive include scanner embed the native frame/SDK closure. No
generated or third-party file was hand-edited.

## Evidence and validation boundary

[Provenance](shader-unification-evidence/step4-provenance.json) records commands,
task/build revision, owned source and deletion inventory, generated-header/compiler/
binary hashes, relevant demo input hashes and retained artifacts. The binary version
banner identifies parent documentation commit `b3e19ba8e9`; source hashes identify
the actual uncommitted implementation compiled before the scoped task commit.

The full ordinary Windows editor build command was
`scons platform=windows target=editor accesskit=no d3d12=no -j16`. Initial build/link
passed in 32.63 seconds; final rebuild after invalid/empty metadata guards and removal
of an unnecessary procedural alpha filter passed in 29.23 seconds. The
[retained build output](shader-unification-evidence/step4-editor-build-output.txt)
includes the final command result. MSVC reported `CowData` C4724 warnings; these
were not presented as errors or independently diagnosed.

The final ordinary editor runs `demos/rtxdi_manual/main.tscn` on real Forward+ Vulkan
with the RTX 4090, using the existing manual capture controls (`--still --delay=8`).
The [gallery](shader-unification-evidence/step4-gallery.png) and
[runtime output](shader-unification-evidence/step4-gallery-output.txt) record actual
surface, DI and NRD/HDR dispatch together, not merely shader/pipeline creation.
The run exits zero with no ERROR report. Visual inspection sees the expected
geometry arrangement, checker, textured emission, materials, floor/wall shadows
and sky. A parent comparison against the retained baseline confirms those features;
it does not establish pixel identity or performance improvement.

Runtime output retains unsupported-material warnings and SPIR-V parser messages
for `OpDemoteToHelperInvocation`, `OpTypeForwardPointer` and
`OpTypeAccelerationStructureKHR`. Their presence is disclosed without claiming a
validation-layer-clean run or attributing them to a verified fallback mechanism.

This bounded STEP4 closure does not independently establish template/export/bake,
double precision execution, material/include hot reload, sampler/global/instance
uniform variations, camera cuts, resize, multi-viewport, separate render thread,
fog/FSR2 interaction or numerical energy equivalence. Those full axes remain STEP5.
The inherited custom upload does not gain array-value evaluation or custom procedural
coverage execution; current unsupported diagnostics remain. No automated tests,
assertions or new test harness were authored or run.

Navigation: clangd find/refs/def/hover against root compile_commands, followed by
actual source/declarations; targeted shader/SDK/SCsub reads; scoped history; bounded
text removal inventories as lower bounds, closed by the full build and real run.
One-shot header refs were often declaration-only. History identifies the removed
PT owner as fork-local (`135dff3887`); shared RD contracts remain intact. No ClassDB
binding/XML surface changed. Taxonomy classes 2–9 apply: resource lifetime, shader
and backend contracts, fork replacement, build graph, proof boundary, C++/thread
safety, shared-tree ownership and approved scope. D3D12/Metal RT, DDGI, reflections
and transmission remain outside the approved implementation. Fresh exact-commit
source review and proof audit remain for the parent.
