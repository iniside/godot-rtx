# Shader unification STEP5: final validation evidence

2026-09-07 15:18 UTC, completed observations at 15:38 UTC. Final source/proof
review follows the scoped evidence commit.
Authority: STEP5 of the approved [plan](../plans/2026-09-07-1126-shader-unification-plan.md).
Task baseline `99c67e1f78e67f9fcc4d6a3858b0415f6658d8b2`; migration baseline
`a89cd8a3b064427e29a89af0f0b6823bfac21da7`. Executing agent: core-implementer,
gpt-6-astra, high effort. Comments: default NONE. No engine implementation,
automated tests, assertions, or automated test fixtures were authored by STEP5.

## Evidence location and source identity

Retained artifacts are under
`C:/Users/lukas/AppData/Local/Temp/godot-shader-unification-20260907/step5` (P below).
Selected images, command/output receipts, hashes and exact manual source inputs
are archived in [STEP5 evidence](shader-unification-evidence/step5/artifact-manifest.json),
with [presentation inputs](shader-unification-evidence/step5/manual-presentation-inputs.json).
The isolated presentation project copies the 31 tracked `demos/rtxdi_manual`
inputs, without copying `.godot`; `copied-inputs.json` records their hashes.
Only the isolated configuration and manual presentation assets were changed.
The owner's dirty `demos/rtxdi_manual/project.godot` remains untouched, SHA-256
`41cc7b1055d440b3267640675bd79942fd2ef4fa19aae378592755b3e9fbd582`.

`final-build-provenance.json` records the 70 migration source paths at
documentation revision `12924470328a540ee9b5822212bc688f21e42cdd` and these binaries:

| Binary | SHA-256 |
| --- | --- |
| Windows editor | `2ab863e4372a41b990b2fbdebbdd6421afe81ca3723eebb807b0d3c09b00c414` |
| Windows template_debug | `db68360b5b1a5673e4ea605729402b813ce461caa6d5e068e03db20d38409801` |
| Windows editor, precision=double | `ab8e60b551f3534345ba72c5ba7b4f4930ee8e4b7c3d2afe4ebbbee614b25fc1` |
| Adjacent pinned Slang compiler DLL | `6cd4e6bee3ee8a1f8349f77c838ac711a3d217465771b114f461075b77b71b40` |

The ordinary editor/template banners identify `99c67e1f7`; the double editor
identifies `129244703`. The intervening commit changes documentation, not engine
source. These hashes are retained before the separately owned optimizer fix.

## Builds and shared interfaces

Commands from the repository root were:

```text
scons platform=windows target=editor accesskit=no d3d12=no -j16
scons platform=windows target=template_debug accesskit=no d3d12=no -j16
scons platform=windows target=editor precision=double accesskit=no d3d12=no -j16
```

`editor-build.log` ends with SCons done, elapsed 23.79 seconds. That initial
orchestration retained the log but lost the explicit process-exit receipt; it is
not represented as a recorded exit code. Template and double build receipts
record exit 0. No double template was built. The ordinary template proves the
no-tools link axis; the separate double editor proves the changed precision axis.

`headers-tools` and `headers-no-tools` retain exact MSVC commands, output and
exit-0 receipts for `shared_backend_headers.cpp`, using the actual compile database
command with `/Zs`, with and without `TOOLS_ENABLED`. The translation unit includes
the shared RD/driver/compile-request interfaces and Vulkan/D3D12 shader-container
headers. This is bounded interface compilation, not a full D3D12 backend build or
RT execution. The Metal headers require the unavailable Apple CommonCrypto/simd
SDK; no replacement stubs were supplied. No D3D12/Metal RT claim is made.

## Actual rendering and interactive topology

All successful runtime captures below use real Forward+ Vulkan on the RTX 4090.
Each named run retains argv, executable hash, stdout/stderr and process receipt.
The existing `_capture` helper can exit normally after a failed save; therefore
the saved PNG and `MANUAL_CAPTURE result=0` were checked separately from exit 0.
Images are observations, not a pixel-identity or performance measurement.

`cold-gallery.png` and `warm-gallery.png` retain the ordinary editor gallery.
Parent comparison with the pre-migration/STEP4 gallery sees the expected geometry,
checker, textured emission, materials, wall/floor shadows and sky. The initial
fresh copied project does not by itself prove a cold user-data shader cache.

Six separately launched scene captures retain current source behavior:
`scene-shadows_merged.png`, `scene-shadows_expanded.png`,
`scene-energy_directional.png`, `scene-energy_emission.png`,
`scene-unsupported.png`, and `scene-test.png`. All have exit 0 and saved images.
Parent inspection observes the expected merged/expanded shadow arrangement,
directional/emission fields, unsupported diagnostic boxes and rendered zdm2 scene.
The existing test scene is a manual demo; its animation continues despite the
gallery's `--still` argument. No automated test was run.

The `interactive` run used `main.tscn --dual --fog --upscale`, with motion enabled.
`interactive-dual-fog-fsr2.png` records frame 925. Posted C cuts the camera at
frame 1518; Z resizes to 1024x640 at frame 2057; the next image is frame 2532.
L removes a light at frame 3324, R reorders lights at 3335, B changes shadow state
at 4027, Delete releases the existing runtime deformer at 4039, and V releases
the second viewport at 4051. `interactive-release-lights.png` records frame 4621;
Escape exits normally. These exercise the existing animated CLAS/ordinary-geometry,
multi-viewport, fog, FSR2 and release topology. They do not measure the precise
GPU retirement frame or prove temporal/numerical equality over every frame.

Input uses a verified single visible HWND belonging to the launched PID and
targeted `PostMessageW` WM_KEYDOWN/WM_KEYUP pairs. `input-actions.jsonl` records
queue results; the engine's action/frame logs and changed captures establish
handling. This is posted-window-message operation, not physical keyboard or
SendInput evidence. Foreground activation failed in an earlier attempt, so that
helper refused to send input. No unrelated HWND was targeted.

## Public material frontend and visible reload

The supported presentation is `project/material_manual.tscn`, its GDScript,
`.gdshader`, and `.gdshaderinc`. It inherits the existing directional scene and
puts the shader on the receiver plane. The shader writes ALBEDO, with supported
fragment operations; it does not change the renderer's material support contract.

`material-supported` retains the successful session. The paired upper/lower
swatches exercise a nonidentity `mat3` uniform, matrix/vector and vector/matrix
multiplication, both matrix-product orders, transpose, inverse, mat3-to-mat4
extension and matrix-column indexing through actual Godot shader AST lowering.
The lower swatches contain independently calculated NumPy double arithmetic
constants; `matrix-reference.json` records matrices, vector and expressions.
`matrix-pixel-observations.json` records eight computed/reference pixel pairs from
`material-initial.png`: seven match at 8-bit RGB, while the mat4-extension pair
differs by one green-channel value. This is bounded tone-mapped image evidence,
not proof of full floating-point equality or the cause of that one-value difference.

The same 4x4 runtime texture is bound to nearest/repeat and linear/clamp samplers;
the bottom strips visibly differ. Sequential public operations and saved frames are:

| Input | Public operation | Image |
| --- | --- | --- |
| Initial | Matrix/texture material | `material-initial.png` |
| 1 | ShaderMaterial local uniform | `material-local-uniform.png` |
| 2 | RenderingServer global uniform | `material-global-uniform.png` |
| 3 | GeometryInstance3D instance uniform | `material-instance-uniform.png` |
| 4 | Shader.code adds a uniform and changes ALBEDO, then sets new uniform | `material-source-reload-layout.png` |
| 5 | ShaderInclude.code changes dependency output | `material-include-reload.png` |
| 6 | VisualShader color constant connected to fragment Albedo | `material-visual-shader.png` |
| 7 | Existing VisualShader color constant changes | `material-visual-shader-edited.png` |

Every operation produces visibly changed output; VisualShader changes cyan to
pink. Reload occurs in the same running process without reloading the scene.
Source/include changes use their public code setters; this does not independently
prove an external filesystem watcher or editor text-edit interaction.

Before authoring, clangd navigation located actual implementations and bindings
for Shader/ShaderInclude code setters, ShaderMaterial parameters,
GeometryInstance3D instance parameters, RenderingServer globals, VisualShader
add/connect methods and VisualShaderNodeColorConstant. Their class XML was read,
including module-local VisualShader XML and Albedo output port 0. No public API
was invented or changed.

The first EMISSION presentation rendered the existing magenta diagnostic and is
retained as `unsupported-emission-probe.gdshader` plus `material` logs.
`prepare_material.py` records that initial rejected recipe, not the final supported
shader. The final `material_manual.gdshader` is the authority for the successful
run. `render_forward_clustered.cpp` classifies non-generated custom EMISSION as
unsupported; history identifies that rule before this migration. Prior VERTEX
shader probes are likewise unsupported, so neither is reported as visible custom
vertex-uniform semantics. The existing CPU-updated runtime deformer is covered
by the interactive scene; custom vertex deformation remains outside the supported
RTXDI material contract and visibly diagnostic.

## Double precision and cold/warm identity

The double editor runs `double-cold` and `double-warm`, both exit 0 with saved
images. `double-cold-cache-before.json` records that the unique user-data path
`Godot/app_userdata/Shader unification STEP5 double 1455` did not exist before
the first launch. The actual ShaderRD path is its `shader_cache` subdirectory.
Cold logs record compiler group identities and misses; warm logs reuse them.
`double-cache-after-cold.json` and `double-cache-after-warm.json` record 78 cache
files with unchanged relative paths, sizes and SHA-256. No cache was deleted.
Parent inspection sees the expected gallery in both double images. This is
source-matched double runtime and actual user-cache identity, distinct from the
exported baked-cache check described below.

An additional operator T translates the common gallery root by
`(100000000.25, 0.125, -100000000.75)`, using the verified Node3D global-position
API. The resulting camera position is
`(100000008.25, 6.125, -99999989.75)`. `double-translation-before.png` and
`double-translation-after.png` retain projected geometry but show materially
changed lighting/shadows. This is a precision limitation, not large-origin PASS.

Source attribution used clangd, actual source, baseline `git show` and scoped
history. `RenderRaytracing::build_light_registry` still packs absolute origin into
`RT_LightData::position[3]` floats; its transform array is also float. `build_tlas`
passes absolute instance transforms, and Vulkan's
`_store_transform_transposed_3x4` stores translation in `VkTransformMatrixKHR`
floats. Those contracts are unchanged by this migration (light-registry history
`afbe198fefa`; Vulkan packing `27e4f248004`). Both former GLSL and current native
bridge reconstruct the inverse-view translation into float matrices and use
absolute float receiver/light positions. Float spacing at 1e8 is eight units.
This establishes a pre-existing source vulnerability; no historic executable or
large-origin image was rerun, so exact before/after historical equivalence is not
claimed. No broader world-coordinate architecture was introduced.

## Retained consumers and editor observation

`consumers` renders a separate Node2D presentation with a canvas ShaderMaterial
gradient, UI Label and GPUParticles2D using ParticleProcessMaterial. Public setters,
bindings and XML were read before authoring. `consumers-particles-canvas.png`
contains the gradient/text and orange particles; the log records actual
CanvasShaderRD and ParticlesShaderRD cache misses, capture result 0 and exit 0.
Sky and fog are observed in the gallery; FSR2/post composition is observed in the
interactive run. These are distinct retained GLSL consumers using their glslang
compiler target through shared ShaderRD/RD infrastructure, alongside native
RTXDI/NRD frame shaders compiled with Slang.

Native capture attempts were insufficient: Pillow's owned-HWND capture returned
flat dark frames, and screen readback failed. The isolated manual EditorPlugin
instead observes an operator-written request file and calls public
`EditorInterface.set_main_screen_editor`, `RenderingServer.force_draw(false)` on
the main thread, then reads the base control's viewport texture into an image.
The actual implementations, bindings and XML were checked. Merely waiting for
`frame_post_draw` did not advance the inactive editor; explicit draw provides the
successful readback. This is a manually requested screenshot, with no assertions
or pass/fail automation.

`editor-2d.png` shows the actual 2D editor, scene tree, canvas gradient and text.
`editor-3d.png` shows the actual 3D editor with the gallery's geometry, materials,
lights and shadows. Both runs save with result 0 and close normally, using the
final ordinary editor after the optimizer fix. These are editor viewport
observations, not claims of identical runtime/editor framing or lighting.
The earlier `editor-canvas-final.png` retains the empty 3D editor's magenta grid.
The grid shader at `editor/scene/3d/node_3d_editor_plugin.cpp:1204` has unshaded
render mode, vertex COLOR processing and ALPHA output; its history predates the
migration (`abc38b8d663`, `fcbf7011cc0`). Existing material classification rejects
unsupported shading consistently. This source attribution does not establish
historical image equivalence or add support for editor grid shading.

## Export failure, fix and final package closure

The production Windows exporter was inspected through clangd and actual source:
`platform/windows/export/export_plugin.cpp` copies the pinned compiler DLL and
`slang.LICENSE.txt` from the custom template directory under `MODULE_SLANG_ENABLED`.
The isolated Windows preset enables shader baking and uses the built debug
template. No dependency has been manually copied into the output directory.

`export-bake` launches the ordinary editor with `--editor --path P/project
--rendering-method forward_plus --rendering-driver vulkan --verbose --export-debug
"Windows STEP5" P/export/manual.exe`. It exits with access violation 3221225477
before producing the package. The separately delegated debugger capture identifies
the fault inside `respv::Shader::inlineData` through an exact current COFF-object
byte match; `debug-export-*` retains the dump, input and receipts. The separately
owned [optimizer fix](2026-09-07-1523-re-spirv-entry-variables-fix.md) is commit
`4c24eeefe0007d0802af4937d8c6c6fc82fd4672`; its source review and proof audit pass.
It reserves inlined variables at the caller entry label without replacing the
existing Vulkan specialization optimizer.

A direct template `--path` attempt exits 1 because the built template disables
path overrides. That is a documented target restriction, not a rendering failure;
the required template proof below is the actual exported package.

The fix owner rebuilt all three required configurations, with explicit exit-0
receipts in `optimizer-fix-build-provenance.json`:

| Final binary | SHA-256 |
| --- | --- |
| Ordinary editor | `32e0b1b2a811e18adf9f7a91e9304423bcddf6889df3c90fae71cd1551aac69d` |
| template_debug / exported manual.exe | `003ddb63e7f9b26b310fc92dc8e6d772d907073bdf540f149928e3a2de0651db` |
| Double editor | `0e81fb9ba1c95e52b7ef8f7a4972579c06d8b3019ce5d852520e3d62cf1ff071` |

The final source manifest binds these binaries to the migration sources plus
the recorded re-spirv patch. Their banners retain the earlier build version hash;
source and binary hashes, not the banner alone, identify the fix. The final
ordinary editor is exercised by both editor captures and production bake; final
template by the package runs; final double editor by `double-final.png` (capture
result 0, exit 0). The earlier detailed material/topology/cold-cache observations
retain their explicit pre-optimizer binary identities and are not relabeled as
reruns of the final executable.

`optimizer-fix-export` completes the production shader bake and export with exit
0. `exported-package-identity.json` records EXE/PCK/compiler/license hashes and
the actual PCK v4 directory. There are 161 packaged files, including 104 baked
shader-cache entries. All 104 entry checksums match their actual bytes and all
SHA-256 values match the production baker files under
`project/.godot/exported/4119599170/shader_baker/Windows/vulkan`. Entries include
native RtxdiDiShaderRD/RtxdiFrameShaderRD and retained GLSL consumers. Offline PCK
inspection follows `PackedSourcePCK::try_open_pack`'s current layout; it does not
execute a new test fixture.

The initial `exported-cold` filename is historical: that first package run used
an existing user-data location and is not a cold-cache claim. It saves
`exported-gallery.png` and exits 0. For the controlled baked check, documented
`override.cfg` changes only the application name to a unique user-data directory,
recorded absent before launch in `exported-fresh-before.json`. `exported-fresh`
and `exported-warm` both save gallery images with result 0 and exit 0. Their
actual user ShaderRD cache directories contain zero cache files after both runs,
and neither verbose log reports a shader-cache miss. The package remains unchanged.
This is evidence of baked cache reuse: `RendererCompositorRD` registers
`res://.godot/shader_cache` and `ShaderRD::_load_from_cache` first tries user files,
then those resource files. The group identity includes the compile request,
defines and dynamic buffers. The baker obtains the same relative cache path from
the actual ShaderRD version. These source paths, actual packaged entries and
successful fresh-user runtime together establish the bounded baked identity.

For runtime dependency proof, the same package runs once with the documented
`rendering/shader_compiler/shader_cache/enabled=false` override, which disables
both user and resource shader-cache registration in the template compositor.
The child environment removes `VULKAN_SDK` and its PATH entry. Live module
enumeration in `exported-loaded-compiler.json` records the compiler DLL loaded
from `P/export/slang-compiler.2026.13.1.x86_64.dll`, SHA-256 `6cd4e6be...b71b40`.
The DLL and license came from the production exporter, without manual copying.
`exported-runtime-compile.png` records actual rendered output and exit 0. This
establishes packaged runtime compilation outside VulkanSDK; it does not claim
distribution of every optional NVIDIA SDK feature or execution on another machine.

Warnings include the existing unsupported-material diagnostics and SPIR-V parser
messages (`OpDemoteToHelperInvocation`, forward pointers and acceleration-structure
types), plus twelve unclaimed StringNames reported by verbose exported teardown.
No validation-layer-clean or leak-free claim is made. Applicable taxonomy classes 2–9
cover lifetime, shader/backend contracts, upstream scope, build axes, proof,
crashes/threading, task ownership and scope. No separate-render-thread, exhaustive
material language, custom procedural intersection, D3D12/Metal RT, DDGI,
reflections/transmission, or performance improvement claim is made.
