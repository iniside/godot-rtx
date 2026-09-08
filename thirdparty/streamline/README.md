# Pinned Streamline / DLSS delivery

Run `python misc/scripts/update_streamline.py` from the repository root before a
Windows x86_64 Streamline build. The importer validates the entire payload before
installing it into `thirdparty/streamline/runtime` and `bin`. SCons validates the
runtime manifest and copies that same closure beside editor/templates. Windows
export copies this closure, licenses and manifest beside the exported executable.
No Vulkan SDK runtime is required by this payload.

* Streamline production plugins and headers: [v2.12.0](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.12.0),
  source `e8aaa6eaac968711fb62473d4ae8256dde20919b`.
* Official release archive SHA256:
  `f5c0a3d870707dddc3570fb4bcd3655cf48a8a68c3a9d342910cfa21b77dcf48`.
* NGX production DLLs: [DLSS 310.7.0](https://github.com/NVIDIA/DLSS/releases/tag/v310.7.0),
  `a291cc7d2cc642a51566f3dfd5376f635cd1b284/lib/Windows_x86_64/rel`.
* Individual DLL/license SHA256 values: `runtime_manifest.json`.
* Local imported DLLs reported valid Authenticode signatures on 2026-09-08;
  NGX file versions were 310.7.0.0, Streamline plugin versions 2.12.0.0.

Runtime binaries are downloaded artifacts, not generated C++ source. Do not
replace them with files from an NGX user cache. The importer uses no driver or
NVIDIA App changes. The engine does not request optional OTA updates and disables downloaded-plugin
loading. Streamline production binaries still invoke the NGX bootstrap/update
helper independently of that preference. These flags do not disable all updater
activity or the separate NGX model-cache search. The 2026-09-08 hybrid RR trace
explicitly selected the bundled SR/RR DLLs; inspect each validation run rather
than infer effective payload solely from preferences.

The SR SDK offers L/M, the second-generation transformer presets associated
with [DLSS 4.5](https://www.nvidia.com/en-us/geforce/news/dlss-4-5-super-resolution-available-now/).
`?` retains SDK defaults: K for DLAA/Quality/Balanced, M for Performance, L for
Ultra Performance. Explicit SR `M`/`L` selects those models through the app API.
RR is a separate feature: its supported transformer presets are D/E; its F and
later enum entries revert to the default. RR F is not evidence of SR 4.5.

This establishes official redistributable provenance and model-selection APIs,
not by itself the model actually executed by a GPU. The 2026-09-08 hybrid RR
trace selected RR preset D with NGX 310.7.0 and driver 616.64 (see the dated
lifecycle evidence). A subsequent ordinary-SR trace confirms NGX selected the
application preset M at Quality and successfully evaluated SR310.7.0. L was not
separately exercised. Record driver, selected quality/preset,
loaded module paths/hashes and NGX diagnostic model output during manual GPU
validation. A preexisting driver override may still affect effective behavior.
The final exported-project run outside VulkanSDK and motion-image quality remain
required; none was performed by the importer.
