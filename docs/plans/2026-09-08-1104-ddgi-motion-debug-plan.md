# DDGI motion darkening: debug views and bounded correction

Status: owner-approved for implementation ("implementuj"). Date: 2026-09-08 UTC.
Baseline: `2f2941f31d11aeb371cbdc8c1234d63b22a150bc`, existing dirty tree.
Fresh independent plan review: PASS, classes 1–9. Its nonblocking wording
clarification is applied: native state/accessors are added to the existing
RenderSceneBuffers; they are not claimed to exist already. No implementation
or runtime reproduction is implied by this plan verdict.

## Requested outcome and scope

The owner identifies `demos/rtxdi_manual/test.tscn`: translation causes the
approached region to darken before indirect lighting returns; camera rotation
alone does not reproduce it, and stationary lighting is acceptable. The owner
requests a plan for a debug view or a repair. Make the actual probe movement,
validity and cascade contribution visible, then fix the demonstrated cause.

Preserve the world-aligned virtual grid, camera-following active coverage,
bounded storage, history in overlapping cells and progressively sparse distant
coverage. Spatial retention/hysteresis and lower distant work are desired design
directions; do not replace the renderer with a sparse-world streaming system to
investigate this defect. No VR/XR, DDGI/PT image parity, unrelated editor fixes,
automated tests, benchmark framework, new scene Node or authored DDGI volume.
The owner approved execution after presentation of the independently reviewed plan.

## Verified source and unresolved cause

- `DDGIEffect::prepare`, `ddgi_effect.cpp:226`: floor(camera / spacing), absolute
  int64 cells, ring slots and reset masks; 16 cubed probes at every level,
  doubled spacing per level, immediate edge recycling, whole-cascade scheduler.
  There is no spatial retention margin. Header hysteresis 0.97 blends lighting.
- `RenderRaytracing::_prepare_ddgi`, `render_raytracing.cpp:168`, passes camera
  position and RT origin separately. Settings/layout changes own DDGI epochs;
  ordinary camera translation does not itself increment the DDGI epoch.
- `ddgi_state.slang`: reset clears old light/validity; finish marks a probe valid
  only when traced and relocated offsets match. Validity does not express
  lighting convergence. Do not equate an active, valid probe with a mature one.
- `ddgi_query_inc.slang` computes support and edge confidence;
  `ddgi_grid_inc.slang::ddgi_query` already assigns remaining weight to farther
  cascades. Uncovered remainder contributes zero. Absence of fallback is not an
  established defect. Camera shading and recursive probe lighting share this query.
- `misc/scripts/patches/rtxgi_ddgi/0002-probe-validity-query.patch` owns the
  local SDK validity/support extension; change it through the existing vendor
  update procedure if necessary, never by hand-editing vendored/generated files.
- `ddgi_camera.slang` writes material-weighted linear indirect radiance to
  `Context::indirect_radiance`. Existing snapshot diagnostics expose GPU state,
  but are synchronous and cannot replace an interactive overlay during movement.

Cause is unconfirmed: scrolling/reset, relocation invalidation, insufficient
fresh coverage, confidence/fallback, reconstruction or downstream denoising are
distinguishable branches. Existing room captures do not resolve this map report.
During planning, the owner-edited scene now serializes `ddgi_updates_per_frame=4`
and `pathtracing_max_bounces=4`. Preserve this dirty scene and record actual
runtime settings when reproducing; do not assume the default one-update budget
caused the reported behavior or overwrite it to fit that hypothesis.

## Existing mechanisms to extend

Extend Viewport debug drawing and Forward Clustered's existing late debug pass
`RenderForwardClustered::_render_buffers_debug_draw` (`render_forward_clustered.cpp:2167`).
The existing GI_BUFFER view reads the other GI buffers, not DDGI indirect output.
Extend `DDGIEffect` and its per-render-buffer Context for DDGI diagnostics; reuse
volume descriptors, probe offsets, validity and update stamps. No CPU probe scene,
duplicated allocation/lighting authority, replacement server or persistent cache.
SDFGI's `GI::SDFGI::debug_probes` is an existing procedural GPU marker pattern;
reuse the approach, not its incompatible probe layout or a second GI resource.

## Step 1 -> GPU debug views and frozen anchor [independent]

Add four appended Viewport/RenderingServer debug modes, preserving existing enum
values: `DDGI_PROBES`, `DDGI_PROBE_STATE`, `DDGI_CASCADE_WEIGHTS`, `DDGI_INDIRECT`.
Expose them through the existing editor Debug Draw selector and Viewport API.

- Probes: actual GPU relocated positions, color by cascade, world-aligned bounds.
- Probe state: distinguish invalid/new, inactive and usable probes, and age from
  actual successful-update stamps; visibly identify newly recycled slots. Do not
  use a zero invalid stamp as an ordinary age value. Reuse reset masks only on
  the frame they apply; a stale retained mask must not imply repeated resets.
- Cascade weights: evaluate the same query authority and display its actual
  per-cascade contribution/confidence plus uncovered weight. Do not implement a
  second approximation of cascade selection just for visualization.
- Indirect: show the current material-weighted DDGI camera texture before
  composition/denoising, with known fixed display exposure/encoding. Label that
  meaning rather than describing it as raw probe irradiance or reflections.

Proposed control: `Viewport.ddgi_debug_freeze_anchor: bool = false`, with
`set_ddgi_debug_freeze_anchor(bool)` / `is_ddgi_debug_freeze_anchor()` and
RenderingServer forwarding `viewport_set_ddgi_debug_freeze_anchor(RID, bool)`.
This is diagnostic viewport state, not shared Environment lighting configuration.
Add an independent check item to the existing editor viewport menu, so selecting
another debug mode does not silently unfreeze the anchor.
Transport it through RendererViewport by adding native nonvirtual state/accessors
to the existing RenderSceneBuffers (`servers/rendering/storage/render_scene_buffers.h`), then read
it in `RenderRaytracing::_prepare_ddgi`. Do not add a new RendererSceneRender
virtual or dummy/GLES implementation solely to carry this flag.
Latch the current scrolling anchor when enabled; keep lighting updates, real
camera transforms, depth reconstruction and RT origin handling live. Unfreeze
resumes ordinary scrolling; context recreation latches the current anchor, not
a dead context's coordinates. Two ordinary viewports remain independent.

Files/contracts: `scene/main/viewport.{h,cpp}`, `servers/rendering_server.{h,cpp}`,
`servers/rendering/{rendering_server_enums.h,rendering_server_default.h}`,
`servers/rendering/renderer_viewport.{h,cpp}`, RD render-buffer state,
`servers/rendering/storage/render_scene_buffers.h`,
`forward_clustered/render_forward_clustered.{h,cpp}` and `render_raytracing.{h,cpp}`,
`effects/ddgi_effect.{h,cpp}`, DDGI shader query/camera/debug sources,
`editor/scene/3d/node_3d_editor_viewport.{h,cpp}` (display enum, parallel option
arrays, `_menu_option`, advanced menu), `doc/classes/{Viewport,RenderingServer}.xml`.
Wire enum bindings, method forwarding and property metadata together. Runtime
must not depend on editor types. Unsupported backend/mode or absent DDGI context
shows unavailable/empty diagnostic output without creating DDGI or reading stale RIDs.

Read-only diagnostic passes run after production reads/writes in defined RD
order, use current GPU state, and do not feed debug colors into lighting history.
Bind update stamps only to debug consumers; the normal query does not currently
bind them. Align projected markers and depth with the real camera, jitter and
internal/output resolutions when the late pass follows DLSS scaling.
Own optional diagnostic output/scratch RIDs in the existing render-buffer/DDGI
context; release on resize, context free and teardown. No synchronous GPU
readbacks or extra probe-ray tracing in the interactive view. Disabled views
must not dispatch diagnostic passes or allocate their optional output resources.
New Slang files use `shaders/effects/SCsub`'s existing RD_SLANG discovery; C++
stays in existing units. Verify binding layouts and normal/double coordinates.

Why first: this makes the competing causes observable before lighting changes.

## Step 2 -> Reproduce and classify on the reported map [independent]

Extend only the existing manual demo controls in
`demos/rtxdi_manual/migrated/test.gd` and its HUD/README as needed to select the
views and freeze anchor. Report actual runtime mode/denoiser/DDGI settings;
replace the existing hard-coded direct-light/NRD HUD description. Preserve
`project.godot` owner edits, scene settings and the existing movement controller.
Use text scene/resource edits, not Inspector automation.

On RTX 4090/Vulkan, record the same forward/return movement and a rotation-only
control, with moving versus frozen anchor. Pause the demo's moving emissive cube
for isolation, then restore it. Inspect normal output, isolated DDGI and weights.
Capture a short contiguous interval spanning darkening/recovery with camera
position, minimum cells, DDGI epoch, selected cascades and reset/update evidence.
Use existing bounded snapshots separately where exact GPU cells are needed;
do not use their timing as performance evidence. Check movement within one cell
versus crossing a boundary, and brief back-and-forth motion across that boundary.

Produce a concrete causal finding before Step 3. Frozen-anchor success is a
discriminator, not proof of which scrolling submechanism is wrong. If isolated
DDGI stays stable while final output fails, trace the existing composition/
denoiser consumer instead of rewriting probe allocation.

## Step 3 -> Repair the demonstrated mechanism [independent]

Make the smallest correction at the existing authority; remove the faulty path
in the same step. Candidate actions are conditional, not four mandatory features:

- Incorrect mapping/reset/epoch: preserve surviving absolute cells and their
  data; invalidate only genuinely entering or relocated probes.
- Boundary churn: add a bounded spatial retention margin to scrolling, distinct
  from lighting hysteresis; preserve coverage and define teleport/unfreeze behavior.
- Fresh-data starvation or premature contribution: prioritize exposed/needed
  work within the budget, preserve fair distant updates, and retain usable farther
  support until closer data is ready. Never attach old-cell history to a reused
  slot, fabricate validity, hide the issue with a global exposure boost, or
  normalize away the intentional outer-coverage fade.
- Reconstruction/denoiser issue: fix the demonstrated existing consumer contract.

Do not increase fixed probe counts or redesign sparse allocation without evidence.
Per-cascade allocation/ray-budget redesign for distant lighting is a separate
follow-up if needed; doubled spacing already reduces spatial density today.
If the cause requires a new subsystem/public contract beyond these diagnostics
and a bounded existing-authority fix, report that concrete scope change first.

## Step 4 -> Validate, review and handoff [independent]

Build Windows Vulkan editor, precision=double editor and template_debug after
source changes. Compile/validate affected Slang variants and transitive shared
query consumers: DDGI camera and probe RT stages, normal/double and existing sky
variants. No headless proxy for GPU behavior and no automated tests.

Repeat the recorded translation at the same settings, including cell reversal
and freeze/unfreeze. The approached surface must not lose usable indirect light
because coverage shifts; unaffected cells retain positions/history. Verify the
debug overlay agrees with GPU snapshots, is absent when disabled, and does not
alter lighting updates. Check two mono viewports, resize/free, translated double
coordinates and one PT/RR smoke run only for affected shared consumers.
Check the affected runtime debug API in a template build. No VR or PT parity gate.

Commit each completed above-threshold implementation step before fresh source
review of the exact commit and cumulative baseline. Executable demo/proof changes
also receive independent proof audit. Route classes 1–9 by changed surfaces;
review the proposed plan before presentation. Update `ddgi-diagnostics.md`, a dated
result record and the linked project-state entry with actual results and limits.
