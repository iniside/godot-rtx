# Native stress repair execution

## Current result — 2026-09-11

Repair source: `ab4e61b17ab36754317aab47128ec2e3f34f6a65`; cadence correction:
`0ae5fc578a`. Ordinary Vulkan editor, unchanged native scene/camera, actual
2689x1602 guides and1345x801 lighting, all10000instances. With original origin
visibility restored and profiling disabled, last-ten median FPS increases
23 ->43.5 (about89%). Final run exits0 without ERROR lines; resident pages3724,
pending pages0,62 shared RT cuts and245705 selected RT clusters. Baseline
streaming was still active, so these are launch-window results, not identical
residency snapshots. Scene SHA256 remainsF276E9A2B9A18B8DE4AE672BF339D7D7925D589E29131EA99A1F61B939A8B19A.

With origin hidden in both profiled captures and target1px unchanged, GPU
43.218 ->22.897ms; camera25.247 ->12.382ms; shadows9.080 ->3.340ms.
Camera emission170763929 ->77938217 triangles. The tightened conservative
transform scale bound replaces the loose Frobenius bound. The temporary4px
probe is removed from source and the delivered binary; no quality setting was
changed. Counters remain available with `--gpu-profile`, sampled through the
existing async readback approximately every120frames.

Final camera selection active storage152115400bytes, retired0, queue/record
overflow0 and no admission failure. The original512MiB admission fix remains.
All four temporary global editor timing/display fields and the3D origin setting
are restored and verified. Root measurement editors have exited. No automated
tests or motion/resize checks were performed; owner clarified stationary excess
detail as the relevant symptom and retains visual quality assessment.
Origin decoration still invalidates HZB under the existing shared-depth contract;
its separate pass topology is not repaired. This work establishes a measured
speedup, not completion of all microgeometry/renderer optimization.


The owner approved the
[selection/HZB repair plan](../plans/2026-09-10-2217-native-stress-selection-hzb-plan.md)
with "zaczynaj". Plan commit: `3e5aa39bf03cc546233858a75bd6e157c11d9a24`;
task-start source: `6bb550ae7f`. Selection admission repair is implemented and
measured without allocation errors. No FPS improvement is established; the
2026-09-11 resumed investigation measures editor-origin influence on HZB.

The first matched-route ordinary editor measurement uses the existing executable
SHA256 `DA0FF8839DA9FAB89FE7DC1FDE2D1FEEDA348995E35F4B17101F3864E6CDB419`
and native stress scene SHA256
`F276E9A2B9A18B8DE4AE672BF339D7D7925D589E29131EA99A1F61B939A8B19A`.
Capture name: `stress-repair-before-noprofile`, under
`%TEMP%/godot-render-repair-20260909/`. It requests Vulkan, no VSync, FPS logging,
900 frames and the saved native scene. The editor restores its maximized layout
despite the requested 1600x1000 window; actual initial camera guides are
2689x1766 and lighting is 1345x883. Use the logged actual size for comparisons.

No automated tests, standalone game launches or proof audits are part of this
execution. Preserve the owner scene and unrelated working files. Final GPU and
editor measurements remain pending.

The first run exits 0 but reports exactly 10 FPS: the existing unfocused editor
sleep is 100000 us, so it is not a useful renderer baseline. For matched captures
only, editor focused/unfocused sleep values become 1 us, VSync is disabled and
continuous updating enabled. Original values (6900/100000 us, VSync 1,
continuous false) are retained in
`%TEMP%/stress-repair-editor-settings-original.json` and must be restored after
the captures. `stress-repair-before-uncapped` repeats on the preserved original
executable `bin/godot.stress_repair_before.exe`.

Implementation feasibility refinement: RenderingDevice timestamps cannot be
inserted after draws inside an active draw list. The diagnostic keeps combined
conventional/microgeometry lists intact and reports their counts alongside
camera/shadow GPU intervals. It does not alter render topology for profiling.

Uncapped original run exits 0 without timeout. Last ten FPS reports are
21,22,22,22,22,22,21,22,22,22 (median 22); all three original selection admission
errors recur. Last residency sample: 10000 instances, 2 assets, 89 cuts/BLAS,
434537 RT selected clusters, 4095 resident pages, 279 pending pages and 67442
resident CLAS. Streaming remains active, so this is a reproducible launch route
and observation window rather than a fully converged streaming baseline.

Step 1 diagnostics commit `4629cb4c4c` compiles in the ordinary editor. Native
`stress-repair-diagnostics` exits 0 but exposes bool arguments incorrectly passed
to `%d` in Godot vformat. Predicate/selection messages fail to format, so the run
does not identify the HZB cause and is not a performance baseline. Root fixes
renderer string placeholders in `35547df1c9`; the selection counterpart is part
of the ongoing Step 2 owned edit. The next run will contain admission repair and
correct diagnostic strings together, then isolate any history defect separately.

Step 2 `a003e854cbf51c2cf240d9520f01a7d446af4e81` uses one exact fixed-buffer
account and applies 512 MiB to target active allocation. Retirement waiting stays
retryable without worker/allocation attempts until the pending set completes.
Ordinary editor build passes in 33.49 seconds. Root launches
`stress-repair-admission-profile` with unchanged scene/camera and uncapped editor
settings. Its runtime outcome and the HZB diagnosis remain pending.

Admission profile completes 900 frames, exit 0, without ERROR lines. At frame858
all reported camera/shadow passes have zero queue/record overflow, no admission
failure and no retired bytes. Camera active allocation is325750472 bytes with
last replacement peak648178136 bytes; the new accounting admits it successfully.
Warm history samples reject only deformation, subtype `material_deformed`,
persistent instance82/surface0. Camera history IDs and frame continuity match.
At frame240 camera statistics report1910694 clusters /164048252 triangles.
Last ten stage batch medians: camera combined raster28.195 ms, shadow combined
raster9.630 ms, sparse traversal3.109 ms. Whole GPU samples45.085-50.885 ms and
FPS19-22. This establishes admission behavior, not a matched speedup.

The editor layout now reports2689x1602 guides after startup, so root repeats the
preserved original binary as `stress-repair-before-layout-matched` before final
comparison. Step3 is delegated to qualify the deformation veto against actual
RTXDI surface depth contributors; no blind exemption is authorized.

The repeated original baseline exits 0, retaining scene hash and actual
2689x1602 guides /1345x801 lighting. Last ten FPS reports are
22,22,23,22,21,23,23,23,23,23 (median23), with the same three admission errors.
This is the size-matched comparison for the eventual final binary.

Step3 `07daa73d01628d72409221cb2ed1ed95fc5a6888` qualifies raster deformation
vetoes using the depth-write behavior of the actual shader, including existing
debug shader substitutions. Path tracing retains the previous conservative
guard. New sampled skipped-surface details will identify whether instance82
is excluded for a valid reason. Ordinary editor build passes33.36 seconds;
`stress-repair-history-profile` and one brief final source review are in progress.

That run did not confirm the proposed cause: instance82 remains a material
deformation veto and no non-depth exclusion is reported. Source review found no
correctness blocker in the reviewed code, but this does not establish relevance
or a speedup. Commit `6c2e0d48dd` removes the unconfirmed depth filter and restores
the original predicate, adding actual first-veto shader identity/compiler flags.
Build passes33.93 seconds; `stress-repair-veto-identification` is a240-frame
diagnostic run. The preceding history run exits0 but reports one occlusion RID
leak at shutdown; its cause is unassigned, not claimed fixed by these changes.

Identification run completes240 frames, exit0, without ERROR lines. The first
veto remains instance82/surface0: uses_position=true, other recorded deformation
flags=false, uses_time=false, generated_standard=false, depth_draw=1/depth_test=1.
The shader constructs a3-pixel screen-space line from MODEL_MATRIX endpoints
and writes POSITION. This identifies shader behavior; source ownership and
whether its depth contribution makes the veto necessary are being traced.
No HZB correction or speedup is established by this run.

The final no-profile run at6c2e0d48 completes900 frames, exit0, no ERROR lines.
Both original and repaired binaries report a last-ten median23 FPS at actual
2689x1602 guides /1345x801 lighting. All10000 instances remain. Streaming is
still active: final4089 resident/320 pending pages versus baseline4064/286.
No speedup is demonstrated. The four temporary editor settings were restored
and verified before the owner pause.

Source identification locates the veto in the editor origin line shader,
`Node3DEditor::_init_indicators`, node_3d_editor_plugin.cpp:1072-1149. Its
POSITION write and actual depth contribution make the current deformation
guard legitimate; Step3 behavior change is omitted under the approved gate.
On owner continuation, an existing show_origin=false editor setting provides
a controlled intervention before considering any renderer topology change.
The scene, saved camera and geometry quality remain unchanged. Temporary
editor fields and origin visibility will be restored after measurement.

Controlled origin-hidden profile at6c2e0d48 completes900 frames, exit0, with
one screen swap-chain preparation error (not a clean run). History becomes
valid immediately after initialization; warm HZB and recovery are enabled.
Nevertheless frame720 reports2013576 camera clusters /172915661 triangles.
Last-ten batch medians: whole GPU44.823 ms, camera raster26.290 ms, shadow
raster9.462 ms, recovery draw0.0072 ms. Profiled editor FPS median21. Main
late samples spend44-45 ms waiting for frame admission versus roughly0.6-1.1 ms
simulation and0.8-0.9 ms process/navigation. This identifies GPU raster cost,
not a new main-thread scheduling bottleneck. A separate no-profile run follows.
The origin-hidden intervention is diagnostic only, not a shipped fix. Existing
transparent callbacks have no geometry draw; preserving decorations outside
history requires a real color pass, so that topology extension is deferred
while HZB effectiveness/selection volume counters are added.

The origin-hidden no-profile repeat exits0 with no ERROR lines, retaining
2689x1602 guides /1345x801 lighting and10000 instances. Last-ten FPS:
21,20,21,20,21,21,20,21,21,20 (median21), versus origin-visible23.
Streaming remains active (4077 resident/442 pending pages). Thus HZB activation
alone is not an observed whole-editor improvement. The next diagnostic uses
existing asynchronous statistics readback to count initial/recovery rejection
reasons and actual emissions, sampled only under profiling.

Diagnostic commit4182482539688004d25d97c565cc1161a203653a expands the
existing selection statistics allocation16 to128 bytes, including admission
accounting/reset. Extra counters run only on profiler camera frames divisible
by120, with initial/recovery phases separated and existing Ref/epoch async
readback retained. Original totals remain at offsets0/1; no selection or
quality change is made. Ordinary editor build passes in37.06 seconds.
The origin-hidden900-frame diagnostic capture is in progress.

First counter run at4182482539 exits0 but emits no detailed async counter
lines: fixed modulo120 sampling collides with the existing pending readback.
Immediate selection lines alone are insufficient diagnostic evidence. The
correction samples the next available readback frame at least120 frames after
the last successful diagnostic submission, preserving async ownership and
requiring no wait. Build/measurement precedes its commit to reuse the current
shader-cache version; capture provenance records the exact dirty source diff.

Cadence correction builds in30.65 seconds; exact dirty-diff profile completes
900 frames, exit0, no ERROR lines. All14 phase samples balance input/rejection/
emission counters. At frame840: candidate=committed2066070 clusters;
retained_units0 of10000. Initial frustum rejects67103;188330 extend outside
viewport;1810637 receive HZB samples,1398802 include zero depth; HZB rejects
10407. Actual initial emission1988560 clusters /170788567 triangles. Recovery
rechecks10407 and emits0. This proves a fresh cut is published in the stationary
sample and occlusion rejection is only about0.5% of input. It does not prove
target LOD reached: wanted children lacking ready pages remain inactive and
their coarse parent can form a new valid candidate without retaining old cuts.
Owner observes meshes sticking at coarse or fine levels during movement;
additional sampled refinement readiness/fallback counters address this gap.

Owner clarifies the excess detail is visible while stationary on distant
meshes; movement is not required for this reported symptom. Read-only parsing
of the actual native_assets/microgeometry_stress mgdata manifests follows
MicroGeometryData::encode_manifest/load; both manifest SHA256 digests match.
Lucy has28055742 source/leaf triangles,19 group depth levels and104 triangles
in its single coarsest terminal cluster. Thai has10000000 source/leaf triangles,
18 levels and74 coarsest triangles. Simplified geometry exists in the shipped
fixture; these metadata facts do not prove runtime LOD selection is correct.
The source copies group bounds.error directly into the GPU layout; projection
parameters use the current camera and measured output_height1602/error1.0.
Next160byte diagnostic extension builds in30.71 seconds and measures readiness,
threshold stops, force-finest and coarse fallback under unchanged selection.

The160byte refinement run exits0 but reports three screen swap-chain errors;
not a clean performance run. At frame840 it reports5614 wanted-not-ready
group occurrences,30 ready groups blocked by inactive parents,279019 accepted
refinements,274271 threshold stops and0 force-finest units. Every emitted
cluster (1988560) is a non-leaf cluster whose finer child is inactive. This
confirms actual simplification in the sample; it does not establish sufficient
simplification of distant objects. Of1398802 zero-depth HZB footprints,47547
overlap padded image edges and1351255 are interior: padding alone is not the
main explanation. Final sampled buckets will attribute emitted triangles to
conservative projected instance size and identify near-distance clamp cases.

Size-bucket diagnostic builds in40.33seconds and its900-frame run exits0 with
no ERROR lines. Buckets sum to10000units and agree with total cluster/triangle
counts in each sample. At frame841 the conservative32-64px bucket contains
3625units and24864454 emitted triangles;64-128px contains4572units and61020762
triangles. No emitted cluster is a leaf. Epsilon distance clamping affects only
215 nonterminal group decisions, rather than the broad population.

A temporary camera-only parameters.error=4 probe builds in34.15seconds, exits0
without ERROR lines, and reduces camera emission to23640304 triangles versus
170763929 at1px. Last-ten profiled medians: camera25.247ms ->3.864ms;
whole GPU43.218ms ->20.545ms; FPS22 ->44. Shadows stay roughly9.1-9.3ms.
This is a sensitivity experiment, not a quality-equivalent speedup. The temporary
line is removed before the subsequent build; default1px is restored in source.

The measured continuation is recorded in plan commit e7ea2bbc39. The existing
shader scale estimate gives sqrt(3) for identity, inflating both error and bounds.
An existing-shader helper replaces it for instance/MultiMesh transforms with
sqrt(min(trace(A^T A),maximum absolute row sum(A^T A))), plus a1ppm outward
rounding margin. Both terms bound squared stretch; ordinary rotation/scale is
tight while affine shear remains conservative. Shared raster/shadow/RT thresholds
are unchanged, although their selected cuts may change. Ordinary editor build
passes35.70seconds; exact dirty-diff1px measurement is in progress. No additional
pass, asset format, setting or CPU traversal is introduced.

The restored1px scale-bound run completes900 frames, exit0, no ERROR lines.
Same origin-hidden camera/resolution: late GPU batch medians43.218 ->22.897ms,
camera25.247 ->12.382ms, shadows9.080 ->3.340ms; profiled FPS22 ->41.
At frame840 camera emission falls1988277 ->912067 clusters and170763929 ->
77938217 triangles. Configured1px is logged, all10000units remain, retained
cuts0 and recovery emission0. This is the measured numeric-bound repair,
separate from the temporary4px sensitivity result. Shared RT/shadow selection
also uses the tighter bound, so its changed work is part of the outcome.
Root restores the original3D origin visibility and starts a final no-profile
900-frame run on the same frozen binary. The four temporary editor timing/
VSync/update fields will be restored after that root-owned editor exits.

One fresh brief source review passes exactab4e61b17a and cumulative
6bb550ae7f..ab4e61b17a: conservative bound,272byte buffer/account/reset,
Ref/epoch callbacks, readback cadence and existing submission retirement.
This is source review, not a runtime audit; runtime outcomes are recorded above.
Final original-origin no-profile capture exits0/no ERROR lines, last-ten FPS
44,45,45,44,43,42,42,43,43,45 (median43.5). All temporary settings restored.
