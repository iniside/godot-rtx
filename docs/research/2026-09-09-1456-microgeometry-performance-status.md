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
