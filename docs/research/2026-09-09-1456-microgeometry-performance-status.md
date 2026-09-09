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
worker dispatch. RD recording remains Step 6.

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
`slGetNewFrameToken` advances the six-slot ring at line 1141; supplying an old
counter does not pin a token. Step 4 limits admitted outer CPU iteration owners
to two, with FIFO retirement after their queued draws. This is separate from
GPU resource retirement and does not establish worker recording.

The named independent Step 4 proof-auditor could not be spawned because the
harness reports `agent thread limit reached`; its verdict remains pending.
This does not block independent Step 5 implementation. No automated tests were
run or added. Steps 5-6 and conditional async compute remain incomplete.
