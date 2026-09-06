# DLSS camera contract correction

Status: approved by owner on 2026-09-06; preimplementation plan review PASS.
Task baseline: `26a18f0d1382382a921bba8f56f9517fa9b21e8a`.

## Context and scope

The owner reports flicker/strobing and ghosting that increase with motion in
`gi_demo/test.tscn`, and wants these addressed before integration performance.
The project selects DLSS, RR preset E, default 1 spp and 8 path-tracing bounces.
The existing double-precision binary reports Vulkan on an RTX 4090. Correct the
existing camera-data contract first; visual causality remains to be verified.

Only two implementation files are owned:

- `servers/rendering/renderer_rd/effects/dlss.cpp`
- `servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp`

No jitter, preset, sample-count, history-reset, scene, import, performance,
shader, new buffer, setting, public API, or compatibility-path changes.
Existing `DLSSContext::Parameters`, `DLSSEffect`, and `_render_3d_upscaling`
already own the behavior; no new abstraction is required.

## Evidence and contracts

Targeted source reads were used because root `compile_commands.json` is absent.
Bounded text inventories are lower bounds; source and git history establish
the integration as NVIDIA fork-local behavior.

- `dlss.cpp:195`: current `sl_convert_matrix` preserves the numerical Godot
  column-vector matrix. NVIDIA uses row-vector transforms, requiring its
  mathematical transpose, so Godot column i must become Streamline row i.
  This applies to all existing view/projection/reprojection helper consumers.
- `core/math/basis.h:177`: actual camera axes are basis columns, not rows.
- `core/math/projection.cpp:858` and `core/math/projection.h:103`: aspect is
  width/height; horizontal-to-vertical FOV conversion needs its reciprocal.
- `render_scene_data_rd.cpp:84` and `render_raytracing.cpp:3385`: hardware
  projection uses unjittered depth correction with Y flip, reversed Z, and
  remapping into [0,1]. Jitter remains a separately supplied quantity.
- `shaders/effects/motion_vector_inc.glsl:1`: the existing decoder maps hardware
  depth to [-1,1]. Its matrix convention must survive the caller correction.
- `dlss.cpp:253`: raw copying float-sized bytes from `Projection` corrupts the
  decoder matrix in double builds. `MaterialStorage::store_camera` converts
  elements to the existing float GPU representation.

Primary external authorities:

- [Streamline matrix helper](https://github.com/NVIDIA-RTX/Streamline/blob/main/include/sl_matrix_helpers.h)
- [Official sample](https://github.com/NVIDIA-RTX/Streamline_Sample/blob/main/src/StreamlineSample.cpp)
- [Common constants](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuide.md#2111-common-constants)

## Step 1 -> Step 2

### Step 1 [independent]: correct the complete camera-data path

Dispatch one coherent task to `core-implementer`, gpt-5.6-sol, high effort.

1. In `sl_convert_matrix`, replace the current packing with the transpose
   required by Streamline, converting real_t elements to float.
2. In `DLSSEffect::_upscale_internal`, use basis columns 0, 1 and negative 2
   for camera right, up and forward respectively.
3. In the DLSS branch of `_render_3d_upscaling`, use `1.0 / aspect` for FOV.
4. Use `set_depth_correction(true, true, true)` there. Supply the corrected
   current projection and build reprojection from corrected current/previous
   projections, making both Parameters matrices hardware-space, unjittered
   Godot column-vector matrices.
5. At the decoder upload in `DLSSEffect::upscale`, convert hardware reprojection
   R to `D.inverse() * R * D`, where D is
   `set_depth_correction(false, false, true)`. This reproduces the decoder's
   existing [-1,1] clip convention without changing its shader or adding fields.
   Upload with `MaterialStorage::store_camera` instead of raw memcpy.

Replace all wrong paths in this step. Comments default to none. Existing
signatures, ClassDB/XML/defaults and shader layouts stay intact; push constants
remain 80 bytes. There are no new allocations, RIDs, resource owners, teardown
paths, thread transitions, tags, barriers or callbacks. Existing SCsub wildcard
sources cover both files. No editor-only dependency or generated/thirdparty
edit is required. The common DLSS SR/RR correction is intentional; FSR2, TAA,
and MetalFX branches are outside this change. Streamline code is shared by
Vulkan and D3D12; native Metal does not exercise it.

### Step 2: build, real-device validation, commit and review

Build the Windows editor with `precision=double`, using verified existing SCons
options and available build configuration. Confirm the binary includes the
changed source. Launch `gi_demo/test.tscn` on Vulkan/RTX 4090, inspect logs and
observe continuous slow/fast rotation and translation, then stopping. Compare
static world edges separately from the camera-attached mirror's reflections,
with scene/settings/resolution fixed. Both flicker and ghosting must improve
before claiming the reported problem resolved; report residual artifacts.

Known limitations: baseline reports a mesh without baked cluster data; do not
silently reimport owner content. `camera.gd` starts its rotation accumulator at
zero, so ignore the first mouse-induced jump when judging continuous motion.
Baseline startup is not visual proof. If native observation/capture is not
available, report motion validation pending owner observation. Claim D3D12 or
regular SR validation only if actually exercised. No automated tests are
authorized or proposed.

Save and commit this approved plan separately before implementation. Preserve
all unrelated working changes and untracked scene content. Commit the owned
implementation after scoped checks, freeze its SHA, then dispatch a fresh
hostile reviewer on that exact commit and the cumulative baseline-to-target
diff. Follow repository rules for any finding fixes and the two-round cap.
Relevant failure classes: 2, 3, 4, 5, 6, 7, 8, 9.
