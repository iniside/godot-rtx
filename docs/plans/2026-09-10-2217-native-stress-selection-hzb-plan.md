# Native stress selection and HZB repair

Status: owner approved with "zaczynaj"; implementation authorized.
Baseline: `6bb550ae7f`. Date: 2026-09-10 UTC.

One brief independent plan review found no concrete blocker in allocation
ownership, history synchronization, scope or measurement boundaries. HZB cause,
runtime allocation behavior and speedup remain to be measured during execution.

## Outcome and scope

Make the native `microgeometry_stress/scene.escn` render without false selection
admission failures, then remove the demonstrated unnecessary raster work.
Keep the existing GPU DAG, CLAS, persistent inputs and worker system. Use the
real editor, one viewport, the existing 10000 instances and unchanged quality
controls. No Node/Flecs redesign, new scheduler, async-compute project, asset
format change, automatic quality reduction, foliage, voxels, XR/VR, split screen
or multiview. No automated tests or proof/auditor workflow; one brief review.

## Evidence and unresolved measurement

The [profile](../research/2026-09-10-2211-native-stress-editor-profile-status.md)
reports an early 16-18 FPS window with 53-59 ms GPU, including 41.246 ms Raster
Initial Draw and 7.326 ms sparse traversal. This was an interactive editor run,
not a fixed-camera comparison. Late samples also contain 240-284 ms simulation
intervals and 2-5 FPS; do not merge these with the earlier window or attribute
them to GPU. The editor has since closed; do not restart or close an owner
session merely to reproduce these insufficiently attributed samples.

The [source research](../research/2026-09-10-2214-native-stress-selection-repair-summary.md)
identifies old-plus-new allocation accounting and permanent admission latching.
The 540.70 MiB rejected peak includes approximately 269.68 MiB new variable
buffers; fixed buffers must also be included in the final active size.
Existing samples show HZB construction but no recovery markers. The exact
history predicate and contribution to the 41 ms draw interval remain unproven.
Bounded source tracing confirms that native editor camera history follows the
normal persistent Camera3D/viewport path; no missing camera RID was found.
The deformation veto can also invalidate the history and must be attributed.

## Existing authorities and contracts

- `micro_geometry_selection.{h,cpp}` owns pass buffers, capacity feedback,
  committed cuts and submission retirement. Extend this implementation; no
  parallel allocator, global memory manager or fallback mesh path.
- `RenderBufferDataForwardClustered::prepare_rtxdi_surface`,
  `_select_micro_geometry` and `_render_list_with_draw_list` own depth-history
  validation, HZB activation and raster passes in `render_forward_clustered.cpp`.
  Correct the existing producer/consumer contract rather than forcing history
  valid or adding a second occlusion implementation.
- Existing `micro_geometry_stats_received` provides asynchronous raster cluster
  and triangle counts; render-info and shared-cut logging expose residency,
  CLAS and RT selections. Reuse these and existing `--gpu-profile`/CPU timers.
  No new synchronous GPU readbacks or profiler framework.
- `micro_geometry_select.slang` publishes candidate cuts only without overflow;
  capacity failure retains the prior committed cut. Keep that completeness
  contract. Do not silently truncate draws to make memory admission succeed.

## Step 1 — attribute the missing HZB and establish a baseline [independent]

In `render_forward_clustered.{h,cpp}` and `micro_geometry_selection.{h,cpp}`,
add bounded, profiler-enabled reporting through existing logging:

- camera/shadow pass identity; HZB enabled and individual history rejection
  reasons; engine and last-rendered frame identity;
- for a deformation veto, subtype and first triggering instance/surface,
  collected inside the existing visible-instance inspection rather than a new scan;
- active/fixed/retired/requested and replacement-peak bytes, overflow counts,
  resize attempts and allocation outcome;
- existing raster clusters/triangles, selected RT cuts, pages and CLAS counts.

Split the ambiguous Raster Initial Draw marker into camera and shadow labels,
and distinguish conventional drawing from microgeometry drawing where both
share the interval. Preserve meaningful parent/child timing semantics.
Counters must not create recurring per-instance CPU traversal. Attach any
asynchronous callbacks to existing refcounted owners/epochs and preserve teardown.

Build an ordinary editor, run the native scene with a fixed saved camera and
viewport size, and collect a short warm stationary interval plus a camera move.
Record the actual resolution/upscaling, scene and executable identity and
whether compiler/streaming activity persists. Identify the exact failed history
predicate before selecting the Step 3 change. No blind history relaxation.

## Step 2 — correct selection replacement admission [independent]

Change `MicroGeometrySelection::_resize`, `_retire_buffers`, `needs_retry`,
`select` and their private pass state in `micro_geometry_selection.{h,cpp}`.

Apply the existing 512 MiB pass budget to the complete target active allocation,
including fixed allocations and minimum buffer sizes. Track old-plus-new
residency separately as a transient peak. Keep at most one retired replacement
set per pass and postpone another resize until that set retires. This bounds
the transient overlap by old and new admitted active sets; it does not double
the permanent budget or remove checked RD/index limits.

Preserve the last complete cut, allocate a complete replacement, copy committed
records, publish new buffers and retire the old set after its GPU submission
completes. On partial allocation failure release only replacement resources.
Keep true hard-limit failures distinguishable from waiting for retirement and
actual allocation failures. Temporary retirement waiting must remain retryable;
unchanged impossible requests must not trigger allocation/log spam every frame.
Do not introduce a general OOM recovery scheduler. Actual OOM remains explicitly
reported and safely retains the valid pass; input recreation can reattempt it.

Remove the false peak-limit failure path and its permanent latch for admissible
growth in the same change. Reuse existing capacity and retirement state rather
than adding another source of truth. Verify all shared users, including RT
selection, while keeping RT visibility independent of camera occlusion.

## Step 3 — repair the demonstrated history/HZB defect [independent]

After Step 1 identifies the rejected predicate, correct its existing producer or
consumer in `render_forward_clustered.{h,cpp}` and, only if traced there,
`renderer_scene_cull.cpp` / `renderer_viewport.{h,cpp}`. Preserve viewport/camera
identity and actual previous transforms/projection/jitter, correct resize and
camera-cut invalidation, and conservative deformation handling. Retain the
existing two-pass occlusion recovery so newly revealed geometry appears.

If no incorrect history invalidation is observed, omit this code change and use
the split draw counters to identify the remaining raster cost. Do not substitute
an unmeasured shader redesign or global LOD reduction. An additional algorithm
change requires a concrete extension of the plan based on those measurements.

## Step 4 — measure the result and hand off [inline validation/docs]

Use the same ordinary Vulkan editor, scene, camera, resolution and quality.
Check stationary rendering, ordinary camera movement/reveal, one viewport resize
and editor close/reopen. Confirm no selection-admission errors for this workload,
no stuck overflow after warm-up, stable retirement accounting, and all 10000
instances retained. Do not require the physically impossible condition of zero
overflow during first capacity discovery or zero streaming during movement.

Compare camera raster, shadow raster, traversal, main active intervals versus
waits, and whole-editor FPS. Repeat a short matched run without detailed profiling
to confirm the improvement survives instrumentation overhead. Report admission
correctness separately from speedup; do not claim completion of performance work
if draw cost/FPS does not improve. Inspect rendering only for regressions caused
by the changed cuts/history, without reopening the owner's quality assessment.

## Compatibility, ownership and review

No public bindings, ClassDB/XML, settings, serialized fields or import rebuilds
are planned. Existing C++/shader layouts remain unchanged; if a diagnostic needs
shader data, prefer the current state/statistics buffers. Do not edit generated
files or add SCons entries without an actual new source. GPU allocation and
publication stay on their current owner; retirement remains submission-based,
never an added blocking wait. Renderer-level history changes must retain runtime
build guards; ordinary Vulkan is the execution target. Double/template builds
are needed only if the eventual change touches their distinct contract.

After approval, commit this plan separately; delegate each whole implementation
step and commit owned changes. One brief final source review covers the final
diff and relevant resource/history risks. No proof auditor or repeated review
packages. Update the canonical status with measured outcomes and remaining
limitations. Preserve all unrelated working files and the owner's editor state.
