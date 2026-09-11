# Microgeometry HZB repair

## Context and scope

Owner directs HZB repair so occluded geometry is not submitted unnecessarily.
Task start: `f450782247`; renderer source `ab4e61b17a`. Previous native stress
result is43.5FPS with origin visible; matched origin-hidden profiling22.9ms GPU
and78M camera triangles. Retain1px, the tightened scale bound, all10000instances,
existing page/CLAS ownership and one viewport. No tests/audits, XR/VR/multiview,
split screen, foliage/voxels, import rebuild, quality reduction or new renderer
subsystem. Cluster HZB stays conservative; it cannot remove every invisible
triangle within a partially visible cluster or guarantee a9ms frame.

## Source findings and overlap

`prepare_rtxdi_surface` (render_forward_clustered.cpp:95) combines camera/depth
continuity with any-deformation invalidation. The origin shader writes POSITION
and is correctly classified RTXDI_MATERIAL_DEFORMED (bit64); classification is
already stored in surface attachment5. Original surface history must retain its
invalidation because other temporal consumers use it. Existing transparent
callbacks have no geometry draw; adding a color/overlay pass is unnecessary for
this repair. Instead filter unsafe material pixels only when copying depth into
the existing microgeometry pyramid, preserving visible colors and scene depth.

`micro_geometry_hzb.slang` uses correct reverse-Z minimum reduction. The host
pads mip0 to power-of-two dimensions and query UVs use the true source size;
these agree. `occluded` (micro_geometry_select.slang:282-301) queries coarse cells
spilling outside the projected rectangle and treats ceil(high) as inclusive.
Zero depth outside that rectangle can prevent a valid occlusion decision.
Existing recovery renders initially rejected clusters against current depth.

## Step1 — safe HZB history source [independent]

Owned files: render_forward_clustered.{h,cpp}, micro_geometry_selection.{h,cpp},
micro_geometry_hzb.slang. Extend build_depth_pyramid to accept current surface
classification attachment5 and bind it in the existing mip0 copy shader; pixels
with the existing DEFORMED bit become reverse-Z0. Subsequent reductions remain
min. Do not modify actual scene/surface depth, colors or classification output.
All existing pyramid call sites, uniform layouts and source lifetimes must agree.

Factor camera/depth continuity once in prepare_rtxdi_surface. Keep original
rtxdi_surface_history_valid semantics unchanged; derive dedicated microgeometry
history validity from the same continuity checks plus remaining unsafe-geometry
vetoes. Material-deformed pixels are filtered, so they no longer globally veto
HZB. Teleports, procedural instances and unsafe previous-vertex alias cases still
veto HZB. The existing validity scan must continue past material-only failures to
find those cases; preserve original first-reason diagnostics and worker join.
Do not equate all deformation with safe history. Reset new validity on teardown,
resize/discontinuity and use it only at the microgeometry selection gate. HZB
construction and two-pass recovery remain in their current order.

In this step reserve336bytes in the existing statistics buffer and decode eight
query fields per phase at offsets68 and76: refinement tests, extra rejections,
inside-zero failures, inside-nonzero failures, exhausted budgets, texture fetches,
maximum fetches and boundary descents. Existing Ref/epoch readback and sampled
cadence remain; allocation/accounting/reset share the size constant. This closes
the C++ contract before Step2; unused fields remain zero until that shader lands.

## Step2 — bounded rectangle occlusion [independent]

Owned file: micro_geometry_select.slang. Preserve near-plane/projection/task
fallbacks, conservative nearest depth, reverse-Z comparison and its depth bias.
Use integer floor(low)..ceil(high)-1 coverage with invalid/empty bounds visible.
Preserve the previous-camera outside-viewport fallback initially; current-frame
recovery may intersect its query with the viewport. Begin at a mip with at most
four intersecting cells. A passing cell proves its overlap hidden. A failing
cell entirely inside the rectangle returns visible; a failing boundary cell
descends only to intersecting children. Reject only if every covered cell passes.
Bound total fetches (initial32) and traversal control; exhaustion always returns
visible. Use small/implicit traversal state, avoiding a large per-thread stack.
Replace the oversized query at its authority, not a second runtime strategy.
Fill the sampled fields reserved in Step1 to separate genuine interior holes,
new rejection and budget cost. Do not turn zero depths into occluders.

## Step3 — measured handoff [inline]

Build the ordinary Vulkan editor after the source is frozen. Run the unchanged
native stress editor with origin visible, same saved camera, actual resolution
and1px target. Check HZB history enabled despite material-only origin deformation,
query/selection balances, recovery, retirement and allocation errors. Compare
emitted clusters/triangles, query GPU time, camera raster and whole GPU; repeat
without profiler against43.5FPS. Preserve visible origin and restore any temporary
editor sleep/VSync/update settings. No unsupported modes or automated tests.
Stationary performance is the owner-prioritized check; report any unperformed
movement/reveal/resize coverage rather than claiming it. One brief final source
review, no proof auditor or repeated review packages.

No ClassDB/XML/settings/serialized format or SCons additions are needed. These
are existing Vulkan/CLAS renderer shader paths, not new backend APIs. Keep RD
work on the render owner and existing uniform-cache/resource retirement paths.
Commit the plan before source implementation and each completed source step;
root records measured source/binary identity and updates project-state/status.
