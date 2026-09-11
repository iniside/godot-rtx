# Editor camera motion cost

Verified 2026-09-11 UTC, source `244dc27176`, Windows Vulkan primary double
editor, native 10000-instance stress scene, viewport 2689x1602. Owner authorized
raising the selection limit and measuring movement. No renderer optimization,
automated tests or proof audit was performed in this step.

## Change and capture

`MicroGeometrySelection::MAX_PASS_BYTES` increased from 512 MiB to 1 GiB
(`fadf8414c4`), then 2 GiB (`244dc27176`). The 1 GiB attempt requested
1,077,613,540 bytes and failed admission. The streaming page pool remains 1 GiB.
The final double editor build passed in 33.48 seconds, exit 0.

The camera tool ran 45 seconds warmup followed by six nominal 10-second
segments: stationary, translation, rotation, two combined motions, stationary.
The route completed and restored the exact initial transform. A separate
full-profile run used the same route. Initial unfocused-editor captures were
limited to 10 FPS and are excluded from the performance result. For both final
runs, editor idle sleeps and VSync were temporarily disabled and continuous
updates enabled. All four original settings were restored; the capture-owned
editor was stopped after completion.

## Unprofiled result

| Segment | Drawn FPS | Viewport CPU median ms | Viewport GPU median ms | GPU p95 ms |
| --- | ---: | ---: | ---: | ---: |
| Stationary before movement | 14.99 | 53.72 | 14.87 | 19.65 |
| Translation | 12.82 | 59.40 | 14.21 | 30.69 |
| Rotation | 13.64 | 60.17 | 17.61 | 30.90 |
| Combined first | 11.00 | 62.43 | 19.13 | 33.07 |
| Combined second | 14.48 | 44.33 | 13.80 | 29.06 |
| Stationary endpoint | 8.78 | 18.41 | 10.93 | 15.04 |

Aggregate movement: **12.98 FPS**, drawn frames divided by actual elapsed time.
The last stationary segment took 18.10 seconds instead of 10 seconds, exposing
a long stall. Viewport timings are delayed samples with possible duplicates
and boundary lag; they do not account for all wall time or establish whole-frame
GPU latency. Earlier 34.4 FPS at the smaller limit is not a quality-equivalent
comparison: admitting more selection storage permits substantially more work.

The unprofiled route itself has no admission error through its completion log
offset 68144. Afterwards, at offset 68557, the restored camera requested
2,191,709,344 bytes against the 2,147,483,648-byte limit, with replacement peak
4,264,672,284 bytes. Thus **2 GiB does not solve admission generally**. The
full-profile route completed without ERROR lines; its timings differ materially
from the unprofiled run and are diagnostic, not an FPS estimate.

## Located costs and source causes

Full-profile stage medians across report windows:

| Stage | Translation ms | Rotation ms | First combined ms |
| --- | ---: | ---: | ---: |
| CPU RT TLAS Build | 43.16 | 48.26 | 34.61 |
| CPU AS Dependency Union | 42.64 | 47.76 | 23.78 |
| GPU RT Sparse Traverse | — | 44.44 | 131.09 |
| GPU interval after Request Readback Complete | 13.36 | 11.05 | 63.22 |

CPU dependency union is nested in TLAS work; do not add them. GPU labels describe
until-next-timestamp intervals, not inclusive stages. The interval after
`Microgeometry Request Readback Complete` contains record/page bitonic sorting
(`render_raytracing.cpp:2309`), not just readback. Stage values are report-window
averages whereas profiler GPU total is the latest frame; these statistics must
not be summed into a frame-time claim.

1. Camera pose and projection enter `input_signature`
   (`render_raytracing.cpp:2045`), which seeds `transform_signature` at 2542.
   Consequently camera-only changes invalidate TLAS transform preparation.
   Rotation does not change instance transforms. RT origin is independently
   stable until a 1024-unit boundary condition (`:137`), so ordinary movement
   need not change instance transforms either. Separate selection invalidation
   from actual instance/AS changes.
2. `tlas_build_from_buffer` rebuilds resource dependency unions
   (`rendering_device.cpp:917`, `_tlas_update_dependency_trackers` at 371).
   It visits every BLAS and its cluster/page dependencies, deduplicating anew.
   Cost is linear in all visited dependencies, including repeated shared pages.
   The measured dependency-union counter explains nearly all of the 43–48 ms
   CPU TLAS interval. Worker waits precede that interval. Cache/update the union
   from actual resource changes before considering more worker dispatch.
3. RT selection skips frustum rejection, then uses absolute view-space Z for
   projected error (`micro_geometry_select.slang:571–587`). Lateral/offscreen
   geometry can have nearly zero depth despite being distant, requesting fine
   detail. Primary selection also clamps error to at most 1 px and offscreen
   multiplier to 1 (`render_raytracing.cpp:2052`). The profile reaches 10,389,151
   selected clusters across 1178 shared cuts. This establishes a problematic
   criterion and observed growth, not isolated attribution of all growth.
4. Sorting and publishing large replacement cuts are expensive. Publication
   creates BLAS for new representatives, not every live cut every frame. Its
   free-slot search also restarts from slot 1 for each new representative
   (`render_raytracing.cpp:2375`). Avoid full replacement work where cuts survive.

Stationary TLAS invalidation reason is not isolated by current stdout counters.
Camera jitter is separate from the hashed projection in source; it is not a
supported explanation. Residency, publication and persistent generation changes
remain relevant. Device fence wait median was 0.003 ms but maximum 158.933 ms:
there are stalls, despite small typical waits.

## Memory interpretation

The profile reports `cut_working_bytes=9,817,640,832`, including
`BLAS_scratch_bytes=6,114,962,560`. Do not add them. The latter includes BLAS
allocations and shared scratch, despite its name. Other accounted buffers total
3,702,678,272 bytes. The unprofiled post-route log grows to
`cut_working_bytes=14,541,567,724` before admission failure.

This is current-build allocation accounting, including cuts awaiting retirement;
it excludes separately retired whole builds and TLAS itself. It is not physical
VRAM residency. The 2 GiB limit bounds selection/epoch admission, not the total.
Definitions: `render_raytracing.cpp:1711,2111,2404,2454,2517,5668`.

After the unprofiled route, nvidia-smi reported global GPU memory 23029/24564 MiB;
process private bytes were 28,434,665,472 and working set 7,346,524,160. These are
post-route observations, not recorded peaks or proof of GPU paging.

## Evidence

Local captures are under
`C:/Users/lukas/AppData/Local/Temp/godot-render-repair-20260909/`:

- `motion-2g-unthrottled-noprofile-{result.json,markers.json,launch.json}` and `.log`.
- `motion-2g-unthrottled-profile-{result.json,analysis.json,markers.json,launch.json}` and `.log`.
- `editor-camera-motion-route.json`, `motion-editor-original.json` and `capture_motion.py`.

Build log: `C:/Users/lukas/AppData/Local/Temp/godot-motion-budget-2g-build.log`.
Logs and generated editor caches are not staged. These measurements identify
specific renderer work to remove; they do not establish a repaired frame budget.
