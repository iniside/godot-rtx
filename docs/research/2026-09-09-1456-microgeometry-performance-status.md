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
released. Independent artifact audit round 2 passes at documentation commit
`57e9d447e5`, after correcting baseline provenance and narrowing unsupported
exact-memory and external-workload claims.

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

The dense Lucy/Thai workload is authorized at 5000 separate instances per mesh.
SceneTree/game-logic scalability work is explicitly excluded by the owner and
remains on separate roadmap items; its measured cost must be reported separately
from rendering preparation, recording and synchronization.

Dense-workload capacity finding before launch: preserved Lucy/Thai inputs contain
28,055,742 and 10,000,000 triangles. The importer caps each cluster at 128
triangles (`micro_geometry_builder.cpp:95,305`) and checks complete leaf coverage,
so the two assets require at least 297311 leaf clusters. With 5000 instances
each, current RT `cluster_work` is at least 1486555000, exceeding its
`UINT32_MAX / 8` admission limit (536870911). Membership plus committed membership
and reference buffers alone require at least 23784880000 bytes (22.15 GiB),
before internal DAG clusters, selection, AS and scratch allocations. The current
10k-instance path therefore necessarily rejects before rendering. Do not lower
the fixture count or change it to a few MultiMeshes to hide this renderer limit.
Exact imported DAG counts remain pending. A bounded investigation is identifying
the necessary renderer data-layout correction; SceneTree changes remain excluded.

## Step 3 dirty-input reuse

Commit `0d1ed0abe9` passes fresh exact/cumulative source review and final ordinary
and double builds. Twelve source hashes are unchanged across both recorded
builds. The double executable is retained under `step3-bin`, SHA256
`4e0ff3dd0979531bdc0d999a229cf11c34da307b4899052b2e3013f517d7cbff`.
The native RD invalidation query preserves same-address BLAS rebuild/refit
handling; externally exposed MultiMesh buffers remain conservatively dirty.

`step3-still` and `step3-moving` each complete 3000 single-view Vulkan frames
with exit 0, unchanged executable hashes, no timeout and no Godot `ERROR:` lines.
Existing startup diagnostics remain. `step3-comparison.json` retains raw-label
summaries. Still capture reports RT Prepare Cut only in the first batch; the last
ten dependency-union build counts and measured helper cost are zero. Still GPU
median is 2.981 ms and render-wall median 3.175 ms. Moving work remains active:
last-ten union builds are one per frame and helper cost 2.837 ms. Moving GPU
median is 6.482 ms and render wall 10.033 ms; this capture does not establish an
incremental moving-frame improvement over Step 2. Several unrelated GPU pass
intervals also differ, so additional causal attribution is withheld. The main
thread boundary and workers remain unimplemented at this checkpoint.

The owner-authorized instancing/streaming extension is recorded as Steps 3A/3B
in the corrective plan: sparse shared selection, then compact GPU-interned cuts
and shared BLAS with exact page ownership. Existing asset/page streaming remains
the authority. Fresh independent extension-plan review passes against `0d1ed0abe9`; Step 3A
implementation follows, with Step 3B and threading still pending.

## Dense fixture import and population

Fixture `c4633542e7` in `demos/rtxdi_manual/microgeometry_stress` passes fresh
source review and separate proof audit. Normal import, native resource inspection,
and three-frame headless population/teardown exit 0 with empty stderr. Actual
population is 5000 Lucy plus 5000 Thai MeshInstance3D nodes, 10000 valid API
instance RIDs and two shared ArrayMesh resources. Startup placement/counting
observed 93.133 ms after resource loading; it is not a steady-state measurement.
No GPU launch has been performed for this fixture.

Real import took 436.188 seconds. Lucy has 634477 clusters, 316198 leaf clusters,
38934 groups, 19 levels and 37918 pages; Thai has 229774 clusters, 114600 leaf
clusters, 14091 groups, 18 levels and 13505 pages. At 5000 instances each the
old full-DAG RT layout requires 4321255000 work items and at least 69140080000
bytes for membership/cached membership/references (64.39 GiB). These actual
counts supersede the conservative leaf-only bound above. File metadata totals
are not GPU metadata admission sizes. The native storage formula at `0d1ed0abe9`
requires 45803180 B for Lucy and 16572108 B for Thai: 62375288 B (59.486 MiB)
combined, leaving 4733576 B under the 64 MiB admission cap in fresh storage.
Existing active/retired metadata also consumes this cap. This is source/ABI
arithmetic, not a driver-memory measurement; it does not require a budget increase
for the two shared resources in the isolated fixture.

Evidence: `C:/Users/lukas/AppData/Local/Temp/godot-dense-microgeometry-20260909/`,
including import/inspect/population receipts, exact asset/output hashes and
`fixture.diff`. The copied GLBs remain local untracked dependencies; originals
are unchanged. The same pinned Step 3 executable produced these artifacts.
A later `runtime-inputs` snapshot retains the exact still-matching project.godot,
profiling override and scene/script contents for subsequent real-device runs.
The review boundary remains importer/resource/Node/API allocation and teardown;
headless configured Vulkan labels do not establish an initialized Vulkan device.

Step 3A first implementation `c02a8db660` builds in ordinary/double and passes
both selector shader compilations/SPIR-V validation. The pinned `step3a-bin`
double hash is `ac937cd8251b6858341a6aeaf0288ac0731942ea10a016b5e698f01a38f6330d`.
Its 3000-frame still Vulkan run exits 0 without Godot `ERROR:` lines. Fresh
source review nevertheless rejects an intra-dispatch frontier-publication race
and capacity-history exhaustion across retired surface generations. Those
findings require fixes and a fresh second review; successful execution does not
disprove either defect. Step 3B remains held until the selector contract closes.

Step 3A correction `571816b9f4` passes fresh final round 2 source review,
ordinary/double builds and both selector Slang/SPIR-V variants. The immutable
per-depth frontier snapshot closes publication ordering; live/recent capacity
ownership replaces lifetime accumulation. The double executable is retained in
`step3a-fixed-bin`, SHA256
`5de9b317eb1b90f4090eb7825d77e75113bab7273fd66e1c2862334e582bb81e`.
Both `step3a-fixed-still` and `step3a-fixed-moving` complete 3000 single-view
Vulkan frames with exit 0, unchanged hashes, no timeout and no Godot ERROR lines.
Last-ten reported GPU medians are 3.910/7.731 ms, render-wall medians
3.967/14.033 ms; these runs do not claim an incremental performance improvement.
Still dependency builds remain zero; moving helper cost is 2.828 ms. Evidence
is in `step3a-fixed-comparison.json` under the same root capture directory.

The bounded `step3a-fixed-lifetime` run exits 0 without timeout after native
unload/reload observations: resident pages 541 to 1, then 3340 with 597 pending,
and later 4092 with 364 pending. Four screenshots and an event log are retained.
Owner input was detected during the reload action; motion and RT error changed
from still/4 to moving/16, so reload/settings attribution is explicitly mixed.
One native save/load diagnostic reports no imported ArrayMesh available; this
interactive run is not claimed error-free. Long history saturation and targeted
HZB-rejection outcomes remain unverified. Step 3B implementation now follows.

The dragon HUD CLAS statistic is cumulative successful cluster builds, incremented
at page CLAS creation in `micro_geometry_storage.cpp:206`; it is not a live CLAS
count. Rigid transform changes alone must preserve CLAS. New resident pages may
require new builds; the Step 3B telemetry distinguishes sharing and residency.

The follow-up `step3a-frozen-transform` interaction was interrupted before the
freeze/motion observation. Its wrapper reached the 180-second timeout and
records no actual normal exit; it supplies no transform-only CLAS proof.

Bounded selector-cost research at `571816b9f4` identifies repeated parameter
buffer uploads and 21-binding uniform construction on every depth dispatch.
The dragon traverse interval includes 37 dispatches. Fix `d7b9f9cdd2` moves
mode/level to an 8-byte push constant, prepares the 424-byte parameter block
and uniform set once per operation, and rejects unused queue reservations
before resolving task/instance/asset data. It preserves the publication
dispatch and raw-address dependencies on every list. Both selector Slang and
SPIR-V variants pass; coordinated Step 3B caller closure, engine builds, fresh
source review and matched runtime timing remain pending. No performance gain
is claimed from source or shader compilation alone.

## Step 3B shared RT cuts: admission and bounded lifetime proof passed

Commit `48e42e5fb9` and selector optimization `d7b9f9cdd2` pass fresh source
reviews and ordinary/double engine builds. Pinned ordinary SHA256 is
`bf838f143a68c6935fd6adbd29c75f2a5f0de006a5109962736371ef639deab3`;
double is `4b523b414dbbaad7b8eaf8f2e052b7b8bc1cf753b1312a692cebdc627c78cdcb`.
The seven-file RT/storage replacement removes full-DAG per-instance membership
and broad CLAS dependencies, introduces exact shared cut/page ownership, and
closes metadata replacement admission in MeshStorage. Existing Slang build
registration remains authoritative; no new RD public API is introduced.

`step3b-dense-admission` renders 300 real RTX 4090/Vulkan frames at 1280x720,
exits 0 without timeout or Godot ERROR lines, and confirms 5000 Lucy plus 5000
Thai native instances sharing two meshes and two cut BLAS. Latest publication
reports 53873376 B cut working storage, 62375288 B metadata, 268435456 B page
pool and six resident pages with zero pending. These are separate accounting
buckets; neither a whole-VRAM measurement nor a like-for-like total reduction
from the old 64.39 GiB membership/reference bound is claimed.

`step3b-dragon-moving` completes 600 frames and exits 0 without timeout/ERROR.
Its eight geometry instances publish three then six shared cuts, selected count
8 to 1982, and resident pages 3 to 2147 with final pending zero. The 3000-frame
`step3b-dense-orbit` instead reaches the 180-second timeout. Its 148 reporting
windows have no ERROR lines; provisional last-ten medians are GPU 13.045 ms,
MAIN render-draw reported frame mean 71.784 ms (including waits) and RT Scene
Gather CPU 49.024 ms. This is not normal
completion evidence. The saved screenshot establishes the rendered dense field.

The 600-frame `step3b-dense-orbit-bounded` records normal profiles followed by
37781 timestamp-capacity ERROR lines and a 120-second timeout with no actual
exit. The first proof audit rejected completion on this concrete shutdown/
lifetime gap. The submission-maintenance correction below closes that failing
branch without raising the timestamp budget. Receipts retain all failed runs;
the earlier admitted/dragon runs remain bounded valid evidence.

Separate source diagnosis found two full material-generation scans per instance
surface inside RT Scene Gather despite shared materials. The gather-local
RID/instance-uniform-offset reuse below addresses this without changing
SceneTree behavior or adding a persistent material cache.

### Submission maintenance correction and subsequent evidence

`cfa892579a` adds one maintenance execution per pending GPU submission.
Source tracing shows instance teardown repeatedly called dirty-resource
maintenance and emitted five streaming timestamps without a new frame reset.
The finite cleanup amplification, rather than a proven readback-drain loop,
owns this correction. Ordinary/double builds and fresh final source review pass.
Pinned ordinary is `06cdd71759609c800fee3effd131f193ee13dbe3445299e0541bef1493c5382c`;
double is `30af6881dc080de20402a4332afd8145e30fe0cb7baf7095933d891e3aa2c04e`.
`step3b-fixed-dense-orbit` still timed out at 120 seconds without timestamp
errors; it remains invalid as normal-exit proof. The same 600-frame engine
command in `step3b-fixed-orbit-stack` completed naturally in 95.017 seconds
with exit 0, no timeout, no ERROR lines and unchanged binary hashes. Its
external timeout was 180 seconds. No debugger was attached and no input action
closed that run; the name reflects an unused stack-capture intention. This
proves that retained run, not the cause of every earlier timeout.

Native cfa unload/reload observations in `step3b-fixed-lifetime` show pages
541 -> 1 -> 541 and AS accounting 105371 -> 55 -> 105371 KiB. Screenshots
and events retain the successful transitions; that manual session later reached
its external timeout, so it is not normal-exit proof. F4 plus motion retained
selected counts but continued requesting pages; camera-pass freeze does not
freeze internal shadow selection (`render_forward_clustered.cpp:650`). No
transform-only CLAS invariance is claimed. A subsequent manual reload/close
session was cancelled after Computer Use stopped; root terminated only its
verified helper process. The owner clarified that stopping Computer Use does
not cancel implementation. Normal shutdown evidence comes from the completed
automatic dense and dragon runs, not those interrupted manual sessions.

### Gather-local material reuse

`e319cca724` implements the gather-local full-RID material and RID/instance-
uniform-offset generation tables in the four `build_tlas()` geometry paths.
No cross-frame cache or SceneTree behavior changes. Ordinary/double builds pass;
ordinary SHA256 `9af58feaaadf0aaa1c2cc50f3187c0a14af2d7e332c90518a4e9f37a77035596`,
double `57d784798aa04f15dcdaF710c658e9f3de260e397ba9f7adfa15ee4fbd28b537`.
`material-reuse-dense-orbit` completes 1500 Vulkan frames and exits 0 without
timeout/ERROR, retaining 10000 instances and two shared assets. Last-ten report
medians against the successful cfa 600-frame run are RT Scene Gather CPU
50.269 -> 6.809 ms and MAIN CPU PROFILE render draw 73.876 -> 27.103 ms
(reported frame means, including waits). Both use unchanged
1280x720/settings/workload, but time-driven camera positions differ; no isolated
whole-GPU gain is claimed. MAIN render draw includes waits and is not main
active CPU. `material-reuse-comparison.json` retains raw labels and boundaries. Fresh
exact/cumulative material source review round 1 passes at `e319cca724`. Final shared-cut proof round 2 passes
at frozen `cfa892579a`, including dense admission, successful profiled shutdown,
streaming and the bounded unload/reload observations above. It does not close
transform-only invariance or the remaining threading steps.

## Remaining worker preparation

Step 4 frame ownership passes final source review at `9f42b239e3`; its
independent proof audit is pending. Step 5 implementation is active. The bounded
[canvas preparation inspection](2026-09-09-1919-canvas-worker-preparation-summary.md)
and [RT gather inspection](2026-09-09-1919-rt-worker-preparation-summary.md)
and [3D cull/raster inspection](2026-09-09-1945-raster-worker-preparation-summary.md)
identify cull, batch, cache and upload ownership for Step 5. They do not claim
worker dispatch. RD recording remains Step 6; a bounded
[graph/driver handoff](2026-09-09-2021-graph-worker-recording-summary.md) identifies
shared instruction/resource state and the existing draw-only secondary contract.

## Step 4 frame ownership: final source review and bounded native evidence

Step 4 candidate `42ec20f6c3` builds in ordinary, double and template-debug
configurations; source review round 1 rejects editor admission reentrancy.
The correction and final evidence follow below. Retained earlier ordinary
build02 SHA256 `a5266e7afd74c790420e5c8568dd851938cea5a47419029f158b227d8696b8bb`
uses separate rendering by default, bounded iteration admission and captured
frame inputs. Its exact source manifest and patch are retained as
`step4-build02-source.json` and `step4-build02.patch` beside the previous receipts.

`step4-dense-orbit` completes 1500 frames on the 10000-instance Vulkan workload
with exit 0 and no ERROR. Last-ten MAIN reported frame-mean medians are render
transfer 0.010 ms, admission/callback active 0.010 ms, simulation 0.4285 ms,
process/navigation 0.048 ms and admission wait 26.8395 ms. Waiting for an older
occupied slot is reported separately from main active work. Rendering remains
serial behind this boundary; these results do not establish worker preparation
or recording. `step4-dense-orbit-metrics.json` records these exact labels.
The render-start overlap counter can miss main work begun during an already
running draw and is not evidence that dense execution never overlaps.

The initial build01 SR fixed-60 callback run exits 0 without ERROR and captures
frame 301. Build02 RR fixed-60 callback also exits 0 without ERROR, reports
feature 1001 evaluation with `sl::eOk`, and captures frame 301. Both images visibly
contain the gallery and HUD; no appearance acceptance is claimed. Build02
reports render thread 7 versus main thread 1. The build02 no-draw command uses
`--disable-render-loop --fixed-fps 60 --quit-after 120` and exits 0 without ERROR.
Source changes after these pinned binaries are not covered by those observations.

Bare `-e --verbose --quit-after 30` on build02 exits 0 but reports one empty-image
ERROR in `TextureStorage::_texture_2d_update()`. The retained pre-Step4 material
binary completes the same command without ERROR. Queued image snapshots in `42ec20f6c3`
close the traced font-atlas producer mutation; build03 bare-editor replay exits
0 without ERROR. Candidate final ordinary SHA256
`bb31c83f05a0647238dc622bfce783bdf09da76057b9254a7364d23b20616b3c`
then passes both SR/RR fixed-60 callbacks with captures and clean exit.

The candidate project-editor run times out at 120 seconds without ERROR. An
unchanged retry with a 240-second limit exits 0 in 42.24 seconds, but reports
four begin/end-frame ownership errors. The latter is a concrete defect:
ProgressDialog and editor thumbnail generation call nested Main iterations;
rejected nested admission still reaches unconditional end-frame and consumes
the outer owner. Round 1 source review independently confirms this finding.
The correction makes only outer entry/exit acquire/release admission; nested
iterations retain the outer token and cannot consume its slot. No debugger was
attached; the first timeout cause is unproven. Runtime text input hashes are
retained in `step4-runtime-inputs.json`. The ordinary editor automatically
removed the existing Double Precision feature marker from `project.godot`;
only that metadata delta was restored, reproducing the original SHA256 exactly.
`step4-editor-project-feature-change.patch` retains the delta. All eight text
inputs match the retained manifest after the final runs; no quality setting
was changed.


### Final nesting correction and pinned runtime

`9f42b239e3` makes admission depth explicit and drains completed main callbacks
on nested entries. Fresh final source review round 2 passes against exact and
cumulative `e319cca724..9f42b239` changes. Ordinary, double and template-debug
builds pass (`step4-editor-build06.log`, `step4-double-build06.log`,
`step4-template-build04.log`). The 22-file final source manifest is
`step4-nesting-complete-source.json`; pinned binaries are in
`step4-nesting-complete-bin`, with hashes in the corresponding binaries JSON:

- Ordinary: `b7cd64375ccf1d3aa3b7a3b717f45e57f481eb9efdac93e170f3eeea2b77ac0e`.
- Double: `67aed4ca531c5bfd0512efceeb283f1e7b8f43f7de33a41d197aa62d3e27539e`.
- Template: `3cb00f4a5340c456eee82d4165a652b4b783a8b7988ce509387fdfeff5648e68`.

The final `step4-nesting-complete-project-editor` run exits 0 without ERROR
in 21.292 seconds. Final `step4-nesting-complete-dense-orbit` completes 1500
iterations without concurrent compilation and exits 0 without ERROR. Last-ten
reported window medians are main render transfer 0.0225 ms, admission/callback
active 0.0115 ms, simulation 0.038 ms, process/navigation 0.067 ms and admission
wait 27.1335 ms. Render queue delay is 27.261 ms. These are window means summarized
by a median, not individual-frame percentiles. Admission wait and queue delay
are not active CPU work and must not be added together as independent costs.
The time-driven orbit prevents an isolated whole-GPU gain claim.

Final `step4-verified-sr` and `step4-verified-rr` fixed-60 runs capture frame 301
and exit 0 without ERROR. These final PNGs have not been visually evaluated;
this establishes the capture callback and bounded native execution only.
Final `step4-verified-bare-editor` and `step4-verified-no-draw` also exit 0
without timeout or ERROR. Existing shader/material warnings remain outside
these narrow zero-ERROR claims. All evidence is retained beside earlier failed
runs; successful replacements do not explain every earlier timeout.

The exact Streamline source at commit
`e8aaa6eaac968711fb62473d4ae8256dde20919b`, `source/core/sl.api/sl.cpp:1125`,
was refreshed and retained as `streamline-e8aaa6-sl-api.cpp` (SHA256
`665bb44d9f41b00609c26a9cb5117abb85a803033e417509cf97efcb945d9f1b`).
`slGetNewFrameToken` advances a modulo ring at line 1141; the retained source
does not include its size definition. Supplying an old
counter does not pin a token. Step 4 limits admitted outer CPU iteration owners
to two, with FIFO retirement after their queued draws. This is separate from
GPU resource retirement and does not establish worker recording.

The named independent Step 4 proof-auditor could not be spawned because the
harness reports `agent thread limit reached`; its verdict remains pending.
This does not block independent Step 5 implementation. No automated tests were
run or added. Steps 5-6 and conditional async compute remain incomplete.


## Step 5 intermediate preparation build

Incomplete build02 compiles ordinary editor/console successfully
(`step5-editor-build02.log`, 34.72 seconds). Its five held source files and exact
working patch are retained in `step5-build02-source.json` and `step5-build02.patch`
at base `999bbd18cc`. Ordinary pinned binary SHA256 is
`1095cdbdb568b818132423e41502d77e92d7b05a24c2a1eacff652fac72fd16c`.
This snapshot includes FC lists/host payload, cull intents and initial RT
resource discovery/parallel assembly. It does not complete Step 5.

`step5-build02-dense-smoke` runs 300 orbit iterations on the real Vulkan dense
fixture and exits 0 without timeout or ERROR. Last telemetry confirms 10000
instances, two assets, three shared cuts/BLAS, eight resident pages and zero
pending pages/build delta. Shader caches are cold at startup. The last reported
RT gather CPU is 11.957 ms, raster preparation 4.041 ms and main admission wait
35.751 ms. These are short-run reported means, not a matched speedup or a
controlled regression against the longer Step 4 orbit. No performance acceptance
is claimed; warm comparison and remaining preparation work are outstanding.


### Existing page worker boundary, source inspection

Read-only inspection at `51e3d39392` confirms `_read_page()` in
`micro_geometry_storage.cpp:39` already executes as a native worker task (`:666`).
`MicroGeometryData::read_page():411` performs decompression and index/identity
validation there. `read_encoded_page():389` serializes shared FileAccess seek/read
under the source mutex, with SHA validation/decompression outside that lock.
The remaining host payload in `_build_page_clas():137` constructs per-cluster
TriangleInfo and build totals on the owner. The Step 5 author received this
concrete extension point; no replacement streaming scheduler is needed.

ReadTask retains its source Ref independently of Asset lifetime. `release():425`
invalidates the full Asset RID; update consumes or discards completed results
and joins/deletes the task. The destructor at `:767` joins all reads before
releasing assets/pool. Extended page payload must preserve these boundaries
and keep worker jobs independent of GPU/shader resources. Navigation used
clangd find/refs, actual sources and scoped history; no new runtime claim.


Intermediate build04 passes ordinary editor/console compilation in 27.91 seconds.
Its seven-source held snapshot (`step5-build04-source.json`, `step5-build04.patch`)
includes additional RT host preparation and canvas batch payload jobs, before
canvas tree-cull extraction. Pinned ordinary SHA256 is
`49fa6e6653911f1ce297383624b90b5b99cfd4e4755a36c4a0f4e3fe370503db`.
`step5-build04-gallery-smoke` runs 300 native Vulkan gallery iterations with
hybrid NRD at 1280x720 and exits 0 without timeout or ERROR. No visual quality,
final parallelism or speedup acceptance is claimed from this bounded run.


Intermediate build07 passes ordinary editor/console compilation in 36.67 seconds.
Its thirteen-source held snapshot is retained in `step5-build07-source.json`
and `step5-build07.patch`; ordinary SHA256 is
`37afc6293c10b4ccbea5471de8377c7f937d597fc0c6cd3c7005892182e48530`.
`step5-build07-micro-smoke` runs 300 native moving microgeometry iterations and
exits 0 without timeout or ERROR, exercising the new canvas/page payload paths.
The existing warnings remain; no final Step 5 acceptance is claimed.

The owner subsequently requested live dense-scene counters. The HUD now updates
FPS/wall-frame milliseconds and completed viewport CPU/GPU milliseconds at 4 Hz,
in still and orbit modes. CPU is labelled as rendering time including waits,
not main active CPU. No population scan or new engine API is added. Source inputs
and patch are `step5-dense-hud-inputs.json` and `step5-dense-hud.patch`.
`step5-dense-hud-capture` uses pinned build07, 600 still iterations at fixed delta,
and exits 0 without timeout or ERROR; PNG save returns 0. Actual PNG inspection
confirms readable, non-overlapping counters and the 5000+5000 population label.
The visible 32.7 FPS / 30.54 ms frame / 28.80 ms CPU / 8.42 ms GPU is an
illustrative intermediate sample, not final performance acceptance.

`step5-baseline-fixed-dense` completed 1500 orbit iterations on final Step 4
without ERROR or timeout, using `--fixed-fps 60 --print-fps --gpu-profile`.
This controls the trajectory's simulation delta but predates the new HUD;
final matched comparison must use the same HUD inputs on both pinned versions.


HUD checkpoint is `1f339891f9`. The refreshed same-HUD Step 4 baseline
`step5-baseline-hud-fixed-dense` completes 1500 fixed-delta orbit iterations,
exit 0 without ERROR/timeout. Its last-ten medians are main transfer 0.037 ms,
admission/callback active 0.012 ms, admission wait 28.753 ms, current render wall
26.9185 ms, RT gather window mean 7.1057 ms, raster preparation window mean
4.4077 ms, GPU last-frame sample 9.7725 ms and reported FPS 34. The metrics JSON
keeps these differing timing semantics separate. The final comparison below
supersedes the pending status of this baseline.

Build10 ordinary SHA256
`7196104c64918d1e5fb9b34b6326615ea82b867ff5848ef32802855dfa1b2023`
and its thirteen-source manifest/patch retain the initial camera/RT/shadow
scheduling boundary. `step5-build10-dense-concurrency` completes 300 fixed-delta
orbit iterations, exit 0 without ERROR/timeout. All 10000 instances remain
admitted, with two assets, 2-3 shared cuts/BLAS and zero page-build delta at the
last reports. This is bounded execution, not measured cross-pass overlap.

Build12 adds sampled native worker rows; ordinary SHA256 is
`67f16c34df3211c64a0c512415939734e3a5fae76fc6506e61c6a106efa16a57`.
`step5-build12-worker-windows` completes 600 fixed-delta orbit iterations with
profiling, exit 0 without ERROR/timeout. Its 855 sampled rows show worker IDs
different from coordinator 7 throughout. Peak overlapping elapsed job intervals
are 40 for RT discovery, 40 for RT assembly, 19 for raster lists and 22 for cull.
These are elapsed intervals including possible preemption, not counts of cores
continuously executing or summed active CPU. The tiny HUD has single-interval
canvas batches. `step5-build12-worker-windows-observations.json` retains stage
counts and worker identities. Same-frame sampled camera-list versus RT discovery/
assembly intervals do not overlap; source scheduling alone does not close this
runtime observation. Dirty-record, viewport canvas and selector host preparation
subsequently landed in the candidate below; performance acceptance remains open.


## Step 5 candidate: performance regression and source correction required

Frozen engine candidate is `8563b9a8e759e4469df0ed67b6de7716b688b8cd`,
with cumulative baseline `9f42b239e3e28267470a263b201d9b3b42f00435`.
Ordinary build18, double build01 and template-debug build01 complete successfully
(30.62, 63.21 and 55.49 seconds). The retained `step5-final-source.json` records
raw built-source hashes; `step5-final-commit-map.json` maps all sixteen files to
Git blobs after LF normalization. Final ordinary/double/template binary SHA256:

- `55f54824bbe6395ac5ee64d4d3858a435b4b7e7ce11fff48ed976a7fcf69dd83`
- `39ee649f7535ab34af90426a021742a9f896a914ff1997098fb8b6e6ef92a00c`
- `f883b851d53c80c562890251734a01271b3e0d93fb93002d4c2599a2ad0b52e9`

`step5-final-hud-fixed-dense` completes 1500 native Vulkan orbit iterations,
exit 0 without ERROR or timeout. It uses the same HUD, 1280x720 resolution,
no-vsync, fixed simulation delta 60 and profiling arguments as the retained
Step 4 baseline. Reported last-ten medians are:

| Measurement | Step 4 baseline | Step 5 candidate |
| --- | ---: | ---: |
| Main transfer window mean, ms | 0.037 | 0.024 |
| Admission/callback active window mean, ms | 0.012 | 0.0095 |
| Admission wait window mean, ms | 28.753 | 35.968 |
| Main process/navigation window mean, ms | 0.1495 | 0.1215 |
| RT gather window mean, ms | 7.1057 | 11.3081 |
| Raster preparation window mean, ms | 4.4077 | 4.2709 |
| Current render wall sample, ms | 26.9185 | 34.6465 |
| GPU last-frame sample, ms | 9.7725 | 7.1445 |
| Reported FPS window | 34 | 27.5 |

These are not frame percentiles or summed active worker CPU. GPU samples do
not establish an isolated GPU gain. The CPU/frame regression prevents accepting
Step 5 merely because preparation now executes on workers.

A matched second pair removes only `--gpu-profile`, retaining the HUD viewport
timestamps, `--print-fps`, fixed delta and all other scene/run arguments.
`step5-baseline-hud-fixed-dense-no-profile` and
`step5-final-hud-fixed-dense-no-profile` both exit 0 without ERROR or timeout.
Median of their last ten reported FPS windows is **40 versus 29**. Thus detailed
profiling overhead alone does not explain the regression. Exact samples and
receipt outcomes are retained in `step5-no-profile-comparison.json`, alongside
both logs and receipts. This pair does not provide unprofiled stage CPU timings.

Fresh source review round 1 by `repair_step5_review1` returns **REJECT** on the
frozen exact/cumulative range. Conventional light/decal gathering, sorting and
host packing remain on the render coordinator: the active camera call at
`render_forward_clustered.cpp:2759` reaches `_pre_opaque_render` and synchronous
storage helpers at `:2340-2341`; `light_storage.cpp:768,986,1003` and
`texture_storage.cpp:4270` contain the preparation loops. Move their CPU work to
workers while retaining resource resolution, publication and uploads at their
owner. No separate concrete race/lifetime defect was established by that review.
This omission and the measured regression remain unresolved; they are not
asserted to have the same cause.

The HUD itself is committed at `1f339891f9` and has the native visual evidence
above. Its independent named proof audit, Step 4 proof audit and final Step 5
proof audit remain pending. The harness rejects both resuming the implementation
author and spawning a fresh correction context with `agent thread limit reached`,
even after source review completes. No inline threading rewrite bypasses the
required separate-context implementation rule. No automated tests ran.
Step 6 worker command recording has not started; Step 7 async compute remains
conditional on measured useful overlap after Step 6.


## Node/SceneTree bottleneck check (2026-09-10)

The owner asked whether the regression belongs to the separately planned scene
architecture replacement. Re-reading both retained native logs gives last-ten
window-mean medians of simulation 0.0385/0.136 ms and process/navigation
0.1495/0.1215 ms for Step 4/Step 5. Admission wait is separately
28.753/35.968 ms. `Main::iteration` (`main/main.cpp:4994`) starts the simulation
clock after frame admission and encloses MainLoop process plus message queue
flush in the process/navigation interval. These measured main-thread intervals
do not support Node/SceneTree processing as the dominant steady-state bottleneck.
The dense script creates and counts the 10000 MeshInstance3D nodes only in
`_ready`; `_process` moves the camera and samples the HUD, without an instance
loop. The compared engine range changes no `main/`, `scene/` or `core/` files.

This evidence identifies a renderer-side regression under the same Node load;
it does not isolate the expensive worker function or prove that all Godot
renderer data structures are efficient. Renderer instance gathering/culling is
separate from SceneTree processing, and replacing Nodes alone does not establish
that those renderer costs disappear. No Node/SceneTree rewrite is warranted by
these measurements. No new runtime launch or Node-free fixture was needed for
this bounded check; conclusions use the retained matched runs, actual timing
boundaries and current fixture source. Navigation used clang-nav for
Main::iteration, direct source reads, scoped Git diff and raw log recalculation.


## Step 4 independent proof closure (2026-09-10)

Fresh named `step4_evidence_audit` returns PASS on the bounded final Step 4
proof at `9f42b239e3`, baseline `e319cca724`. Exact/cumulative patches match Git;
all 22 source identities reconcile to frozen blobs, including three mixed-line-
ending files. All 23 pinned executables/DLLs match their manifest. The six final
native receipts and logs verify ordinary Vulkan completion without ERROR or
timeout, 5000+5000 dense population, SR/RR capture callbacks, bare editor and
no-draw execution. The auditor independently recomputes all six reported dense
medians and finds positive main/render overlap in six windows. Double/template
proof remains compile-only; PNG appearance and exhaustive nesting-depth coverage
are not claimed. Earlier timeout and zero-exit ownership errors remain preserved.
The SDK ring's exact size is unverified because its definition is absent from
the retained external source; modulo recycling and the local two-owner bound
are independently supported. This closes the previously harness-blocked Step 4
proof audit, not Step 5/6 architecture or performance acceptance.


## Step 5 focused regression experiments (2026-09-10)

Retained timestamp-window comparison localizes the largest Step 5 changes to
RT Scene Gather (+4.2024 ms), RT Decals and Lights (+1.0976 ms), CanvasItem tree
cull (+0.6247 ms) and canvas rendering (+0.4855 ms). These are last-ten medians
of reported CPU window means. `step5-stage-regression-comparison.json` retains
all common stages. Sampled native worker rows show RT discovery median
queue-to-join 6540 us, execution span 6333.5 us and longest chunk 6241.5 us;
most of its elapsed interval is inside worker execution, not merely initial
queueing. Elapsed intervals include preemption and do not prove a mutex cause.

The first focused patch retains geometry sources/readiness once per distinct
asset in each existing discovery batch and avoids copying the retained source
Ref in assembly. It preserves task scheduling. Ordinary build passes in
33.85 seconds; held-source manifest/patch are `step5-refcount-fix01-source.json`
and `step5-refcount-fix01.patch`. Pinned ordinary SHA256:
`2e13b2aae6335121fccaf2495f2c71d2705b9efdf8b65adef19983f72844e99a`.
Native same-HUD fixed60/orbit1500 no-profile and profile runs both exit 0 without
ERROR or timeout. No-profile last-ten reported FPS median remains 29. Profiled
FPS is 23.5 and RT gather 13.6341 ms. This experiment does not demonstrate a
performance improvement; the shared-Ref hypothesis alone is insufficient.
A current-session baseline refresh is required before attributing variation
against the prior profiled run. No automated tests ran.


The current-session baseline refresh `step5-baseline-refresh-no-profile` exits
0 without ERROR/timeout but falls to 25 FPS. A contemporaneous host sample finds
many active external `cl.exe` processes; neither this task's author nor root was
building then. This run is confounded and cannot establish an engine regression
or gain. Reference-only experiment comparisons across that interval likewise
cannot isolate shared-reference cost. No external process was stopped.

Final correction `9f6512180c30254fa6482a7e407a02cb979cf0db` additionally resolves
mesh/surface material once per existing discovery batch and reuses the merged
result during assembly. `MeshStorage::owns_mesh` enters the thread-safe RID
owner mutex; repeated per-instance calls were a concrete contention source in
the parallel traversal. Conventional light/decal gather, sort, packing and
native ClusterBuilder payload hooks now execute in existing sequential worker
jobs; owner publication and GPU uploads follow their joins. No persistent cache,
new storage authority or public API is added. These changes are only three
source files; the full Step 5 cumulative range includes eighteen engine files.

Final03 ordinary/double/template builds pass in 30.85/26.70/22.39 seconds.
Pinned executables in `step5-resource-fix03-bin` have SHA256:

- Ordinary: `32d7cffe49f6952d205c28588b0e87436e74d49a844f17e9fc3e0e159d92dc9c`
- Double: `8671a0f37ff1c06293fbc373edba06fcd31ac7da895be372a173dd5b73f29662`
- Template: `ddabe9f2701f0c0cb9622dab53873b410ea4b1c31f0e9b904ead24b627aa5632`

`step5-resource-fix03-source.json`, `-additional-binaries.json`, `-commit-map.json`
and the retained working patch establish all eighteen source identities against
frozen `9f6512180c` after LF normalization. The eight runtime text inputs match
previous Step 4 inputs plus the intentional HUD checkpoint; see
`step5-fix02-runtime-inputs.json`. Intermediate fix02 compiled but was superseded
by the native cluster-hook closure before its own runtime launch.

After external compilers finish, a short CPU sample shows no busy compiler or
clangd process. `step5-baseline-quiet-no-profile` and
`step5-resource-fix03-quiet-no-profile` then run sequentially with identical HUD,
fixed60/orbit1500, Vulkan1280x720 and no-vsync settings. Both exit 0 without ERROR
or timeout. Last-ten reported FPS medians are **41 versus 37**. Start/mid/end
checks find no `cl`/`link` processes; these observations do not guarantee continuous
exclusive host use. The prior candidate reported 29 FPS, but this final pair
still demonstrates remaining loss against Step 4, not completed performance
acceptance. Samples are in `step5-resource-fix03-quiet-comparison.json`.

Final03 detailed profile also exits 0 without ERROR/timeout. Last-ten medians:
RT gather 5.3382 ms; RT decals/lights 1.2748 ms; canvas cull 0.6611 ms; canvas
render 0.5179 ms; microgeometry raster preparation 4.2714 ms; reported FPS 33.
Main simulation 0.042 ms, process/navigation 0.1045 ms, transfer 0.020 ms and
admission/callback active 0.0105 ms remain small; admission wait is 30.024 ms.
These are reported window means, not frame percentiles or summed active worker
CPU. `step5-resource-fix03-profile-metrics.json` retains all stage medians.
The final gallery run (300 fixed-delta native hybrid-NRD iterations) exits 0
without ERROR/timeout, exercising existing Omni/Spot/Area lights. It overlaps
read-only reviewer navigation and supplies execution evidence only, no performance
or appearance claim. No nonempty decal workload was found in the existing manual
text-scene inventory; that runtime branch remains unverified.


Final source round 2 by `step5_review2` returns **REJECT** on frozen
`9f6512180c`, original `8563b9a8e7`, cumulative `9f42b239e3..9f6512180c`.
One concrete scope omission remains: `render_raytracing.cpp:4502` calls
`TextureStorage::build_rt_decal_snapshot` synchronously; its body at
`texture_storage.cpp:4314` gathers camera/resident decals, performs three sorts,
packs data and hashes payload/texture generations on the render coordinator.
The active caller is `render_forward_clustered.cpp:2749`. Move that snapshot
preparation into a joined worker job before owner publication/uploads, preserving
ordering, offscreen residency and generation semantics. The conventional decal
buffer correction does not cover this separate RT path. No additional concrete
race/lifetime defect was established. The dense fixture has no decals, so this
omission is not asserted to cause the remaining 37-versus-41 FPS loss.

The mandatory two-source-review-round cap is reached. Source work stops here;
there is no third review or silent follow-up fix. Step 5 remains incomplete:
RT decal worker preparation, remaining performance diagnosis and final named
proof audit are outstanding. Step 6 recording and conditional Step 7 async
compute have not started. The next authorized continuation should address the
specific snapshot call above, retain the final18source/binary evidence and use
the quiet matched baseline; do not rewrite SceneTree or add a new renderer layer.


## Renewed measured closure (2026-09-10)

The owner explicitly resumes after the two-round stop and requests more counters
rather than repeated speculative optimizations. Renewed task baseline is
`daa4003f7e71aec873e6f6c4e80f053b24f381b3`. The existing sampled RenderPrep
mechanism is extended in four engine implementation files; no second profiler,
public setting, per-instance printing or automated test is added. Records use
frame numbers, work/job counts and queued/begin/end/join times, plus bounded
resource-resolution/publication/upload intervals. Payload spans are elapsed
including preemption, not summed active CPU. The only initial behavior change
moves nonempty RT decal snapshot preparation to a joined worker; empty snapshot
semantics remain on the existing constant-time path without dispatching a job.

Initial detail01 build fails because PagedArray has size(), not is_empty(); the
one-line API correction is verified against its actual header. Detail02 ordinary
build passes in 35.33 seconds. Its held-source manifest and patch are
`step5-detail02-source.json` and `step5-detail02.patch`; pinned ordinary SHA256
is `c82a9e6276dfbd6f7a547af6e2efa1a7745289192a62101eca9301a5453abe12`.
`step5-detail02-profile` completes 1500 fixed60/orbit native Vulkan iterations,
exit 0 without Godot ERROR or timeout. A parallel one-second process telemetry
capture (`step5-detail02-host.jsonl`) records 145 samples: no cl/link process,
and zero cumulative CPU increase for the four observed clangd processes. This
rules out those observed compiler processes as this run's confounder, not all
possible host interference.

Across twelve sampled frames, the detailed rows establish:

| Measured stage | Work | Elapsed observation |
| --- | --- | --- |
| Geometry motion preparation | 3 jobs/frame, each scans 10001 instances, dirty=0 | Median payload 189 us; containing owner invocation 223 us |
| RT light registry | 5 callback units in 4 group submissions; one analytic light, zero emissive sources, two resulting lights | Owner 389.5 us; analytic payload 7 us, empty emissive 0 us, merge 3 us, history 6.5 us |
| Canvas tree preparation | 2 invocations/frame, one empty and one with two roots; 3 jobs each | Owner median 308.5 us, payload span 23.5 us |
| Canvas batch preparation | 3 jobs for two items / 310 commands | Owner 380 us; payload 42.5 us and merge 36.5 us |
| RT decal snapshot | Zero camera/resident decals | 0 jobs, 1 us, no upload |

These are medians per invocation, not sums of overlapping work or frame
percentiles. `step5-detail02-summary.json` retains sample counts and per-frame
invocation counts. Root source inspection also corrects interpretation of the
coarse RT Decals and Lights timestamp: it extends through the build_tlas tail,
camera-list completion/sort/payload and DDGI preparation until Setup Shadows.
Its entire 1.27 ms must not be attributed to the 389.5-us light-registry interval.

The resulting bounded changes selected for implementation are one motion-list
aging traversal per frame with same-frame insertion invalidation and independent
dirty drains; skipping zero-root canvas tree jobs while retaining the downstream
renderer call; merging one-chunk results on that existing owning worker; and
coalescing dependent RT registry phases while retaining parallel payload workers
when both independent inputs are nonempty. No SceneTree rewrite, threshold
solver, DDGI redesign or unrelated optimization is selected. Final builds,
matched measurements and a fresh review of this renewed closure remain pending.

Named `dense_hud_proof` independently returns PASS for HUD commit `1f339891f9`
and its exact three-file task range. It verifies actual population, binary/input
identity, native receipt, metric semantics and readable PNG SHA256
`8ce5d4a8519130f53f42ccaa7e60ae6b2dee0bb67dc3dc9ebdd9a535ada6372a`.
The artifact proves one still sample; cadence/orbit behavior is source-backed,
not established by a static PNG. The capture log has zero Godot ERROR lines but
32 lowercase SPIR-V Parsing error diagnostics, so it is not warning/error-text
clean. This closes the HUD-specific proof audit, not final Step 5 performance.

The measured batched01 candidate builds ordinary editor in 34.16 seconds and
double editor in 33.04 seconds. Its ordinary executable SHA256 is
`b34644cb5cf374014fcc2d8abd3f10b9ff43d5b6b1315bf2cfaee726ee71660b`;
`step5-batched01-source.json` and `step5-batched01.patch` retain its held source.
The final profile completes 1500 fixed60/orbit Vulkan iterations, exit 0 without
Godot ERROR or timeout. Twelve sampled frames confirm one actual 10001-instance
motion traversal per frame instead of three; the other two invocations retain
independent dirty processing. Empty canvas cull now dispatches zero jobs
(median 0.5 us), while the two-root invocation uses two jobs (median 232 us).
Canvas batching uses two jobs and has median owner elapsed 212.5 us, versus
380 us previously, with unchanged 310 commands, 310 instances, eight batches
and 39680 uploaded bytes. The fused payload span includes its merge; these
nested spans must not be added together.

RT light registry now uses two callback jobs for this input, with median owner
elapsed 90 us versus 389.5 us. The actual registry callback spans 12.5 us;
environment and upload remain owner work at medians 5 and 11 us. Phase rows
inside the callback are nested intervals, not separate scheduler waits.
Empty RT decal snapshot remains zero jobs (median 1.5 us). Nonempty RT decal
snapshot worker execution is source-backed only: no existing bounded native
fixture exercises a nonempty RT decal population. Counter aggregates are retained
in `step5-detail02-summary.json` and `step5-batched01-summary.json`.

All three matched no-profile dense runs finish normally without Godot ERROR or
timeout. Last-ten FPS medians are 34 for `step5-detail02-control-no-profile`,
38 for `step5-batched01-no-profile`, and 34 for the candidate repeat
`step5-batched01-warm-no-profile`. The repeat does not establish a durable
whole-frame gain. The first control/candidate runs contain isolated 13/3-FPS
windows despite settled page/CLAS counts; their cause is unproven. Structural
work reductions above are established, but total performance is not
declared fixed and no stutter is attributed to SceneTree or shader compilation.

The independent proof audit finds cl/link processes during the final candidate
profile interval (06:25:53.732-06:27:29.187 UTC). The available profile monitor
starts at 06:26:48.749, so it does not cover the entire run. Four compiler/linker
processes appear only in the last in-run sample at 06:27:28.562; their combined
cumulative CPU is 0.796875 seconds, but one sample cannot determine CPU earned
inside the native interval. Final local elapsed numbers are observations with
unexcluded host interference, not isolated speedup proof. Work counts and source
identity remain valid. The later root double and
template builds finish at 06:34:45 and 06:35:29 UTC and do not explain those
earlier compiler processes. Their workload is unidentified. Control/candidate
no-profile telemetry covers almost the full runs (88/85 samples) without compiler
processes or observed clangd CPU increase; warm-repeat telemetry covers only its
last approximately 43 seconds, which are quiet. The candidate profile contains
32 lowercase SPIR-V Parsing error diagnostics and zero Godot ERROR lines; normal
exit is not a claim that all diagnostics are absent.

The five-file candidate is committed at
`617a3abca7b27832ef2e791a0949a8017658402c`. All eighteen held Step 5 source hashes
match the commit after LF normalization and the built working bytes exactly;
`step5-batched01-commit-map.json` records both comparisons. Template build passes
in 26.02 seconds. Pinned double/template executable SHA256 values are
`87add8709fdbd8257d5e67eda90d89323657f182707e985f74bf7e3addc08c5b` and
`4cd181ac8277ed386818271b979eee07e9175d60a052d21d6b0170d3b892f50d`;
`step5-batched01-binaries.json` records the retained binary/DLL set. Double and
template are build-only evidence. Ordinary native `step5-batched01-moving` and
`step5-batched01-gallery` each complete 300 fixed60 iterations on the existing
microgeometry orbit and hybrid-NRD gallery scenes, exit 0 without Godot ERROR
or timeout. No appearance or nonempty-decal execution claim follows from these
launches. Fresh `step5_measured_review1` and named `step5_measured_proof` are
reviewing the frozen source and evidence independently.

Step 6 is delegated from the same source revision, reusing the existing graph
recording research. Its scope remains actual isolated frontend production,
worker graph compilation and draw/compute/RT driver recording, with one graph
barrier authority and joined CPU jobs before command-pool reuse. No Step 6
implementation, build, overlap or performance acceptance is claimed yet.

Renewed source round 1 returns REJECT for a concrete moved-call boundary:
`render_raytracing.cpp:4512` executes `build_rt_decal_snapshot` on a worker, but
the snapshot generation loop at `texture_storage.cpp:4384` calls
`texture_get_content_generation`, which reaches render-thread-guarded RD texture
validity/content-generation APIs. A valid textured decal therefore emits thread
errors and hashes zero instead of live texture content generation. The named fix
keeps CPU sorting/packing on the worker and resolves only RD-dependent texture
generations on the render owner. RD guards remain intact. No nonempty-decal
runtime result was claimed by the earlier zero-decal captures.

Named `step5_measured_proof` returns PASS for bounded frozen `617a3abca7`
evidence: eighteen source identities, exact patch, 26 candidate/22 control
binary artifacts, eight runtime inputs, seven native receipts, actual job/count
reductions and overlapping preparation intervals on 47 distinct workers.
This does not accept the source defect, prove active CPU utilization or establish
whole-frame improvement. Moving/gallery logs contain 40/48 lowercase SPIR-V
diagnostics respectively, despite zero Godot ERROR and normal exit. The source
fix and refreshed build evidence remain pending; renewed round 2 will review
the correction and cumulative task range.

Final-profile main-thread counters reinforce the earlier SceneTree boundary.
Medians of the last ten reported window means are simulation 0.0375 ms,
process/navigation 0.108 ms, transfer 0.007 ms, script/audio tail 0.004 ms and
admission/callback active 0.011 ms, versus admission wait 32.4515 ms. These are
elapsed profiler observations with the host limits above, not active CPU samples
or frame percentiles. The wait must not be described as 32 ms of Node logic.
`step5-batched01-main-summary.json` retains the extraction; remaining coarse
renderer intervals are separately recorded in `step5-batched01-coarse-summary.json`.
Startup-only one/two-sample entries in that file are not steady-state costs.

The first decal-owner correction builds ordinary editor in 50.72 seconds but
is superseded before runtime/commit: author source closure finds the same guarded
texture-content-generation query in the analytic-light worker for textured
projector/area inputs. The same round-one correction therefore also resolves
those generations in the existing owner resource phase before worker consumption.
Default untextured lights do not exercise that query. This is a known source
defect being corrected, not an inference from the zero-error gallery launch.

Correction `030fa486b560436a86ad622973407f3b08520841` changes only RT code and
TextureStorage source/header (26 additions, seven deletions). CPU decal byte
hashing and ordered texture/atlas inputs stay on the worker; guarded generations
are folded on the owner after joining, in the prior order. Analytic workers read
an immutable map resolved in the existing owner resource phase. No RD guard,
bound API, shader, backend or persistent cache changes. Final02 ordinary/double/
template builds pass in 31.58/52.03/41.30 seconds. All nineteen held source LF
hashes match the commit; `step5-decal-owner02-source.json`, `.patch`,
`-commit-map.json` and `-binaries.json` retain source/build identity. Ordinary,
double and template executable SHA256 values are respectively
`7e1e54d29bce2699a4ad2e514095bff4b1c2860e5b744858e57088396eee5d59`,
`6224ddb607268f7e1f5d6077cb89c1963184d4cc650956288b45ec40e320c8fc`, and
`cac978f70014b5894362fd3d873ca60e08c1478cb3cd910222c667be637ac704`.
Fresh final source round 2 and refreshed native checks are pending. These native
checks are for current-binary execution, not a new matched speedup comparison;
parallel source research is allowed and its load is not controlled.

Final02 dense600, moving300 and gallery300 complete on the pinned ordinary
binary, each native/wrapper exit 0 without timeout or Godot ERROR. Logs retain
32/40/48 lowercase SPIR-V parsing diagnostics. Named `step5_measured_proof`
refreshes its bounded PASS at `030fa486b5`, verifying all nineteen source files,
26 binary artifacts, eight runtime inputs and the three current receipts.
The final dense capture retains the same 10000 native instances, 47 distinct
preparation workers and actual structural reductions at sampled frames
120/240/360/480/600. Nonempty textured-decal and textured projector/area runtime
branches remain unverified. Earlier timing comparisons belong to `617a3abca7`;
the final correction has no new matched performance or appearance claim.

Fresh final `step5_measured_review2` returns PASS at `030fa486b5`, covering the
exact correction, renewed `daa4003f7e..030fa486b5` range and full Step 5
`9f42b239e3..030fa486b5` range. It verifies worker/owner generation-query
separation, hash ordering, analytic discovery coverage, task-local lifetimes,
deterministic merges, motion invalidation, canvas ordering and removal of old
state. Step 5 CPU preparation is closed with final source and bounded proof
PASS. Whole-frame performance remains unresolved; Step 6 actual frontend/driver
recording is in progress and conditional Step 7 async compute has not started.
The existing recording research is reused. A separate read-only source check
maps the remaining coarse microgeometry prepare intervals to actual CPU work
and missing counter boundaries; it does not reopen SceneTree or paging design.

The [remaining preparation cost map](2026-09-10-0706-microgeometry-prepare-cost-summary.md)
closes that source question at frozen `030fa486b5`: RT's retained path still
hashes per-instance inputs in two immediately joined workers, while raster builds
discovery/task/bin/snapshot data before its reuse branch. Neither quoted interval
establishes driver recording or CLAS build cost. A separate instrumentation task
from `6a797920ce` adds sampled phase/wait-entry/work-count rows in
`render_raytracing.cpp` and `render_forward_clustered.cpp`; it does not yet replace
hashes with generations or change preparation behavior. Step 6 owns disjoint
RD/graph/driver source. Counter commit `bbf18999d0` passes fresh exact/cumulative
source review; it is included in the combined ordinary editor build. Native
counter overhead and normal performance results remain unverified.

Step 6 frontend/graph/driver recording remains uncommitted. Combined02 ordinary
build passes in 49.61 seconds, but pinned native dense600 exits with access
violation `3221225477` before a sampled counter row. The same source relinked
with a matching map under local CDB reaches frame 600 but times out at 150
seconds without normal exit; no second-chance crash stack is obtained. The
configured exception-command echo is not evidence of an executed exception.
These are diagnostic failures, not performance acceptance. Retained evidence
is `step6-combined02-source.json`, `-binaries.json`, `-dense.receipt.json`,
`-map-build.log` and `-cdb02-receipt.json` under the existing evidence root.

Debugger frame120 shows RT full-record hashing 3495 us for 5.44 MB, with retained
RT and raster resources, raster discovery 1601 us and two source-cache misses
for 10000 eligible surfaces. Debugger overhead and incomplete exit prevent
native performance claims. The new rows separate these repeated CPU traversals
from CLAS construction.

The author corrects deferred CPU tracker lifetime, captured attachment state
and dynamic offsets, owner swapchain acquisition, and shutdown task admission.
WorkerThreadPool enters language-exit state before final renderer destruction;
late posting through its existing blocking API can stall. A narrow internal
atomic admission operation rejects such work before allocation, allowing the
same graph callbacks to finish on the current render owner during teardown.
Existing public task APIs retain their behavior. Compile06 holds eight files
including WorkerThreadPool source/header; fresh native validation is pending.


Combined04 ordinary06 editor/console build passes in 143.26 seconds. All eight
author-held source hashes match the 27-file combined source manifest; executable
SHA256 is `d6656ad9d1eb83f34ff9099b5f3aa51356d5fc32351f243e32bc3e03399dd3a9`.
Pinned dense600 completes normally with native/wrapper exit 0, UTC
2026-09-10 07:43:09.991 to 07:44:15.755. Zero Godot ERROR; SPIR-V diagnostics remain (32 explicit parsing-error
lines and 50 unsupported-opcode messages, including six ForwardPointer messages). This replaces the earlier crashing/timed-out runtime
boundary for the held source, not a matched whole-frame improvement claim.

Four sampled frames 240/360/480/600 give medians: RT preparation 5853 us,
full-record hashing 3466 us, earlier settings/record hashing 1381 us and nonempty camera raster
preparation 4222 us (four camera rows; empty shadow rows are excluded). Resources are retained while input signatures change with
the orbit. Graph compile payload median is 252 us; recording coordinator elapsed
median is 783.5 us, including waits, not active owner CPU. Distinct-worker
frontend and recording intervals overlap; device frame IDs remain separate from
renderer frame IDs. `step6-combined04-counter-summary.json` retains extraction.
The full-run host monitor has 63 in-run samples: four observed clangd processes
show zero CPU delta; no compiler/linker process is observed during that interval.
This does not measure other system/GPU load. Double/template builds pass in 131.14/75.61 seconds. Step 6 is committed as
`caa122b6519b1108e7ac64504abd4152c116bece`, eight files, 1252 additions and
340 deletions. All 27 held source LF hashes match that commit; the pin includes
26 artifacts. Moving300 and gallery300 also finish normally. Fresh exact and
cumulative source review and named proof audit are active; remaining native
lifecycle coverage and matched performance acceptance remain open.


Frozen04 freeze-after300 and bare `-e` startup120 also exit normally without
Godot ERROR. All five receipts use the same pinned executable; ordinary startup
uses default backend selection. Current fixture readback matches the prior
eight-input map, but no separate at-launch fixture hash map was captured for04.
These runs establish bounded execution and shutdown, not exhaustive resize,
reload or cancellation coverage.

Owner clarification on 2026-09-10: acceptance is actual game speed at unchanged
scene/settings. Artifact and counter audits only bound measurement claims; PASS
is never a speedup. Next measurement priority is profile-disabled before/after
for the measured full-record traversal, keeping CPU/GPU/frame costs separate.
A scoped implementation owns only RT source/header and retains live camera,
residency and dependency checks. No gain is claimed before that comparison.


Named `step6_recording_proof` returns bounded PASS for source/binary identity,
actual overlapping worker recording, five native exits and sampled counters.
It explicitly rejects using those observations as a speedup: a slower renderer
could pass them. Matched no-profile game timing remains the performance gate.


## Replacement direction, 2026-09-10 08:07 UTC

The owner requests a new repair plan: eliminate recurring CPU microgeometry
scene/task reconstruction rather than chiefly distributing its scans across
workers. Reuse persistent instance/surface/material buffers, existing GPU
selection, indirect raster work, page streaming and shared CLAS/BLAS. CPU owns
changed-data ingestion, asynchronous I/O/uploads, resource lifetime and required
command submission. Then fan out all ready independent remaining CPU jobs and
wait only at actual consumers, avoiding serial enqueue-and-immediate-wait chains.

Owner explicitly removes proof-auditor/acceptance-audit workflows and permits at
most a brief code review. Build success, working native game and measured speed
at identical settings are acceptance; worker placement alone is not. Existing
scene is 5000 Lucy + 5000 Thai. No SceneTree/Flecs rewrite or unsupported modes.

Current source is `2108bbedf1701f1a27ed59511c4a898e8ec6e856`. Diagnostic fixes
`8c8833b87f` reserve breadcrumb slots per recording buffer in submission order
and balance debug labels. `2108bbedf1` reuses the full-record hash under explicit
generation validity. Combined ordinary build passes in 41.88 seconds; these
last corrections have not been run or measured. Previous Step 6 reviewer found
the two diagnostic issues; no follow-up audit is requested under the new owner
workflow. Do not claim final runtime/performance acceptance.

Bounded current-source research resolves the new plan's AS boundary: RT shader
mode15 already produces TLAS descriptors; CPU still rebuilds their task/layout
inputs. geometry_base/motion_base, TLAS custom_index/SBT offsets, materials and
hit-program lookups must share a stable index contract. tlas_build_from_buffer
accepts a GPU descriptor buffer but host instance count and BLAS dependency RIDs.
The draft uses admitted slots updated on membership changes and GPU inactive
descriptors; it does not assume GPU-count TLAS submission. Existing host cut
allocation/retirement and exact page pins remain. A brief plan-only sanity
review passes; the replacement plan is a draft pending owner approval.


The owner approves the replacement plan with "zaczynaj"; it is committed as
`8e316e180d` at [the new plan](../plans/2026-09-10-0807-gpu-microgeometry-cpu-removal-plan.md).
Step1 implementation owns changed-data registration/publication and the first
raster metadata consumer. No audit/proof agents are part of the new workflow.

Current ordinary `988a9708ec` (combined diagnostic fixes and hash reuse) runs
the same dense1500 orbit, Vulkan1280x720, no vsync and no detailed profiling.
`micro-delta-baseline01.log` ends normally with zero Godot ERROR. Last-ten FPS
window median is43, versus38 in `step6-baseline-noprofile01.log` on preceding
`d6656ad9d1`. Reciprocal frame times are23.26 vs26.32ms. This is one comparison
of the combined corrections, not isolation of one fix or a repeated steady-state
result. Current shader variant cache misses occurred during startup and are
excluded from the last-ten-window comparison. It is the new plan's baseline.


## CPU removal Step1, `dc7b4b2320`

Dirty publication retains eligible surface/task/bin/source metadata. Raster
discovery uses registered surfaces, and RT append consumes retained metadata.
The 19-word-per-task snapshot is removed in favor of structural publication
generation and bounded pass state. Ordinary transforms/camera do not invalidate
task structure. Asset readiness/deletion and existing GI pairing invalidate
through their owning change paths. Per-pass task/bin union and full CPU culler
inputs remain Step2; RT layout rebuilding remains Step3.

Identical dense1500 orbit no-profile comparison, process frames900-1500:

| Sample median | Previous | Step1 |
| --- | ---: | ---: |
| FPS |43.20|47.85|
| Wall frame ms |23.147|20.900|
| CPU render sample ms, includes waits |21.576|19.194|
| Completed GPU sample ms |7.130|6.099|

Logs `micro-step1-control.log`/`micro-step1-candidate.log`; both normal exit0 and
zero Godot ERROR, 55/49 quarter-second timing samples in the common frame range.
This is one paired result; GPU sample change is not attributed to a GPU
algorithm optimization. Existing HUD timings are printed only in verbose mode
by two-line scene change `cf07aa2993`; both runs use it. Moving300/freeze-after2
also exits0. Final existing-GI pairing invalidation is included in commit
`dc7b4b2320` and final ordinary build32.49s; that small final closure is compile
validated, not part of those native captures. Step2 implementation is active.

## CPU removal Step2, intermediate runtime regression

The uncommitted persistent raster/culling candidate builds, but is not accepted.
Matched dense1500 orbit runs `micro-step2-control.log` (Step1 final binary) and
`micro-step2-candidate.log` (Step2 candidate), process frames900-1500, report
median wall frame24.304/22.245ms, CPU render sample22.527/19.976ms and GPU
sample6.415/15.552ms. Both processes exit0, but the candidate emits five sparse
selection admission errors: about733MB requested against the existing512MiB
limit. Resident pages increase from8 to169 and resident CLAS from108 to2708.
These observations do not establish an acceptable optimization or identical
rendered workload. GPU shadow visibility/refinement and admission sizing are
being corrected before another comparison; scene quality and budgets remain
unchanged. The shared-cut RT instance count remains approximately10000.

The corrected short180 native run `micro-step2-caster-short.log` exits0 with
zero Godot ERROR and returns to8pages/108CLAS, with late GPU samples around6ms.
The correction publishes existing CPU caster planes as bounded pass data and
rejects invisible instances/groups on GPU before DAG refinement/page requests.
The same-settings full comparison is still pending; the short run establishes
closure of the observed early admission/residency regression only.

The full corrected-caster pair is clean but does not improve wall time:
24.343ms control versus24.675ms candidate. Short diagnostic420 then measures
raster preparation at approximately0.012ms CPU, RT gather5.39-5.61ms,
microgeometry RT preparation2.67-2.71ms and the enclosing TLAS Build interval
3.74-3.91ms. These profiled intervals include host work; they are not native
GPU TLAS cost. The retained selector still rebuilt capacity arrays and scanned
all units after feedback on every pass. Capacity publication now belongs to
successful resize; scans run only when feedback actually requests retry.

`micro-step2-capacity-candidate.log` on that correction ends normally with
zero Godot ERROR. Same scene/settings/route, frames900-1500,51 timing samples:
44.99FPS,22.227ms wall,19.894ms CPU render sample,6.816ms GPU sample. The
preceding `micro-step2-caster-control.log` Step1 run has56 samples:41.08FPS,
24.343ms wall,22.443ms CPU,6.351ms GPU. This is an8.7% shorter wall frame in
this comparison, without a claim of isolated GPU improvement. The intervening
caster candidate and diagnostic explain the correction; no build/heavy agent
work ran concurrently with these performance captures.

Step2 is committed as `671aa6cfc43b7e36b92733c88212d847ee15ad47` after ordinary
build27.10s and moving300/orbit/freeze-after2 native exit0 without Godot ERROR
(`micro-step2-moving.log`). It retains persistent camera/shadow batches and
routes conventional culling through a membership-maintained domain. The
temporary RT-only microgeometry gather remains deliberately until Step3
replaces its consumer. Step3 is now implementing persistent GPU RT inputs;
the remaining fan-out step follows that ownership handoff. No tests or audit
agents were run for this replacement workflow.

## CPU removal Step3, `f60eafa5275`

Persistent RT membership replaces recurring microgeometry gathering and full
record/task hashing. GPU mode15 writes retained geometry/material/motion/TLAS
prefix records; conventional geometry uploads only its suffix. Shared-cut
segments and allocations persist until their layout/capacity changes. CPU
publication handles actual changes, conventional records, cut representatives
and resource dependencies. The temporary RT-only microgeometry culler scan is
removed. Ordinary Windows editor/console build passes31.64s.

Same dense1500 orbit, Vulkan1280x720, vsync off, detailed profiler off; common
process frames900-1500:

| Sample median | Step2 control | Step3 |
| --- | ---: | ---: |
| FPS |44.34|114.48|
| Wall frame ms |22.552|8.735|
| CPU render sample ms, includes waits |20.298|6.303|
| Completed GPU sample ms |6.607|4.352|

Logs `micro-step3-control.log`/`micro-step3-candidate.log` contain51/21 sampled
windows respectively; both exit0 without Godot ERROR. Both retain10000 RT
instances,8resident pages and108CLAS; shared cuts reach3. This is one matched
route comparison, approximately61.3% less wall time, not isolation of each GPU
stage. The owner also observed approximately120FPS in the running scene.
Short dense180 and existing moving300/orbit/freeze-after2 runs exit0 without
Godot ERROR. No appearance/quality campaign or automated tests were run.
The remaining independent-job fan-out is now implementing from this commit.
