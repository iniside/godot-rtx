# Editor camera motion tool and measurements

Owner request on2026-09-11; baseline `a82ecdcacb`.
[Plan](../plans/2026-09-11-1041-editor-camera-motion-tool-plan.md) committed at
`f87fe6038a`, with brief plan review PASS. Implementation `2eb86b14fc` is complete; primary double build passes in28.25s,
and fresh brief source review passes exact commit plus cumulative task diff.

The tool controls editor viewport0 through the existing View3DController,
without computer use, renderer configuration changes, or scene modifications.
It restores the initial camera after completion/cancellation. Double is primary.

CLI contract:

```powershell
python misc/scripts/editor_camera_motion.py --project demos/rtxdi_manual --sequence <route.json> --wait --timeout 240
```

The route is a JSON array of segments containing `duration` (seconds),
`translation` (three camera-local displacement components relative to the
segment-start orientation), and `rotation_degrees` (pitch, yaw deltas).
Commands and status live in the project's `.godot/editor/` cache. No server,
environment selector, ProjectSettings key or script binding is introduced.

Completed native exercise:45-second warmup, then six10-second segments, stationary; translate[4,0,0];
yaw+20degrees; translate[-4,0,-4] and pitch-5/yaw-20; translate[0,0,4] and
pitch+5; stationary. All translations are in the segment-start camera basis,
so the final explicit restoration, rather than those deltas, returns exactly
to the original camera.

FPS must distinguish actual engine render-frame deltas from editor process
callback intervals. Viewport CPU/GPU samples are delayed measurements, not
synchronized per-frame evidence. Idle/stationary behavior keeps ordinary redraw
pacing and no editor timer/display setting is modified.

The known sparse-selection512MiB admission failure from the default-double run
remains outside this task. Motion/performance results must report such failures
and must not imply requested geometry detail is proven by nonempty rendering.
Native result `7036f6c4b41444e7a86348326d5ee5a5` completes all seven segments.
The earlier startup command cancelled on editor state restoration after14.8s;
that partial run is excluded. Native double PID63152 remains open after the
completed exercise, with the original camera restored exactly (maximum component
absolute difference0). Scene hash remains
`f276e9a2b9a18b8de4ae672bf339d7d7925d589e29131ea99a1f61b939a8b19a`.

| Segment | Draw FPS | Viewport CPU median ms | GPU median ms | GPU p95 ms |
| --- | ---: | ---: | ---: | ---: |
| Stationary baseline | 100.33 | 5.43 | 9.23 | 10.69 |
| Translation4m | 45.20 | 11.57 | 9.93 | 34.75 |
| Yaw20degrees | 40.76 | 11.74 | 9.59 | 37.71 |
| Combined movement1 | 26.89 | 16.76 | 10.30 | 38.91 |
| Combined movement2 | 24.81 | 15.81 | 10.73 | 34.51 |
| Stationary endpoint | 82.09 | 5.55 | 10.76 | 12.58 |

Combined four moving segments:1379 engine drawn frames in40.072618s,
**34.41 FPS**. These are different camera positions, not an isolated causal
comparison. CPU is viewport render timing, not main-thread time; GPU sample
lag/duplicates remain as stated in the tool result. Both CPU cost and GPU tails
increase during movement. No narrower bottleneck attribution is proven.

Artifacts in `C:/Users/lukas/AppData/Local/Temp/godot-render-repair-20260909/`:
`editor-camera-motion-route.json`, `editor-camera-motion-completed.json`, and
`editor-camera-motion-startup-cancelled.json`. The GUI executable launch did not
capture renderer stdout; the standard app log inspected was stale and is not
used as evidence. This run therefore makes no error-free or admission-success
claim. No renderer error, budget or quality setting was changed to obtain FPS.

To repeat, keep this project open in the double editor, write the route JSON,
and run the CLI above. For example a segment is
`{"duration":10,"translation":[4,0,0],"rotation_degrees":[0,20]}`.
Use `--cancel <id> --wait` to cancel an active command. Status/result is
`.godot/editor/camera_motion.status.json`; commands are consumed once.
