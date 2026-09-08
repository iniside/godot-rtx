# DLSS SR/RR lifecycle and SDK delivery

Implemented source contract on 2026-09-08:

* `create_context(internal, output, ray_reconstruction)` selects SR or RR optimal
  settings independently; `is_available(rr)` checks both adapter support and
  required entry points. `is_ready(context, rr)` includes warmup/configuration.
* `Parameters::dlss_rr` selects exactly one evaluation. Missing RR support or
  required albedo/normal guides reports an error instead of evaluating SR.
* Feature changes drain queued/GPU work before disabling/freeing the previous
  NGX feature, then recompute settings and force the first history reset.
  Context destruction frees the feature before recycling its viewport ID.
  Forward+ must drain before invalidating guide RIDs, as the queued callback
  resolves them during recording. This is a rendering-thread lifecycle contract.
* Output is declared as a storage image write with Vulkan GENERAL/D3D12 UAV
  state. Input exposure is declared and tagged. Missing optional reflection
  distance and motion tags are explicitly cleared, preventing stale PT tags
  when hybrid reuses a viewport. SR clears all formerly tagged RR guides.
* Hybrid reflection-tag omission remains a real-device validation obligation.
  NVIDIA's guide describes reflection motion or distance; API tagging alone
  does not prove NGX accepts our reflection-free hybrid producer.

Sources: [pinned RR guide](https://github.com/NVIDIA-RTX/Streamline/blob/e8aaa6eaac968711fb62473d4ae8256dde20919b/docs/ProgrammingGuideDLSS_RR.md),
`thirdparty/streamline/include/sl_core_api.h` (flush before freeing), and
[package provenance](../../thirdparty/streamline/README.md).

Verification: inspected source/API contracts; ran the pinned import, verified
archive/per-file SHA256, read PE versions and valid Authenticode status for all
12 delivered DLLs. No automated tests, engine build, feature evaluation, motion
capture or exported application run was performed for this subtask. Successful
import is not proof of effective SR 4.5 or RR rendering.


## Integrated hybrid RR trace, 2026-09-08

The root agent captured
`C:/Users/lukas/AppData/Local/Temp/godot-pt-step6-20260908/room-hybrid-rr-trace.log`.
Read-only inspection established:

* Line 31 rejects downloaded Streamline plugin loading; the plugin path is the
  repository `bin` and loaded plugins report 2.12.0 PRODUCTION.
* Lines 655 and 928 identify selected `bin/nvngx_dlss.dll` and
  `bin/nvngx_dlssd.dll`, respectively, both version 310.7.0. Read-back SHA256 of
  both DLLs plus `sl.dlss_d.dll` and `sl.common.dll` matches the pinned manifest.
* Lines 2026-2041 identify actual NGX RR preset D for all quality modes.
  Line 3038 reports evaluated feature version 310.7.0; line 3041 driver 616.64.
* The initial application success trace failed formatting at line 2287 because
  Godot requires an integer for `%d`, not a boolean. This logging site is after
  successful `slEvaluateFeature`, but the malformed record is not a clean
  success artifact. The bool-to-int correction is awaiting a new capture.
* Line 27 records a real NGX updater invocation with `-api update -bootstrap`.
  Disabling `eAllowOTA` does **not** disable every update operation: production
  [pluginManager.cpp](https://github.com/NVIDIA-RTX/Streamline/blob/e8aaa6eaac968711fb62473d4ae8256dde20919b/source/core/sl.plugin-manager/pluginManager.cpp)
  lines 937-944 calls `checkForOTA` unconditionally and uses that flag only for
  optional updates. `eLoadDownloadedPlugins` is separate. Thus the precise
  application policy is no optional-update request and no downloaded-plugin
  loading, not a claim that NVIDIA updater activity is disabled.

NGX also searches its model cache; this capture reports cache misses and selects
bundled SR/RR DLLs. Bootstrap activity alone neither proves a runtime replacement
nor establishes a harmless catalog-only operation. No vendor binary, driver,
NVIDIA App setting or cache was modified to suppress it. Effective SR L/M (4.5)
selection still requires a separate ordinary-SR trace, because RR uses its own
model. The root owns visual/motion and export validation results.


## Integrated SR preset M trace, 2026-09-08

The root agent subsequently captured `room-hybrid-sr-M.log` in the same evidence
folder. Line 655 selects bundled `bin/nvngx_dlss.dll` 310.7.0. NGX's own
`NgxDltss::FillCreationParams` at line 2047 reports `(Quality) Using App hint
Preset M`; lines 2044/2050/2053/2056 report the same for the other selectable
quality modes. Thus preset M selection is confirmed by NGX, not merely by the
application's request or the DLL filename.

Line 2065 records successful SR evaluation: viewport 1, feature ID 0, requested
preset M, actual API enum 13, quality mode 3 (Quality), 854x481 -> 1280x720,
reset 1, `sl::eOk`. Line 2772 reports evaluated NGX feature version 310.7.0.
Together with the official pinned DLL provenance and NVIDIA's preset mapping,
this verifies the DLSS 4.5 SR preset-M selection/evaluation path on this device.
No separate neural-network weights hash is exposed by the inspected log. The
application diagnostic deliberately keeps `effective_model=unknown` because it
cannot infer NGX internals; this manual conclusion additionally uses NGX's own
preset-selection message. L was not separately exercised.

The earlier RR-default-D messages in this SR log describe shared creation
parameters, not an RR evaluation. The actual evaluated feature is explicitly SR.
Exported runtime behavior and temporal image quality remain separate evidence.
