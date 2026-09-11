# Native stress selection repair research

Source baseline `6bb550ae7f`, researched 2026-09-10 UTC from the owner-requested
live editor profile. No renderer edits, builds, tests or editor restarts.
Navigation: root-compiledb clangd symbol/reference queries, targeted C++ and
Slang implementation reads, and bounded existing-log analysis. One-shot clangd
references were incomplete across translation units; source call sites were
read directly. This is a repair recommendation, not a validated speedup.

## Confirmed selection admission problem

`MicroGeometrySelection::_resize` (`micro_geometry_selection.cpp:194`) compares
new allocation bytes plus existing `dynamic_memory_bytes` and fixed buffers
against `MAX_PASS_BYTES` (512 MiB). The logged 696064 queue slots and 2486464
record slots require approximately 269.68 MiB for new variable arrays, including
10000 units. The remaining approximately 271.02 MiB is old variable storage and
fixed overhead: the reported 540.70 MiB is a replacement peak, not the new
steady-state selection size. Keeping old buffers until GPU completion is needed.

On rejection, `_resize` sets `admission_failed`. `needs_retry` then permanently
rejects capacity retries for that pass; `select` also stops capacity readback,
but continues dispatching traversal/emission on old allocations. Slang modes
6 and 9 publish candidate cuts only when status bits 0-2 are clear. Overflow
therefore retains the previous committed cut; this path does not automatically
fall back to drawing the original full mesh. Initial creation failure instead
returns null. Runtime cut completeness after the observed resize failure has
not been verified.

Recommended first correction: account steady-state pass capacity separately
from temporary old-plus-new replacement residency. Retain checked per-buffer
sizes, allocation-failure cleanup, copy-before-publication and submission-based
retirement. Admit this approximately 270 MiB replacement under the existing
steady-state 512 MiB limit, while recording the actual peak. Temporary resource
pressure must not permanently poison a pass; defer retries to a meaningful
resource/capacity change, avoiding allocation attempts every frame. Do not
remove retirement or add a blocking GPU wait. Owned implementation surface is
`micro_geometry_selection.{h,cpp}`; no new public API is required.

## Raster cost is a separate unresolved cause

`RenderForwardClustered::_render_list_with_draw_list` labels all conventional
and microgeometry draws in that call as Raster Initial Draw. Its 41.246 ms
median does not identify one shader or prove that the admission error causes it.
Sparse traversal dispatches the allocated queue range for every DAG level, so
capacity and overflow matter, but fixing admission alone is not a speedup proof.

In the sampled log there are 116 HZB markers and zero recovery draw/select
markers. `_select_micro_geometry` enables HZB only when
`is_rtxdi_surface_history_valid()` succeeds and the pyramid exists. The history
predicate in `prepare_rtxdi_surface` checks consecutive engine frames, depth,
deformation, resolution, camera identity, previous transforms/projections and
jitter. The blocked predicate is not yet identified; do not relax it blindly.

Recommended next diagnostic within the existing profiler: report the failed
history predicate and HZB-enabled state, camera versus shadow pass identity,
selected clusters/triangles, capacity/overflow and replacement bytes. Then fix
the history producer only if evidence demonstrates incorrect invalidation.
Preserve correct resize, camera-cut and deformation invalidation.

After each correction, build the ordinary editor and measure this same native
scene at a fixed editor camera/resolution. First require admission errors to
disappear and cuts to converge, then compare raster/traversal GPU intervals and
whole-editor FPS. No automated tests, separate game benchmark, multiview,
XR/VR, foliage, voxels or worker-system redesign are in this recommendation.

Evidence logs: `%TEMP%/godot-stress-editor-20260911.log` and corresponding
`.stderr.log`. The owner may continue interacting with the open editor; this
capture is not a fixed-camera baseline.

## Planning follow-up

The owner requested a concrete fix plan. The
[draft sequence](../plans/2026-09-10-2217-native-stress-selection-hzb-plan.md)
starts with existing-profiler diagnostics, then corrects active-versus-transient
admission, then repairs history only after its rejected predicate is known.
It narrows retry behavior: retirement deferral remains retryable; true unchanged
hard-limit/allocation failures retain valid state and do not spin every frame.
No general OOM recovery subsystem is proposed.

The captured editor process is no longer running. Its late log also includes
240-284 ms simulation intervals and 2-5 FPS; that interactive interval is not
evidence that the earlier stationary GPU bottleneck moved to CPU. A fixed-view
measurement with the added diagnostic reasons is required for causal attribution.
Existing asynchronous raster statistics already provide cluster and triangle
counts (`micro_geometry_stats_received`, `render_forward_clustered.cpp:216`);
no new synchronous readback is needed for them.

Independent bounded source tracing found no missing native camera-history
producer: the editor Camera3D is persistent, document loading changes its scenario
and seeds the controller on reset, and the normal viewport/cull path fetches
previous CameraData, renders, then stores current data. Anchors:
`editor/scene/3d/node_3d_editor_viewport.cpp:6219`, document setup at 5470;
`renderer_scene_cull.cpp:4246`; `renderer_scene_render_rd.cpp:1414`.
This strengthens the diagnostics gate; it does not establish the live rejected
predicate or authorize forcing history valid.
