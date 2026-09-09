# Canvas preparation ownership

Source inspection, 2026-09-09 UTC, baseline `c36c4a3e86`. This closes the
canvas-specific input to Step 5 of the approved rendering performance repair
plan. It is source evidence, not implemented worker dispatch or runtime proof.
Concurrent Step 4 changes to frame interpolation inputs are excluded.

`RendererCanvasCull::_render_canvas_item_tree()` at
`servers/rendering/renderer_canvas_cull.cpp:71` combines culling and the renderer
call. Its camera transform and z-list arrays are singleton scratch. The loop
over canvas roots can supply independent subtree work only after that scratch
and per-pass results have separate owners; concatenation must preserve root
order within each z bucket. A y-sorted subtree remains an ordering domain.

`_cull_canvas_item()` at line 304 mutates child ordering, material ownership,
interpolation results, repeat state and clipping. `_attach_canvas_item_for_draw()`
at line 190 writes linked draw lists and the shared visibility-notifier list.
It also clears/allocates commands for an empty fitted CanvasGroup. Moving this
function unchanged into parallel jobs is therefore insufficient. Deferred
visibility/redraw effects and fitted-group output need explicit ownership;
main-only notifications must retain their existing delivery contract.

`RendererCanvasRender::Item::get_rect()` in `renderer_canvas_render.cpp:37`
looks const but updates cached rectangles and consults mesh, MultiMesh and
particle storage. It cannot be assumed to be a pure immutable-input operation.
Resolve dirty bounds before parallel readers or give their production an
exclusive job owner after checking those storage paths.

`RendererCanvasRenderRD::canvas_render_items()` in
`renderer_rd/renderer_canvas_render_rd.cpp:509` mixes light payload preparation,
GPU buffer updates, skeleton mutations and an ordered item scan. Screen-texture
copies, CanvasGroups and mip generation split the scan into dependent segments.
These boundaries must survive job scheduling; they are not independent passes
merely because they contain separate item ranges.

`_render_batch_items()` at line 2187 has a distinct CPU batch-building region
followed by RD recording. `_record_item_commands()` at line 2344 computes
instance payloads but writes singleton batch/texture state and particle SDF
settings. `_new_batch()` at line 3247 and `_add_to_batch()` at line 3291 can
allocate/map/flush UMA buffers. CPU payload work needs task-owned host storage
before the existing upload owner consumes it. Moving these helpers unchanged
to workers would retain shared writes and unrestricted RD calls.

Texture preparation at line 3314 consults `canvas_texture_get_info()`; its
storage contract still needs closure before claiming worker safety. Actual
recording at `_render_batch()` line 3008 resolves uniform sets, pipelines,
particle processing and mesh bindings. Those effects belong to the Step 6
recording/resource ownership boundary, not an unchecked worker call.

History `52f27370ab` documents a real shader-mutex/WorkerThreadPool dependency
deadlock in material updates. Preserve its compile-before-lock ordering. A
global canvas lock held across worker completion would not establish safe
parallel preparation.

Methods: clang-nav `find` against the root compilation database, direct current
declaration/implementation reads, scoped Git history, then a bounded helper-call
text sweep. No source edits, builds, tests or launches were performed for this
inspection. No new API or setting is proposed. Single-game-viewport scope applies;
XR, VR, multiview and split-screen work remain excluded.
