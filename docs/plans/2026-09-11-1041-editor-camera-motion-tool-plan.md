# Editor camera motion tool and native FPS measurement

Owner request, 2026-09-11: add a simple tool usable in the editor to move and
rotate the camera, then perform that movement and measure FPS. Task-start
`a82ecdcacb`. This authorizes the tool and the native performance exercise.
No computer use, automated tests, proof auditors, renderer quality/configuration
changes, streaming repairs, XR/VR, multiview or split screen. Double is primary.

## Existing authority and overlap

`Node3DEditorViewport` owns the native editor camera, while EntitySceneEditor
owns entity list/Inspector editing. Reuse its View3DController and existing
camera publication path rather than directly moving a separate game entity:

- `editor/scene/3d/node_3d_editor_viewport.cpp:4779` synchronizes the controller
  from a transform; `:5495` sets the native document camera.
- `update_camera()` and `_cursor_interpolated` at `:3021` publish the Camera3D
  transform and redraw; movement must use that same authority on the main thread.
- `get_state()` at `:5420` serializes the controller cursor. Temporary motion
  must not overwrite the user's persisted camera state.
- Existing viewport CPU/GPU measurement APIs are consumed at `:3770-3790`.
  Reuse those measurements without a new renderer instrumentation subsystem.

Research uses clangd and exact source reads. No existing native script/CLI
playback route exists; main-loop scripts are unsupported in this fork. A narrow
editor-only command file and CLI client satisfy the requested reusable control.

## Step 1 -> camera command and client [independent]

Own `editor/scene/3d/node_3d_editor_viewport.{h,cpp}` and a small Python CLI
`misc/scripts/editor_camera_motion.py`. Extend a neighboring controller source
only if the existing camera authority requires minimal closure. No new public
binding, ProjectSettings, environment variable, server or SCons source is needed.

The first editor 3D viewport polls one project editor-cache command file at low
frequency. The CLI atomically submits one uniquely identified JSON command and
can wait for status/results. Ignore stale consumed commands; reject malformed,
non-finite or unbounded input with a useful status rather than repeated errors.
Do not pollute the project scene or commit runtime command/status files.

Support a bounded sequence of relative translation and yaw/pitch rotation
segments with wall-clock durations, including a stationary segment. Keep the
protocol minimal. Movement runs through the existing controller/camera update
each frame and requests drawing while the tool is active. No rendering mode,
quality, timer or display setting changes. The idle tool must not force redraw.
Stationary segments also preserve ordinary redraw behavior. Reject commands in
preview/pilot/freelook or incompatible interaction modes, since pilot publication
can change a scene camera rather than only the editor view.

Snapshot the initial controller camera state, restore on completion/cancellation
or context change, and return the original state during editor-state saving
while temporary motion is active. Use existing main-thread/editor ownership;
no worker/RID allocation or runtime dependency. Do not save or alter scene data.
`View3DController::Cursor` is snapshotable; `update_camera(0)` publishes without
smoothing. Do not call `set_document_camera` per frame, as it resets distance and
projection behavior. Finish before document/scenario/camera/state retargeting,
and while controller objects remain alive on visibility loss or tree exit.

Record per-segment actual wall-clock duration, frames, FPS/frame-time median and
p95, plus existing viewport CPU/GPU measurements if available. Distinguish
callback intervals from rendered frames where the existing authority differs.
Use `Engine::get_frames_drawn()` deltas for rendered FPS and monotonic process
intervals separately; delayed viewport CPU/GPU samples are not synchronized
per-frame evidence. Restore existing measurement enablement after completion.
Emit start/end and resulting camera transforms so motion is observable through
files/logs. Keep per-frame measurement work bounded, no per-frame logging/I/O.
Status includes completion/cancellation/errors; the client must not wait forever.

## Step 2 -> build, brief review and use

Build `scons platform=windows target=editor precision=double accesskit=no
d3d12=no -j16`. Commit the coherent tool, then perform one fresh brief source
review of exact commit and cumulative task diff. No automated tests or proof
audit; this is an owner-requested editor control/performance tool.

Parent opens the existing native10000-instance stress scene on the new double
editor with normal renderer/settings and no old primary-visibility variable.
After loading, submit a bounded route that translates and rotates while staying
near the initial view, with stationary baseline and return/restoration. Record
FPS during motion, GPU/CPU if available, shadow cadence/coverage and streaming
or admission errors from existing logs. Known512MiB selection-budget failure
must remain disclosed; successful FPS is not proof of requested geometry detail.
Report actual completed route, timing and limits, and document the reusable CLI
in a canonical status linked from project-state. Parent owns those docs.
