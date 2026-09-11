# Native stress editor profile

Owner-requested editor launch on 2026-09-11 local time (2026-09-10 UTC), at
HEAD `6bb550ae7f`, using the existing ordinary editor executable built locally
on September 10 at 23:50. Source/binary equivalence was not independently checked.
Opened `res://microgeometry_stress/scene.escn` with `--editor --verbose
--print-fps --gpu-profile` and Vulkan. Process 107992 remains open for the owner.

After startup the editor responds, reporting approximately 16-18 FPS. Ten
consecutive GPU profile totals ranged from 53.318 to 58.530 ms. Median reported
stage batch means from that window:

| Stage | GPU ms | CPU interval ms |
| --- | ---: | ---: |
| Raster Initial Draw | 41.246 | 0.112 |
| Microgeometry Raster Sparse Traverse | 7.326 | 1.189 |
| Microgeometry Raster Cluster Emit | 0.914 | 0.033 |
| Microgeometry Raster Selection Reset | 0.812 | 0.218 |

Main-thread samples report roughly 54-58 ms frame admission wait, versus about
1-3 ms combined simulation and process/navigation intervals. A render sample
reports 38.695 ms device fence wait. These are elapsed intervals, not active
CPU sampling; GPU totals and stage batch means are different sample semantics.
The dominant measured cost is raster GPU work, not main-thread scene processing.

The stderr log contains three sparse-selection admission failures: 566,965,280
or 566,965,288 bytes requested against 536,870,912 bytes allowed, for 10000 tasks.
This is the selection allocation limit, separate from the removed metadata cap.
Consequent fallback/render completeness has not been investigated. Shader-parser
and material-contract warnings are also present. This is not a clean performance
baseline or a matched comparison to the earlier 1280x720 game measurements.
The editor viewport changed during observation (2689x1766, then 2689x1602;
latest camera lighting 1345x801). Profiling overhead was not isolated.

Evidence: `%TEMP%/godot-stress-editor-20260911.log` and
`%TEMP%/godot-stress-editor-20260911.stderr.log`; both continue growing while the
owner uses the editor. No engine/settings changes, automated tests or fixes.
