# DDGI/PT/RR verification closure

Date: 2026-09-08 UTC. Current executable source: `968d4e9e8e`.
Evidence root, abbreviated `E` below:
`C:/Users/lukas/AppData/Local/Temp/godot-pt-step6-20260908`.

This record supplements the
[runtime status](2026-09-08-ddgi-pt-rr-runtime-status.md) and
[post-commit review](2026-09-08-0848-ddgi-pt-rr-review-status.md). It records
bounded closure evidence and its provenance; it does not broaden any result
beyond the launches, artifacts and reviews named here.

## Current scope and source state

The active scope is DLSS/RR, path tracing and camera-following DDGI. The owner
excluded all VR/XR repairs and validation. Commit
`82c5e7a0a5c6b891be22ee0d60d4528debe4f804` removes the closure-specific
multiview guard added during investigation. Historical multiview artifacts are
excluded from closure and impose no remaining gate. The existing engine XR
subsystem is outside this task.

DDGI is an approximate lighting method. Visual parity with path tracing is not
an acceptance criterion; recorded comparisons describe observed behavior and
limits only.

The useful source correction from
`aba51ef1c82c0bd75779f38a41b1616d0f399951` remains. It replaces a zero-crossing
RT-origin choice based on `floor(camera / 1024) * 1024` with origin hysteresis:
the initialized origin is retained until an axis exceeds a 1024-unit offset,
then the nearest grid origin is selected. Necessary large-travel rebases still
reset camera temporal history. DDGI keeps its separate epoch.

Fresh source review of exact commit `aba51ef1c82c0bd75779f38a41b1616d0f399951`
previously returned PASS. That recorded verdict is retained; no new review
verdict is invented for `82c5e7a0a5c6b891be22ee0d60d4528debe4f804`.
The earlier source/XML correction
`31e3a94fbce898f06681dc0d5550380b891bba64` also retains its independent bounded
source-review and proof-audit PASS recorded in the post-commit review.

No automated tests were added or run. The fixtures below collect observations
and return no automatic image-acceptance verdict.

## Retained origin-motion evidence

`E/closure-serial-hybrid-rr.receipt.json` and
`E/closure-serial-pt-rr.receipt.json` record exit 0 on editor SHA256
`816a0fdca56929a67a1ffc3741a0d84c2f17f690576119812fbcb3bde00ec16e`.
The executable contains the `aba51ef1c8` origin correction. Each matching
`_frames.json` inventory contains 120 consecutive images for fixture frames
240-359. Camera X moves from -1.5 to +1.5 over indices 0-89 and then remains
stationary through index 119. The two modes use the same trajectory and
settings as their historical `review-motion-*` counterparts. Earlier
overlapping `closure-motion-*` launches are excluded.

`E/closure-serial-midpoint-comparison.json` records the zero-crossing comparison
at trajectory index 45, fixture frame 285:

| Adjacent active-region RGB MAE | Before correction | Corrected | Corrected moving mean |
| --- | ---: | ---: | ---: |
| Hybrid RR | 0.068139 | 0.011447 | 0.011328 |
| PT RR | 0.017586 | 0.010473 | 0.010249 |

At the same midpoint, the fraction of active pixels with mean absolute RGB
change above 0.05 drops from 60.59% to 2.967% for hybrid RR and from 4.461% to
3.058% for PT RR. The inspected corrected consecutive-image sheets no longer
show the earlier wall blotching or object/shadow noise burst. Stationary
adjacent-MAE means are 0.000525 for hybrid RR and 0.000643 for PT RR.

These are display-RGB differences without motion compensation. They close the
observed local zero-crossing transient and the earlier sparse-frame limitation
for this room; they are not radiometric error, a universal RR threshold or
evidence that necessary large-travel rebases preserve history. Synchronous
readbacks also exclude these captures from performance evidence.

## Retained partial-setup recovery evidence

`E/closure-ddgi-failure-baked.receipt.json` records exit 0 on editor SHA256
`2c32dfa8417c5ded48886bcfa2e31a0155395f5e46f753916621e7b754db4028`.
The fixture starts with DDGI disabled, requests the first context with finite
camera X=1e20 and spacing 2.0, then disables the failure and enables valid
DDGI. `DDGIEffect::prepare()` rejects the out-of-range int64 cell after context
allocation, and `RenderRaytracing::_prepare_ddgi()` deletes and nulls that
context on failure. Ten consecutive frames reach the requested failure.

| Stage | Total RD video allocations | Texture allocations | Buffer allocations |
| --- | ---: | ---: | ---: |
| Frame 50, before failure | 220,487,008 | 70,125,568 | 76,159,554 |
| Frames 61-70, repeated failure/drain | 245,309,600 | 94,930,944 | 76,176,722 |
| Frame 120, failure disabled and drained | 220,487,008 | 70,125,568 | 76,159,554 |
| Frame 200, valid DDGI enabled | 247,276,272 | 96,897,024 | 76,176,898 |
| Frame 245, viewport freed | 137,384,176 | 2,785,280 | 58,304,414 |

The repeated-failure allocation plateau and exact return to pre-failure totals
are observed cleanup behavior. Frames 130 and 200 show the baked orange cube
after recovery and valid DDGI creation. The fixture uses imported `geometry.scn`
Cube mesh SHA256
`a247b5e6b2d5dcc49fe10bdbddb4ff2769ca94554c813cfde307694f9d865914`.
Expected native guard errors make this a failure-path observation rather than a
clean-log scenario. It does not exercise failed GPU allocation, OOM, device
loss or unsupported hardware.

## Historical editor reload failure

`E/editor-hotreload-final.receipt.json` records actual exit 0. The editor saved
the Environment resource through `ResourceSaver`, reloaded it with cache ignore
and read back rendering mode 1, denoiser 2, DDGI enabled with three cascades and
spacing 1.25, path-tracing SPP 2 and five bounces. External shader, material,
VisualShader and texture changes emitted native `resources_reload` and
`texture_reimport` notifications. These notifications alone did not establish
successful rendering: later image inspection found the post-reload hybrid
capture black as well.

The same run is not a closure PASS. The PT view became black after reload and
the log records repeated native material-pipeline creation failures. This is an
implementation defect at that historical boundary; the successful
serialization readback remains a bounded observation only. Native material
cache identity is corrected in `2d0217ad22`; its subsequent runtime and review
results are recorded at final closure below.

## Retained probe, timing and memory evidence

`E/closure-probes.receipt.json` records exit 0 on the corrected editor SHA256
`816a0fdca56929a67a1ffc3741a0d84c2f17f690576119812fbcb3bde00ec16e`
at `aba51ef1c82c0bd75779f38a41b1616d0f399951`. Eight GPU snapshots cover one
context's frames 240-247 with four cascades, 16 probes per axis, spacing 0.5
and 128 rays per probe:

| Context frame | Completed cascade | Slots stamped with current frame |
| --- | ---: | ---: |
| 240 | 0 | 4096 |
| 241 | 0 | 4096 |
| 242 | 1 | 4096 |
| 243 | 1 | 4096 |
| 244 | 2 | 4096 |
| 245 | 2 | 4096 |
| 246 | 3 | 4089 |
| 247 | 3 | 4089 |

Every snapshot records one completed cascade through `last_update_frame`, and
both updates of all four cascades occur in the eight-frame interval. Cascade 3
retains seven zero stamps because the finish shader publishes a frame only when
traced and relocated positions match; zero does not mean the slot was omitted
from dispatch. The selected cascade dispatches 4096 x 128 = 524,288 primary
probe rays; the trace shader does not skip inactive probes. Hit-dependent
direct-light visibility rays are additional work. The
readbacks establish this bounded schedule only and intentionally stall rendering.
This supersedes the inactive-skip annotation in the retained counts JSON and
its summarizer; the raw counters and their hashes are unchanged.
The wrapper omitted the DDGI capture environment variables; the snapshot
metadata and counters retain the executed frame sequence.

`E/closure-profile-hybrid-sr.receipt.json` records exit 0 without Godot
`ERROR:` lines on the earlier editor SHA256
`2c32dfa8417c5ded48886bcfa2e31a0155395f5e46f753916621e7b754db4028`.
Its one emitted native GPU interval reports 1.716 ms total:

| Component | GPU ms |
| --- | ---: |
| TLAS build | 0.04983 |
| RTXDI surface | 0.01822 |
| Camera guides | 0.02870 |
| RTXDI direct lighting | 0.21200 |
| DDGI probe lighting | 0.41749 |
| DDGI camera irradiance | 0.08667 |
| Camera HDR composition | 0.18162 |
| DLSS | 0.49839 |
| Canvas items | 0.01390 |

This single interval is not an FPS promise or statistical benchmark.
`E/closure-profile-memory.jsonl` has six PID-matched Windows GPU Process Memory
records. The final two dedicated-use readings are 1,112,989,696 and
1,129,799,680 bytes (1061.430 and 1077.461 MiB). Device-wide samples in the
same records are 5103 and 5982 MiB of 24564 MiB, include all processes and are
approximately one second earlier than the Windows counter timestamps. They are
not process allocation measurements.

## Retained export provenance

`E/closure-export-build.receipt.json`, `E/closure-export-rr.receipt.json` and
`E/closure-export-sr.receipt.json` retain argv, cwd, timestamps, actual exits,
binary hashes and environment records. All return 0 without timeout or Godot
`ERROR:` lines.

| Artifact used by these runs | SHA256 |
| --- | --- |
| Exporting editor | `2c32dfa8417c5ded48886bcfa2e31a0155395f5e46f753916621e7b754db4028` |
| Exported `manual.exe` | `d5bae7798dd95a70d80670f3afffd9cb642d7c56ccf9e1a3a1b765ff0ae57429` |

The headless editor exports `Windows STEP5`; that operation is not GPU proof.
The subsequent Vulkan runs use the exported executable with `VULKAN_SDK` and
`VK_SDK_PATH` absent and one VulkanSDK PATH entry removed. Launch and exit
hashes match. `closure-export-rr.log` records RR feature 1001 evaluation with
`sl::eOk`; `closure-export-sr.log` records SR feature 0, requested preset M,
parameter 13 and `sl::eOk`. Both use 854x481 internal and 1280x720 output
dimensions. NGX records preset M selection. The viewed PNGs show nonblack room
geometry, colored walls, two boxes and shadows.

These exports predate the origin correction and current scope commit. They
retain bounded package execution and sanitized-environment provenance, but do
not establish final source-to-export correspondence or independent neural
weight identity. The wrapper's `effective_model=unknown` remains the applicable
delivery/provenance limit.

## Build correspondence and finalization boundary

The following SCons receipts record exit 0, stable inputs and binaries compiled
from the `aba51ef1c8` correction content. Their receipt HEAD is the preceding
`8e816065fd` because the source correction had not yet been committed.

| Build | SHA256 |
| --- | --- |
| Windows Vulkan editor | `816a0fdca56929a67a1ffc3741a0d84c2f17f690576119812fbcb3bde00ec16e` |
| Precision-double editor | `24a63831efc8c1e5a9de5cdaf1f198f66794df84217c5455c519bab1d6079733` |
| Debug template | `7e9450adbf09aaa10d9b30efe01177e31bd35d3823fd7d745517242724e53166` |

The editor and probe/motion receipts establish the retained origin correction's
bounded runtime evidence. These binaries also contained the subsequently
removed closure-specific XR guard, so they are not final binary correspondence
for `82c5e7a0a5c6b891be22ee0d60d4528debe4f804`.

These historical builds are superseded by the final correspondence below.

## Material and VisualShader reload correction

The final corrective range `82c5e7a0a5..3912fa6f22` changes three existing
authorities. The RT material cache compares the current hit-program RID as
well as generated source, and publishes the replacement RID only after the
fallible refresh succeeds. VisualShader clears old user nodes through its
existing `reset_state()` before copying incoming properties; ordinary `_set()`
node loading is restored. Resource self-copy returns OK before resetting the
source object. No new rendering mode, API or compatibility path is introduced.

Fresh source reviews rejected the intermediate premature cache publication,
per-node parameter-name collision and destructive self-copy. Each was fixed
before the next frozen review. Fresh source review of exact
`3912fa6f2267555e2bbf8bafc9f02e34d8490e81` and the cumulative corrective range
returned PASS, covering failure classes 1–9. The separate proof audit remains
responsible for executable observations, not the source reviewer.

`E/final2-hotreload.receipt.json` records actual exit 0 on editor SHA256
`a5999804b1a2ba656d1c46fad5c7a8a71ccf28fd8a42ab4d221375e4307684d6`, built at
`0ad6e466df`. Files were edited externally and reloaded by native editor
`EditorFileSystem.scan_sources()`, without forced ResourceLoader replacement.
The following full editor captures were visually inspected; their prefix is
`C:/Users/lukas/AppData/Roaming/Godot/app_userdata/Shader unification STEP5 export 1457/editor-manual-`.

| Capture suffix | Active mode and observed result |
| --- | --- |
| `1788862713.636.png` | Hybrid baseline: blue materials, wide checker, pink VisualShader |
| `1788862741.667.png` | Hybrid after reload: red materials, narrow checker, cyan VisualShader |
| `1788862768.027.png` | Raw PT baseline: red materials, narrow checker, cyan VisualShader |
| `1788862837.032.png` | Raw PT after reload: blue materials, wide checker, pink VisualShader |
| `1788862908.539.png` | Raw PT after identical shader-source rewrite: still renders |

The native log identifies material/shader/VisualShader reload and texture
reimport events. The earlier black material-pipeline failure and duplicate
VisualShader-node error do not recur. This is not a zero-error editor log:
one pre-reload mesh-without-baked-cluster diagnostic and unsupported editor
shader warnings remain; magenta editor grid/gizmos are visible in hybrid.
The authored baked cubes render and their requested changes are visible.
Raw PT images are low-sample editor observations, not a denoised quality claim.

ResourceSaver returns OK; cache-ignore disk readback retains PT mode 1,
denoiser 2, DDGI enabled, three cascades, spacing 1.25, 128 rays, one update,
two PT samples and five bounces, together with the upstream background and
tonemap settings. The only subsequent executable change, `3912fa6f22`, adds
the three-line self-identity return before reset; distinct-resource reload
follows the same inspected path. This explicit source bridge preserves the
bounded reload observation without relabeling the earlier binary as final.

## Build and export state before the shutdown correction

All three `E/final3-*.receipt.json` build receipts record HEAD
`3912fa6f2267555e2bbf8bafc9f02e34d8490e81`, exit 0 and stable inputs.

| Build receipt | Executable SHA256 |
| --- | --- |
| `final3-editor` | `81796409804ede316645796a4552c0a9e52602825c5c5cc26dc9b1a1a80c8618` |
| `final3-double` | `1c2a8f2b2bdc567c716a9ed5e4b493f0ca2f3ed549d84f1b24d7c9b5d2034416` |
| `final3-template` | `b13d914a38ae8a5ab105a9b99ff293ea229c3540074d3b2e81081962ddbc4b60` |

`E/final3-reopen.receipt.json` records actual exit 0 on that final editor.
The restarted editor reads the saved scene Environment with every RT/DDGI/PT
and upstream background/tonemap value listed above. The editor-only diagnostics
described above remain visible in its log; no clean-log claim is made.

`E/final3-export-build.receipt.json` records exit 0 from headless export of
`Windows STEP5` using the final editor and final debug template.
`E/final3-export-manifest.json` records matching SHA256 values for all 17
files in `thirdparty/streamline/runtime_manifest.json`.

`E/final3-export-rr.receipt.json` records actual exit 0, unchanged final-template
hash, and launch without VulkanSDK variables or its PATH entry. Its log records
RR feature 1001 evaluation with `sl::eOk`; the frame-500 PNG visibly renders
the room, both boxes and shadows. No Godot `ERROR:` lines occur.

`E/final3-export-sr.receipt.json` records an actual crash, exit 3221226525
(`0xc000041d`), after SR feature 0 reports preset M, parameter 13 and `sl::eOk`.
It produces the requested frame-500 capture before the exit failure. The PNG
is byte-identical to the successful repeat's image. The retained Windows
events in `E/final3-export-sr-windows-events.json` identify an initial access
violation at exported executable offset `0x2b61a0a`. This run is not an exit PASS.
`E/final3-export-sr-repeat.receipt.json` records the same configuration and
binary with actual exit 0, successful SR M evaluation and visible frame-500
capture. At that point, the first exit failure still required diagnosis; the
successful repeat did not establish its cause or repair. That result alone
was insufficient to close final proof or Step 8.

The fault instruction was mapped by PE disassembly, embedded source strings
and XRInterface virtual-method order to `XRServer::_process()`. No matching
PDB or retained WER minidump exists, so this is not a recovered symbolic stack.
Source inspection identifies the Windows move/resize timer as the Windows
callback into `Main::iteration()`. The timer previously checked only recursive
iteration; the main loop is deleted and nulled before servers and the display
are destroyed, leaving a window for a queued timer to iterate dead servers.
`968d4e9e8e` gates that callback on the existing nonnull main-loop pointer.
This prevents an entire invalid iteration, without adding XR-specific behavior.

The old-binary 60-second cdb attempt timed out during initialization and is
excluded. The 180-second attempt completed without reproducing an access
violation in its log. Its receipt records debugger exit, not an independently
captured target exit, so ordinary launch receipts remain the exit evidence.
## Final shutdown correction and package execution

Fresh source review returns PASS for exact
`968d4e9e8ef7884552b82c70c3886e9fc5f4efae` and cumulative corrective range
`82c5e7a0a5..968d4e9e8e`. It verifies the timer gate and the preceding material,
VisualShader and self-copy fixes. It does not claim a recovered crash stack.

All final4 build receipts record that HEAD, actual exit 0, stable inputs and
compilation of `platform/windows/display_server_windows.cpp`.

| Build | Executable SHA256 |
| --- | --- |
| `final4-editor` | `305db4048360175a033b4ac4012d0c069bb261421a026956c5145d7f10406ce5` |
| `final4-double` | `9a18e3c63cc18f7d6358c44dbb185ca7f32bbeac456fb8866a85e36655e82350` |
| `final4-template` | `e12f82c3a4319ebf04dd4555b623c3d770741ad6c90287152a648648ba445829` |

`E/final4-export-build.receipt.json` records actual export exit 0.
`E/final4-export-sr.receipt.json` and `E/final4-export-rr.receipt.json` record
serial Vulkan execution on the final template hash, actual exits 0, no timeout,
absent VulkanSDK variables and sanitized PATH. Both logs lack Godot `ERROR:`
lines. SR evaluates feature 0 with preset M/parameter 13 and `sl::eOk`; RR
evaluates feature 1001 with `sl::eOk`. Both frame-500 PNGs visibly render the
room, colored walls, boxes and shadows. `E/final4-export-manifest.json` retains
all 17 matching package hashes.

These close the bounded current-build render/export/exit observations. The
original intermittent crash and the disassembly-based causal limit remain
recorded; one successful fixed run does not prove universal shutdown reliability.
The renderer reload and saved-setting observations above remain applicable:
the final change affects only post-main-loop Windows timer admission.
Independent final proof audit returns PASS for bounded amended Step 8 at
`968d4e9e8ef7884552b82c70c3886e9fc5f4efae`. The auditor independently matched
all final binaries and package hashes, checked native shader inventories,
viewed reload/motion/export images, and audited probe, failure-path, timing
and memory records. No required work remains in the amended eight-step plan.
The acceptance boundary is the recorded RTX 4090/Vulkan configurations and
fixtures, with every historical and verification limit above retained.
