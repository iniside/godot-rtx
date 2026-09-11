# Microgeometry HZB repair status

Current source: `fa50c0398f` (history/filtering) and `19f97459f7` (rectangle query
and numerical margin). HZB now runs with visible axes, but the requested frame
budget is **not achieved**. Final stationary submission is67.36M camera triangles
versus79.12M before. The final unprofiled pair is40FPS before /45FPS after;
earlier unprofiled results43.5/42 and profiling results21.923/23.707ms disagree
with a stable improvement claim. HZB submission reduction is measured; a
reliable frame-time win is not established. One brief final source review passes.

Owner requests HZB repair at unchanged visual settings. Implementation follows
the [committed plan](../plans/2026-09-11-0604-microgeometry-hzb-repair-plan.md),
task-start `f450782247`, plan `b653365d6c`. No automated tests or proof audit.

## Matched baseline

2026-09-11, preserved `bin/godot.hzb_before.exe`, renderer source `ab4e61b17a`.
Native editor scene `demos/rtxdi_manual/microgeometry_stress/scene.escn`, all
10000 instances, saved camera, visible origin axes, 1 px selection tolerance,
actual HZB/guide size 2689 x 1602. Editor VSync and throttling temporarily disabled.

Capture `hzb-before-origin-profile` uses the ordinary Vulkan editor with
`--gpu-profile --print-fps --quit-after 900`. Logs and receipt are under
`C:/Users/lukas/AppData/Local/Temp/godot-render-repair-20260909/`.
Median of the last ten reports: GPU 21.923 ms, camera combined draw 12.383 ms,
aggregate sparse traversal 2.089 ms, cluster emission 0.418 ms, editor 43 FPS.
Frame 840 camera statistics: 926098 clusters, 79120798 triangles, HZB disabled.
Process exits 0, no ERROR lines, no timeout. Prior unprofiled matched baseline
is 43.5 FPS; the older 22.9 ms profile used hidden origin axes and is not the
direct baseline for this repair.

## Implementation and measurement

Initial six-file implementation builds with
`scons platform=windows target=editor accesskit=no d3d12=no -j16` (30.29 s).
Measured uncommitted source over `b653365d6c`; executable SHA256
`AF0669E98C67ACF6149246B6AC5D392B254E3B4DDFAF58B33C93E7C85FFE1681`.

Capture `hzb-after-origin-profile`: last-ten medians GPU 21.564 ms, camera
11.664 ms, sparse traversal 2.366 ms, cluster emission 0.409 ms. Frame840
has HZB enabled while original RTXDI history is still invalid for deformation.
Initial selection rejects 133525 clusters and emits 792573 clusters /
67947863 triangles. Current-depth recovery confirms all 133525 rejected clusters
remain occluded and emits none. Of 849702 queries, 512920 fail on nonzero depth
inside the rectangle, 173397 on interior zero depth, and 29860 exhaust the budget.
Queries make 9123564 texture fetches (peak32), recovery 1861433. Resident pages
3724, pending0, resident CLAS61333. No ERROR lines, exit0, no timeout.

Capture `hzb-after-origin-noprofile`: last-ten median42 FPS versus prior43.5 FPS.
No ERROR lines, exit0, no timeout. Consequently there is **no demonstrated frame
rate improvement**, despite lower triangle submission. The requested roughly9ms
GPU budget is not achieved. Investigating the retained fixed reverse-Z depth
margin before treating this as a completed performance repair.

The query balance closes exactly: 133525 hidden + 512920 nonzero failures +
173397 zero failures + 29860 budget exits = 849702 sampled initial queries.
Only 3.5% exhaust the budget, so raising the fetch cap is not the first fix.
The current fixed `0.00001` threshold is retained from the previous shader;
for perspective reverse-Z with near0.05 at100 units, its equivalent depth
separation is approximately1.96 units. This is a suspected rejection blocker,
not yet measured attribution of the512920 nonzero failures.

## Numerical margin continuation

Source inspection identifies perspective reverse-Z as -P[10] + P[14]/distance
(`projection.cpp`, corrected upload in `render_forward_clustered.cpp`, query
projection in `micro_geometry_select.slang`). The threshold now uses32 float
epsilons times (abs(nearest) + twice the projection cancellation term), rather
than1e-5 absolute depth. This is an engineering roundoff allowance, not a formal
bound for all pathological model/view transform cancellation.

Second ordinary editor build passes in34.12s. Final measured executable SHA256:
`E253D22FB915A3052854ECCF42AC22E782B89FAC88D2647BE7686CE5CB5A1245`.
It was built over `0754f926b5` with the exact six-file source subsequently
committed at `fa50c0398f` and `19f97459f7`; later Git commit identity is not baked
into this already-measured executable.

Capture `hzb-margin-origin-profile`: last-ten medians23.7065ms GPU,
12.253ms camera,2.587ms aggregate sparse traversal,0.409ms cluster emission,
0.0033ms recovery draw. Frame840 rejects140481 clusters and emits785617 /
67357876 triangles. Recovery rejects all140481 and emits none. Initial query
failures:173403 interior zero,508389 interior nonzero,27429 budget exits;
9087519 fetches, peak32. Recovery makes1949475 fetches. This margin correction
adds only6956 rejected clusters relative to the first repair; it was **not**
the dominant rejection blocker. Both final profile and unprofiled captures
exit0 without ERROR lines or timeout. Unprofiled last-ten median45FPS.

Stationary logs do not prove that every remaining cluster is visible: the query
compares a conservative projected bounding volume with depth. No movement,
reveal, visual quality or resize coverage is claimed. The owner retains visual
evaluation. No automated tests or proof auditor were used.

## Final handoff

Fresh baseline `hzb-before-origin-final-noprofile` uses the preserved pre-repair
binary, unchanged scene/camera/quality and visible axes: last-ten median40FPS,
exit0, no ERROR or timeout. This closes an additional paired comparison, not
the conflicting GPU-time result. Do not claim the requested frame budget or
use the triangle reduction as evidence of proportional speedup.

Fresh brief source review passes the exact two source commits and cumulative
`f450782247..19f97459f7`; no independent runtime audit was requested or performed.
All root-owned measurement editors exited. Restored original focused sleep6900,
unfocused sleep100000, VSync mode1 and continuous updatefalse. Origin stays visible.
Scene SHA256 remains
`F276E9A2B9A18B8DE4AE672BF339D7D7925D589E29131EA99A1F61B939A8B19A`.
The ordinary editor executable remains the final measured source, not a probe.

## Next optimization candidates — source inspection, 2026-09-11

At unchanged renderer source `19f97459f7`, `micro_geometry_select.slang:711`
emits one indexed indirect command per surviving cluster, with every triangle
included. `scene_forward_clustered.slang:842` executes the ordinary vertex
shader; `micro_geometry_pull_vertex` at line82 maps a triangle corner to the
cluster-local vertex. It repeats handle/material/page/generation/layout checks,
including the ten-attribute loop, on this vertex path. This is not a mesh-shader
raster path. The selected67.36M triangles represent202.07M triangle corners;
this is an input-work estimate, not a measured vertex-invocation counter.

Highest-priority candidates are amortizing safe shared checks per cluster and
processing unique meshlet vertices once, preferably in a batched mesh-shader
path. That path can also reject back-facing or provably sample-empty primitives
before emission; merely having area below one pixel is insufficient for safe
rejection. Current HZB rejects whole clusters and does not establish per-triangle
visibility. These are source-backed opportunities, not measured speedups or a
completed implementation plan. No new code, build or launch was performed for
this follow-up inspection.
