# Stagger directional shadow updates and simplify shadow geometry

## Authorized scope

Owner approval on 2026-09-11: implement near/far cascade update priorities,
including reduced geometry detail for shadows. Work starts at
`3bcab4a12bf4c2a70e4829f5a133906218ea2afd`. Retain primary T Mega Geometry,
RTXDI/DDGI, current camera, and the temporary 1 GiB page pool. Streaming repair,
full PT, foliage, voxels, XR/VR, multiview, split screen and scene-architecture
changes are excluded. No automated tests or proof auditors; use a short review,
ordinary build and native GPU measurements. No computer-use automation.

The owner already authorized this shape; implementation does not need renewed
permission. Reduced shadow detail is authorized, but camera/RT detail is not
changed by this task. Scheduling trades bounded shadow age for reduced work.

## Evidence and existing owners

Research at the task-start SHA used clangd, direct declarations/implementations,
targeted shader reads and scoped history. Existing APIs remain authoritative:

- `renderer_scene_cull.cpp:2569-2801` calculates directional cascade projections;
  `:3698-3711` gathers conventional casters; `:4013-4029` publishes transforms
  and admits render records. Schedule before gathering and publication.
- `InstanceLightData` in `renderer_scene_cull.h:762` owns per-light lifetime;
  `Cull::Shadow::Cascade` at `:1199` carries per-cascade cull work.
- `render_forward_clustered.cpp:3557,1852,1877,607` prepares microgeometry only
  for admitted shadow records. Omitting a record skips DAG preparation and draw.
- `render_forward_clustered.cpp:2369-2373` clears the entire directional atlas.
  Existing shadow-pass rectangle clearing (`:3582-3583,3702`) can instead clear
  only refreshed regions; near cascade remains refreshed every frame so slot
  assignment (`:3316-3354`) remains present for each light.
- `LightStorage` owns atlas allocation and destruction (`light_storage.cpp:
  2849-2877`) and retained shadow transforms (`:654-670`). Consumer matrices
  rebase the original light-space transform to current camera (`:877-898`).
- Fog samples the atlas (`volumetric_fog_process.glsl:413-433`). RTXDI/DDGI
  visibility traces rays; this task does not claim to cache those rays.

Extend these owners. No second shadow renderer, public setting, binding,
resource format, atlas allocation, or generic cache framework is necessary.
The scheduler and error multipliers are private renderer policy. Stable Godot
public APIs and compatibility remain unchanged.

## Step 1 -> implement the coherent scheduling and detail change [independent]

Owned source scope: `servers/rendering/renderer_scene_cull.{h,cpp}`,
`servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.{h,cpp}`,
`servers/rendering/renderer_rd/storage_rd/light_storage.{h,cpp}`, and the existing
microgeometry selection owner only if necessary for passing shadow error.

1. Replace unconditional directional cascade refresh with internal periods
   1/2/4/8, phases near always, second even, third mod4=1, fourth mod8=3.
   First use and invalidation override cadence. Maintain maximum age without
   an unbounded priority queue. One- and two-cascade modes use the same prefix.
2. Retain map plus all matching transform/sampling data on skipped frames.
   Check admission before publication; do not mark unrendered maps valid.
   Replace whole-atlas clear with clears of updated rectangles. Preserve
   positional-light shadows and other renderer behavior.
3. Invalidate on atlas generation (size and bit-depth), light slot/order/count,
   scenario/camera ownership, camera projection/layers, shadow parameters,
   receiver coverage leaving the cached volume, and large accumulated sun
   direction changes relative to last render. Light SIZE is tracked separately
   from the existing light version. Regular small sun/caster changes may wait
   for cadence, as requested. Never sample cached depth using a new projection.
4. Make coverage meaningful: old culling uses a camera-cropped caster region
   and extra wedge planes. Cached far cascades must populate their retained
   light-space coverage box (preserving depth/pancake semantics), then test
   current receiver split corners against that box with a conservative texel
   margin. Expand the actually populated box with a modest guard band before
   applying the inset check; an exact-fit box plus positive inset would force
   refresh even for a stationary camera. Bypass camera-tight caster planes only
   for cached far cascades.
   If a different equally bounded conservative solution is simpler in actual
   code, preserve that coverage contract and explain it in the handoff.
5. Reuse the existing shadow-specific screen-space geometry error authority.
   Apply simple increasing per-cascade tolerances, starting at 1/2/4/8 times
   the current base error, preserving shadow texel projection. Do not change
   camera selection, RT cuts, asset import, streaming or shared global error.
   Existing `MicroGeometrySelection::Parameters::error` is copied per pass in
   `_prepare_micro_geometry`; `_render_shadow_pass` knows light type and cascade
   index. Keep non-directional/particle-heightfield error at its original 1.
   Do not change the Parameters default: primary T also uses it for its clamp.
6. Add sampled per-cascade refreshed/reused/forced counters, age and error,
   sufficient to verify all cadence phases and explain invalidation. Retain
   existing DAG and shadow GPU timestamps. Avoid per-frame verbose spam.

Implementation is render-thread-owned; workers consume prepared immutable
refresh decisions. Reuse existing atlas RID/free and deferred GPU lifetime.
No new shader or SCons entry is expected; close any actual layout change on
both C++ and shader sides. No editor-only dependency in runtime code. RD
Forward+ is the changed renderer; compatibility/mobile scheduling must not be
silently altered. Vulkan is the runtime validation backend; other RD backend
behavior is checked at the shared API boundary, not claimed runtime-tested.

## Step 2 -> build, short review, measure and document

Implementer builds with `scons platform=windows target=editor accesskit=no
d3d12=no -j16`, commits owned source and returns exact SHA. Fresh read-only
brief review checks exact commit and cumulative task-start diff; no proof audit
or automated tests. Relevant failure classes: ownership, render synchronization,
upstream compatibility, build, CPU thread safety, scope and evidence.

Parent compares the same native 10000-instance scene, saved camera, T mode,
2689x1602 and 1 GiB pool, using existing file/log capture tools. Baseline
`shadow-schedule-before-profile` exits 0 without errors: 7.953 ms whole GPU,
1.578 ms DAG traverse, 0.177 ms emit. Preserved binary is
`bin/godot.shadow_schedule_before.exe`, SHA256
`890B8941DD8F52DD2DC4F1FF79E4626B3C8CE54E121B335A30BAC2E2F4EFE39E`.
Record convergence, per-cascade actual refresh rates, work reduction and GPU
time, and unprofiled FPS if practical. Validate moving-sun/camera invalidation
through a bounded native diagnostic launch if an existing file-driven route
is available; otherwise disclose the unverified boundary instead of claiming
it passed. Owner judges appearance. Restore temporary editor settings after
captures. Update the canonical project-state/status entry with actual results.
