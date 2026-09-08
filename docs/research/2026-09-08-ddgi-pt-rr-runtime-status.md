# DDGI, camera PT and DLSS integration — 2026-09-08

Post-commit review: the original final handoff omitted mandatory review. Fresh
reviews rejected it, then the source/XML and diagnostic-gate corrections at
`31e3a94fbc` received independent bounded PASS verdicts. Full-plan verification
remains REJECT. See the [review status](2026-09-08-0848-ddgi-pt-rr-review-status.md)
for findings, correction evidence and the outstanding requirements. The build
and run records below retain their historical revisions; the review status
records the later corrected builds separately.

## Implemented behavior

The shared native material/TLAS scene now serves both camera-following DDGI
and a true camera-ray path tracer. PT owns primary depth, material guides and
motion; it does not use raster primary hits. Raw PT accumulates an FP32 mean,
with configurable 1–64 samples and 1–32 bounces. NRD and RR previews use one
sample per frame. Raw accumulation invalidation is separate from preview
temporal history, so ordinary camera motion does not clear NRD/RR every frame.

Camera composition supports NRD, RR and no denoiser. RR consumes composed noisy
HDR and bypasses NRD and ordinary SR. Required RR albedo and world-normal guides
are generated from the matching camera surface. Optional reflection-distance
tags remain absent: the PT BSDF-mixture distance is not misrepresented as a
dedicated reflection guide. Both hybrid and PT RR execute successfully without
that optional tag on the recorded device.

Streamline 2.12 and NGX 310.7 are pinned by official archive and payload hashes,
with a reproducible importer and Windows export closure. Live NGX output
confirms SR preset M selection and evaluation (DLSS 4.5), and RR preset D.
These are separate models/features. The default SR preset is not relabelled
4.5. Optional OTA requests and downloaded-plugin selection are disabled; the
production SDK still invokes its bootstrap/update helper. See the precise
[delivery/provenance record](2026-09-08-dlss-lifecycle-delivery.md).

DLSS features drain queued callbacks before context destruction/replacement.
Fresh stationary PT viewports allocate a valid empty motion table. World3D
switching retains the previous world until renderer scenario reassignment.
These last two corrections came from actual resize/world-replacement failures.

The editable [manual gallery](../../demos/rtxdi_manual/README.md) provides keys
1–5 for Hybrid NRD, Hybrid RR, PT NRD, PT RR and raw PT; T selects NRD plus SR.
The pre-existing project configuration and unrelated game assets were preserved.

## Evidence location and builds

Evidence is local at
`C:/Users/lukas/AppData/Local/Temp/godot-pt-step6-20260908`.
Device: RTX 4090, driver 616.64, Vulkan. No driver or NVIDIA App changes.
The `*-world-final.receipt.json` files record actual subprocess exit codes,
before/after source hashes, commands and binary hashes. All three builds return
0 with stable inputs: Windows Vulkan editor, precision=double editor, and
template_debug (`accesskit=no d3d12=no`).
After validation, only surplus terminal newlines were removed from five owned
source files; `terminal-newline-cleanup.json` records before/after hashes and
byte comparison excluding those terminal newlines. The binaries above predate
that whitespace-only cleanup. NVIDIA license bytes remain unmodified, including
upstream trailing whitespace.

| Build | SHA256 |
|---|---|
| Editor | `04c6edaa882c8cb1f215b9bec88f3ce8a364e55816b34e9ee9ccccad33f60fb9` |
| Double editor | `0076f503af6d952dc7150ce3bb095524692f4be586bee64d8d2b934f0e0bfca2` |
| Debug template | `a75228c4937231ee4f441f67ce9235c5fcb6d87da57e266f9a1422acb9f20b46` |

`final/receipts.json` contains 20 successful native Slang/SPIR-V stage results:
PT raygen/miss, DDGI trace raygen/miss and DDGI camera compute, ordinary/double
precision and both sky layouts. Actual camera composition/RR guide shaders are
also exercised by the GPU runs. No automated tests were added or run.

## Observed GPU behavior

- `room-raw-stats`: raw PT reaches 500 accumulated samples at frame 500 with
  unchanged scene signature/history epoch. This rules out history churn in
  that static room; remaining Monte Carlo noise is visible.
- `room-nrd-initial`, `room-pt-rr`, `room-hybrid-rr-trace` and
  `room-hybrid-sr-M`: viewed nonblack room images from the respective paths.
  NGX confirms effective RR D and SR M; SR evaluation reports 854×481 to
  1280×720. The earlier RR trace had a logging bool-format error, subsequently
  fixed; it is not a clean-log receipt.
- `lifecycle-double-near` and `lifecycle-double-1e8`: viewed matching room
  geometry and lighting in independent raw-PT/DDGI viewports at origin and
  translated by (1e8, 0, 1e8). This checks coordinate invariance, not material
  world-coordinate precision or moving DDGI slab retention.
- `lifecycle-world-fixed`: no ERROR lines, exit 0 and captured frame 500 after
  live resize, raw/RR/NRD transitions, World3D replacement, origin translation,
  and object removal/restoration. Earlier `lifecycle-exercise` and
  `lifecycle-exercise-fixed` retain the actual motion-buffer and scenario
  failures; they must not be cited as passing runs.
- `lifecycle-transitions-final`: separate render thread, successful RR and SR
  evaluation on the same viewport at unchanged internal/output dimensions,
  camera cut/return, material color changes and orthographic/perspective
  switches. No ERROR lines, exit 0. Intermediate frame captures include the
  visibly noisy newly reset raw frame after returning to the room.
- `motion-rr_120/125/130`: captures during continuous camera movement, with
  correctly changing camera perspective. Sparse still frames cannot establish
  absence of all temporal flicker or ghosting.
- `demo-pt-rr`: full authored gallery, no ERROR lines, capture succeeded.
  `demo-moving-dual-rr`: animated/deforming gallery and shared-world second
  viewport, separate render thread, no ERROR lines and successful capture.
  Existing explicit unsupported-material and reflection-parser warnings remain.
- The moving DDGI retention/recycling/teleport evidence remains in the
  [Step 5 record](2026-09-07-1701-ddgi-pt-rr-implementation-status.md); the new
  translated-room comparison is not a replacement for those measurements.

Windows export includes Streamline/NGX, Slang and their licenses/manifest.
`export-hybrid-rr-run` executes RR successfully from the exported directory with
VULKAN_SDK/VK_SDK_PATH removed and VulkanSDK entries removed from PATH. It uses
the exported bundled DLLs, without the unused optional DLLs present in the
development bin directory. The initial `export-hybrid-rr` attempt failed because
templates reject scene-path overrides; the subsequent export sets the intended
main scene instead of enabling a development override in shipping code.
The final template is re-exported in `export-world-final`; its
`export-world-sr-M` execution likewise selects NGX preset M and evaluates SR
with the SDK paths removed. The export operation and runtime both return 0.

The gallery's single captured primary-viewport GPU measurement was 2.432 ms
for still PT RR and 2.332 ms with the animated dual-view setup. Reported RD
allocation totals were 611,022,992 and 662,767,328 bytes respectively. These
are observations from those scenes, not benchmark averages, summed multi-view
GPU time, process VRAM, or a promised performance level. Diagnostic readback
runs intentionally stall and are unsuitable for performance comparisons.

## Verification boundaries

This record does not claim exhaustive GPU/material/backend coverage or a
universal visual-quality advantage for DDGI, NRD or RR. RR's optional dedicated
PT reflection-distance producer is absent. Preset L and independent neural
weight hashes were not measured. Stereo remains unsupported. Editor Inspector
interaction, VisualShader hot reload and injected partial-allocation failures
have not been manually exercised in this integration pass. Existing public
serialized properties are retained; this is not a blanket scene-compatibility
certification. Full continuous-motion video analysis and detailed per-pass
timing remain beyond the sparse captures recorded here.
