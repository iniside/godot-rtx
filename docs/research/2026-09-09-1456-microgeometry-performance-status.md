# Microgeometry performance investigation

2026-09-09 UTC. The owner stopped Step 6 correctness/appearance verification and
requested instrumentation and measurements for roughly 34 ms GPU and 60 ms CPU
in `demos/rtxdi_manual/microgeometry/scene.tscn`. Appearance remains the owner's
decision. Step 6 is checkpointed at `cab0af3d04`, with checkpoint-only Unicode
correction `45f5eae299`; its final review remains paused.

## Baseline

Evidence root:
`C:/Users/lukas/AppData/Local/Temp/godot-micro-performance-7dd0711015f641428c05c83a6a93c3c9/`.
The hash-pinned `baseline-bin` snapshot contains the current double editor and
adjacent DLLs; `baseline-binaries.json` and `fixture-inputs.json` record their
identities. The owner closed their editor before measurements. Runs use one Godot
process, RTX 4090/Vulkan, 1280x720, disabled VSync, still camera/objects and normal
rendering. No simultaneous build or screenshot capture ran.

Existing `--gpu-profile` output is flat: stage values are batch means, while the
printed total is the last frame in the batch. The table reports medians of those
reported values, not a mean or percentile of all individual frames. Five startup
batches are discarded, and the frozen run discards five batches after its logged
freeze event. Old console output does not provide CPU phase timings.

| Configuration | Analyzed batches | GPU total, median reported last frame | TLAS Build interval, median batch mean |
| --- | ---: | ---: | ---: |
| Normal, live selection | 35 | 38.210 ms | 31.897 ms |
| Normal, selection frozen after 5 s | 55 | 27.066 ms | 24.817 ms |

Both processes exited 0 without `ERROR:` or `SCRIPT ERROR`. Receipts retain exact
commands, timestamps and executable hashes. `baseline-summary.json` contains
the parsed values and log hashes. Freezing is a diagnostic comparison, not an
optimization proposal. It also changes other intervals, so the difference is
not attributed exclusively to DAG selection.

The broad `TLAS Build` label dominates both configurations. It does not isolate
native TLAS construction from scene gathering, microgeometry selection,
BLAS preparation, feedback or CPU-induced GPU idle time. Finer paired CPU/GPU
markers are being added through the existing native timestamp mechanism before
assigning a cause or changing algorithms.

## Current scope

Instrument streaming, CPU scene preparation, selection, CLAS/BLAS/TLAS,
dependencies, readbacks and waits. Preserve the existing timing mechanism and
render behavior; do not introduce a profiling framework or quality tuning.
Measure the same scene after the instrumentation build, then identify the
dominant GPU and CPU stages. No automated tests are authorized or run.

## Instrumented measurements

Instrumentation is committed at `38ae7d5077` (parent `45f5eae299`, nine source
files). Fresh independent exact/cumulative source and evidence review passed.
Other backends, template builds, separate-render-thread execution and profiling
overhead are unverified. Default timestamp capacity is insufficient for the
moving capture; the successful budgeted runs do not establish default usability.
The double editor instrumentation build passed in 49.32 seconds. Executable
SHA256: `738c17351dcb48793cc0394988340ea8ff47d2f6d0bcfe7a94b2e5d403479685`.
`cpu-moving-budgeted` and `cpu-still-budgeted` each ran 600 frames and exited 0
without ERROR output. Both use the settings above; only the latter adds `--still`.
A temporary project `override.cfg` increased the existing timestamp capacity to
2048. Its exact copy is retained as `profiling-override.cfg`; the project copy
was removed after checking its contents. Production defaults are unchanged.

Earlier `instrumented-moving` and `cpu-moving-short` runs exhausted the default
256 timestamp queries and timed out. Their timings, including CPU above 200 ms,
are invalid and withdrawn. The budgeted moving run reports about 325-349 queries
in its final batches and does not exhaust the pool.

`budgeted-summary.json` records medians of reported stage batch means after
discarding the first five occurrences of each label (45 moving, 16 still).
These are conditional reported samples, not individual-frame percentiles.

| Interval | Moving CPU | Still CPU | Moving GPU | Still GPU |
| --- | ---: | ---: | ---: | ---: |
| RTXDI Initial Sampling | 19.557 ms | 0.866 ms | 0.087 ms | 0.098 ms |
| RTXDI Temporal Resampling | 19.076 ms | 0.698 ms | 0.011 ms | 0.039 ms |
| RTXDI Spatial Resampling | 19.101 ms | 0.694 ms | 0.044 ms | 0.102 ms |
| RTXDI Final Shading | 19.101 ms | 0.701 ms | 0.017 ms | 0.035 ms |
| RT Prepare Cut | 0.002 ms | 0.003 ms | 11.159 ms | 12.260 ms |
| RT Publish Cut | 0.002 ms | 0.006 ms | 6.826 ms | 12.408 ms |
| RT Update Transforms | 0.005 ms | 0.005 ms | 10.429 ms | 8.596 ms |
| Native microgeometry TLAS build | 0.007 ms | 0.005 ms | 0.069 ms | 0.041 ms |
| Device fence wait | 0.003 ms | 8.952 ms | n/a | n/a |
| Download callbacks | 0.114 ms | 0.088 ms | n/a | n/a |

Main instrumentation reports `render threaded false`. Median batch means after
five startup batches are simulation 0.031 ms, process/navigation 0.740 ms and
render draw 132.746 ms moving; still values are 0.026 ms, 0.741 ms and 37.061 ms.
These are wall intervals, not sampled active CPU time. Render
wall includes submission/recycling and can include waits. Timestamp span is
from a delayed GPU-profile frame and must not be subtracted from current render
wall as though both described the same frame. Frame-recycle-inclusive timing
contains fence wait, copies and callbacks; do not sum parent and children.

## Bottlenecks and thread direction

`shaders/forward_clustered/micro_geometry_rt.slang` assigns one lane per geometry
and serially scans `task.cluster_count` in Prepare, Publish and Update Transforms.
The transform-only path scans the complete bitmap to establish nonemptiness.
`render_raytracing.cpp` cycles Prepare/Publish whenever feedback permits, without
a scene/camera/residency dirty predicate. These are confirmed source properties;
no hardware occupancy measurement has been performed. Native TLAS construction
is much smaller than the formerly broad TLAS-labelled preparation interval.

The owner requires minimal main-thread work and CPU rendering preparation as
worker tasks, with central rendering limited primarily to synchronization,
merging and submission. Moving all preparation to one render thread would not
fulfil that requirement.

Source navigation identifies a strong CPU attribution candidate:
`RenderingDevice::_uniform_set_add_acceleration_structure_dependencies` builds
a fresh vector per dispatch/trace and traverses TLAS, every BLAS and each CLAS
storage dependency. `_cluster_tracker_push_unique` uses `LocalVector::has`, a
linear search. `blas_build_from_clusters` attaches the entire supplied CLAS
storage set to every destination BLAS; `MicroGeometryStorage::get_clas_dependencies`
collects allocated page CLAS storage, not just the selected cut. With B BLAS
sharing P pages, the repeated union can require order B*P squared comparisons.
This agrees with the measured stage pattern but is not a direct measurement of
that helper alone. Per-helper timing or sampling is still needed. DDGI batch
means also include frames without a particular cascade trace, so its roughly
9 ms must not be read as half the per-invocation cost of RTXDI's roughly 19 ms.

Current RD initialization selects a combined graphics/compute main queue and a
transfer queue. Rendering submissions use the main queue; there is no dedicated
async compute selection. RD graph has worker-recorded secondary draw command
buffers, but their active configuration was not verified. Async compute would
require graph scheduling and explicit dependencies/resource lifetime handling;
it is not an existing switch. No threading, scheduling or shader optimization
has been implemented in this instrumentation task.

## Approved repair execution

The owner approved the corrective plan, committed separately at `02e21acb92`.
The owner restarted the session after the runtime agent-limit blocker.
Step 1 resumed through a separate implementer from `ac7045103c`; helper-level
instrumentation and matched before/after CPU measurements precede the optimization.
The normal delegated implementation/review workflow is active again.

Step 1 instrumentation-only double build passed (source hashes unchanged during
build); binary SHA256 `e39298f9bef40163e72ddcffcfff5826cc052d318ca57aed238957399c6a0160`.
`step1-before-moving` and `step1-before-still` each completed 600 frames with
exit 0 and no Godot `ERROR:` lines. Existing SPIR-V parser startup diagnostics
also occur in the earlier baseline logs; these captures are not a claim that
all startup diagnostics or fixture material warnings have been fixed.

In `step1-before-moving-summary.json`, 44 reported batches remain after the
first five. Median AS dependency-union CPU time is **114.216 ms**, versus
**0.470 ms** for registering the resulting graph usages and **1.922 ms** for
inclusive cluster-BLAS build CPU work. Median tracker candidates are 196524 per
frame. The unique-search counter reports an upper bound of 748940453.5 comparisons
per frame, not an actual comparison count. This directly confirms the helper's
dominant CPU cost.

Step 1 optimization is committed at `2b2d8476f7`; fresh exact/cumulative review
passes. The double build passed with unchanged source hashes; optimized binary
SHA256 is `8fdfbc65a8ed0db4b426e954b04bcdedcb5bede552179ef5bd008da4114105f5`.
Both matched after captures exit 0 without Godot `ERROR:` lines.
`step1-comparison.json` records the same conditional batch-mean semantics. In the
last ten moving batches, dependency-union CPU falls from 114.789 to 2.822 ms,
including construction at its new TLAS-build location. Graph usages remain
approximately matched (24486 versus 24470 per frame). Still union cost falls
from 4.124 to 0.403 ms with 3312 graph usages in both runs.

Whole-frame attribution is not settled: initial moving render wall improves,
but still wall varies upward; a repeat of the OLD binary also rises sharply
(last samples approximately 80 ms GPU). The retained sequence does not establish isolated GPU conditions or identify
a competing process. After-exit GPU activity and Unreal Editor presence were
operator observations without a retained process inventory. Do not infer a GPU
regression or improvement from this noisy sequence. The owner was asked to quiet
the competing workload; their applications were not modified.
Direct CPU-helper reduction is supported separately.

The bounded `step1-lifetime` run exited 0 without timeout, with unchanged binary
hash and no Godot `ERROR:` lines. Native controls exercised two unload/reload
cycles (including two active viewports), freeze/unfreeze and maximize/restore.
Retained events show unloaded AS memory in the tens of KiB and one resident
page, followed by visible reload and `cache=replace` log events. The reloaded
secondary-view screenshot shows 4064 resident pages and approximately 1.49 GiB
of AS storage; exact pre-unload operator observations were not retained. Events, five screenshots and the receipt are retained.
This exercises real resource-lifetime transitions, not exact RID-generation
reuse or appearance correctness. The owned process exited; Step 2 builds are
released. Independent artifact audit is pending alongside Step 2 implementation.

The Step 1 instrumentation-only binary (`e39298f9...`) and adjacent DLLs are
preserved in `C:/Users/lukas/AppData/Local/Temp/godot-render-repair-20260909/instrumented-before-bin`;
its provenance is recorded in `build-step1-instrument-double.receipt.json` and
the `step1-before-*` launch receipts. The optimized Step 1 binary (`8fdfbc65...`)
was overwritten by the subsequent build; its build/run hashes remain retained,
but that exact binary is unavailable for replay. The separate `baseline-bin`
and `baseline-binaries.json` retain the earlier `738c1735...` binary.
An additional 600-frame moving baseline without
`--gpu-profile` exited 0 without ERROR output in 57.84 seconds including startup
and shutdown. Receipt: `repair-baseline-moving-no-profile.receipt.json` in the
original evidence root above. This elapsed process duration is not a steady-state
frame-time measurement or an isolated profiler-overhead attribution.

The owner explicitly excludes split screen and XR/VR support. Subsequent manual
performance validation uses one game viewport; internal shadow/raster/RT passes
and ordinary editor operation remain in scope. Earlier two-view captures are
historical evidence, not a requirement to expand supported configurations.

## Step 2 parallel GPU preparation

Commit `984dccbd32` replaces serial per-mesh scans with tiled GPU work and a
hierarchical scan. Transform finalization reads retained active counts without
scanning membership. Candidate membership and counts swap together after BLAS
build recording. Ordinary/double builds and fourteen Slang/SPIR-V variants pass;
fresh exact/cumulative source review passes. Build and shader receipts live in
`C:/Users/lukas/AppData/Local/Temp/godot-rendering-repair-step2-20260909/`.

The double executable SHA256 is
`b029f299b2839bcd39885925a257c10d2fd1dac74354c82f9f92274edf116203`;
both precision binaries and DLLs are retained in `step2-bin` under the repair
evidence root. `step2-comparison.json` summarizes single-view Vulkan captures at
unchanged controls, 1280x720 and timestamp budget 2048. Old shader captures use
the retained pre-Step-1 instrumented binary `e39298f9...`: whole-frame comparisons
therefore cover Steps 1 and 2 together, not an isolated incremental Step 2 effect.

| Median of last ten reported values | Older still | New still | Older moving | New moving |
| --- | ---: | ---: | ---: | ---: |
| GPU total, last-frame observations (ms) | 38.822 | 2.957 | 39.445 | 4.906 |
| Render wall, current-frame observations (ms) | 38.003 | 3.870 | 126.598 | 9.311 |
| RT Prepare Cut, batch mean (ms GPU) | 12.246 | 0.0131 | 12.953 | 0.0120 |

Old captures contain 600 frames; extended new captures contain 3000, providing
15 still and 37 moving reporting windows. These are not individual-frame
percentiles, and render wall is not main active CPU. The publish marker is
absent in the new console output; absence is not a zero-time measurement.
New moving transform observations have median 0.00268 ms among their last ten
reported batches. No simultaneous build ran during these captures. Both short
new runs and all table runs exit 0 without timeout or Godot `ERROR:` lines;
existing shader-parser diagnostics and fixture material warning remain.

The bounded native `step2-lifetime` capture also exits 0 without timeout or
Godot `ERROR:` lines. Retained loaded/unloaded screenshots show 4079 versus one
resident page and 707709 versus 58 KiB AS storage. Reload reconstructs geometry;
freeze/unfreeze and maximize/restore complete. The already performed second-view
portion is historical evidence only and is excluded from subsequent acceptance.
Additional user window input occurred during this run; the later far-plane value
is not attributed to automation. Events and screenshots are retained separately
from profiling captures. This is resource-lifetime evidence, not appearance or
an exact RID-generation-reuse claim. Step 3 dirty-input work is in progress;
main/worker architecture changes remain pending.
