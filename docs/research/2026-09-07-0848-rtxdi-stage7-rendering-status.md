# RTXDI Stage 7 rendering evidence

Status: implementation and real-device manual rendering closure complete; fresh hostile review and proof audit remain the final acceptance gate.

Task source baseline: `217396c25cc8a54804007c99e7445d35ac00da49`. Parent documentation commit: `eeed72b42987b2d29f9e1a99e2dd57cc5ff91172`. Approved authority: [renderer plan](../plans/2026-09-06-1854-rtxdi-renderer-plan.md), Step 7 and final contract. Executing agent: core-implementer, gpt-6-astra, high; selected for GPU diagnosis and cross-subsystem lifetime work. Comments: default NONE. No automated tests were authored or run.

## Device and demonstration

The real Windows Vulkan runs report Vulkan 1.4.351, Forward+, Streamline, NVIDIA GeForce RTX 4090. Driver 616.64, 24564 MiB device memory. Normal captures use one 1280×720 mono viewport, no MSAA. The owned project is [demos/rtxdi_manual](../../demos/rtxdi_manual/project.godot). Meshes are newly authored glTF source with imported cluster data. No owner `gi_demo`, `rt_test_scenes`, ArcGame, or cake assets were used.

The interactive gallery contains two rows of metallic/roughness spheres, an alpha-scissor checker, a textured emitter, animated transform and blend-shape geometry, directional/point/spot/area lights, a procedural environment, and an off-screen caster. Separate energy and shadow arrangements isolate contributions and winding modes. The importer retains generated mesh LODs to exercise the raster/AS alignment correction. Compression is disabled on the authored mesh import so both expanded and merged MultiMesh representations can be inspected.

Controls: RMB and WASD/QE move; Space pauses animation; C cuts cameras; V opens/closes an independent mono viewport; F toggles volumetric fog; U toggles FSR2 resolution; L removes the oldest light; F12 saves a screenshot; Escape closes. Command-line capture options select a scene/light and save one real viewport image after a delay. These are manual presentation controls without test assertions or pass/fail logic.

## Confirmed integration defects and corrections

| Defect exposed by the real run | Corrected authority and closure |
|---|---|
| Compositor construction dereferenced camera-attribute storage before allocation. | `RenderingServerDefault::_init` allocates camera attributes before `RendererCompositor::create`. Existing `_finish` releases it after successful or partial initialization. |
| NRD frame shader compiled a zero-length directional-light array because LightStorage's runtime maximum was read before initialization. | `NRDEffect` uses the authoritative `RendererSceneRender::MAX_DIRECTIONAL_LIGHTS` constant, also used when LightStorage initializes its bound. |
| Compute DI rejected the bindless descriptor layout finalized against a ray-generation shader. | `RenderRaytracing::get_bindless_uniform_set` finalizes the existing block against the actual DI compute shader; the obsolete ray-generation finalization and cached RID are removed. |
| re-spirv deleted loop-back-only SSA definitions in valid pinned NRD SPIR-V. | Recorded vendor patch separates skipped loop-Phi liveness uses from the acyclic topological dependency graph. `Shader::sort` includes those uses in out-degrees. The patch is documented in `thirdparty/README.md`; imported sources were changed through `git apply`, not hand-edited. |
| A visible directional light produced a zero-light GPU registry. | `RendererSceneCull::_render_scene` appends directional lights to the RT list as well as the ordinary light list. |
| Adding the environment to local/infinite lights amplified energy dramatically. | `rtxdi_sample_initial` combines finalized SDK and environment reservoirs into a new empty reservoir, then finalizes once, matching the pinned SDK's own combination sequence. |
| Smooth spheres acquired bands of false self-shadow. | `_fill_render_list` uses base geometry for `PASS_MODE_RTXDI_SURFACE`, matching the full BLAS. Secondary shadow-map/tool LOD selection remains available. No ray bias was changed. |
| Deformed BLAS cleanup freed a stale RID after source mesh index-buffer destruction recursively invalidated the BLAS. | An unbound, render-thread-guarded RD AS-validity query is used by deformed cleanup/TTL eviction; stale entries reset before rebuild/refit decisions. Actual deformer/source removal and shutdown produce no invalid-RID report. |
| Separate rendering-thread shutdown left RD owned by the joined worker; later Windows display teardown failed the RD thread guard. | `RenderingServerDefault::finish` returns RD ownership to main after joining the worker, using the existing null-guarded `make_current` method. Ordinary separate-thread shutdown is clean. |
| The old opaque pass wrote reactive alpha zero, while new HDR composition writes alpha one. | The opaque-only FSR2 dispatch consumes the existing black reactive texture. HDR alpha and viewport transparency semantics are unchanged. Old authority: `scene_forward_clustered.glsl` applies `pass_alpha_multiplier`; `RenderSceneDataRD::update_ubo` supplies zero for opaque render buffers. |
| Stationary geometry retained its initial previous transform indefinitely after the PT prepass caller was removed. | `_render_scene` calls the existing `age_out_motion` before instance uploads for raster and RT-only lists. Its frame guard preserves idempotency across independent viewports. Stale prepass comments are removed. |
| An enabled anisotropic material escaped the unsupported-material diagnostic. | `ShaderData::set_code` no longer overwrites the `ANISOTROPY` usage destination with `uses_tangent`; both anisotropy inputs set `uses_anisotropy`, and the existing final OR retains tangent requirements. |

The LOD result has a cost: opaque primary visibility currently uses base meshes, because the retained acceleration structures use base geometry. Independent raster-only simplification would change the visible surface relative to visibility rays. Shared geometry selection remains subsequent renderer work, not a second path added here.

## Diagnostic observations

Initial failed images are deliberately retained as failures. A nonblack frame was not accepted as correctness. The first ordinary gallery had saturated walls/floor and patterned spheres. Individual light captures were bounded, isolating the combination error. The corrected mixed-light image gave wall HDR `(0.1255, 0.0724, 0.07965)` and floor HDR `(0.2715, 0.2512, 0.1721)` at the recorded pixels, versus approximately `(7.28, 2.543, 10.33)` and `(5.246, 2.424, 3.877)` before correction. Those are diagnostic scene samples, not general numerical reference tolerances.

The sphere artifact disappeared with shadow tracing disabled, then also disappeared with shadow tracing enabled and the existing viewport LOD threshold set to zero. The production LOD correction reproduces smooth surfaces with the default viewport threshold. Surface shading/geometric normals were consistent; the NRD packed roughness sign change across a nearly zero normal Z decoded correctly and was not changed.

### Directional and emission energy

The directional arrangement uses a top-down camera over a plane, sRGB albedo `(0.5, 0.5, 0.5)`, zero dielectric specular, one unit directional light, linear tonemapping, and no sky. Raster albedo quantizes to linear `55/255`. Before the culling correction the GPU registry reported total/infinite/SDK light counts all zero despite one configured infinite candidate. After correction, raw demodulated diffuse at the center was approximately `0.9307`, RELAX diffuse `0.9287`, specular zero, and the final PNG gray was 127/255. The nonphysical directional energy uses π, canceling Lambert's 1/π; material recomposition yields the expected linear albedo rather than doubling direct light.

The emission arrangement uses black albedo and authored sRGB emission `(0.125, 0.25, 0.5)`. The actual surface emission and composed HDR agree exactly at the inspected center: half-float RGB approximately `(0.01434, 0.05087, 0.214)`. Both raw and denoised direct lighting are zero. Final PNG RGB is `(32, 64, 127)`. The sRGB-to-linear conversion explains the HDR values; emission is added once.

### Optimized shader bytes

The frozen failing module is `RELAX_HitDistReconstruction.cs.hlsl|NRD_SIGNAL=BOTH|NRD_MODE=RADIANCE|MODE_5X5=0` under `%TEMP%/rtxdi-step7-optimizer-failure`:

| Bytes | SHA-256 | Vulkan 1.3 spirv-val |
|---|---|---|
| Original pinned input | `681D0771CDAE5F6DC4F38E7DF59C22CA9EAC6A7DB5E707AA87845EB077680F5C` | Pass |
| Decoded/stripped pre-optimizer | `318C1D13E8432A1E297BB0FD23DDE832D8E26A30A55F341DF12637F782EB0B3C` | Pass |
| Faulty optimized output | `E0836C09CE40DF0A3E3D13620A382F46E2FFE334CB2C8EBF9BD4E60B5DA37426` | Fail: undefined IDs 307, 310, 312, 314, 316, 327 |
| Corrected optimized culprit | `318C1D13E8432A1E297BB0FD23DDE832D8E26A30A55F341DF12637F782EB0B3C` | Pass |

All 15 distinct ordinary Vulkan NRD optimized modules captured under `%TEMP%/rtxdi-step7-nrd-modules` passed `C:/VulkanSDK/1.4.357.0/Bin/spirv-val.exe --target-env vulkan1.3`. The corrected culprit also received an independent parent validation. No global optimizer disable or NRD-specific bypass is retained.

### RID attribution

Temporary Windows stack capture at RD's invalid-free branch mapped the caller through the `/MAP` diagnostic link artifact: `RenderRaytracing::cleanup_caches + 0x4eb`, immediately after freeing the deformed entry's BLAS. RD's BLAS creation registers source vertex/index dependencies. MeshStorage frees the source index buffer before its later resource notification, recursively freeing the dependent BLAS while the RT cache still holds the RID. The stack hook was removed after attribution. The correction checks the actual RD owner rather than treating a nonzero RID as a live AS.

### Temporal motion and FSR2

The stationary FSR2 arrangement renders internally at 858×483 and outputs 1280×720. At frame 120 its jitter is `(0.000793, -0.000204)`, previous jitter `(-0.000967, 0.001577)`, and history is valid. Before restoring object aging, floor motion at `(429,350)` was `(-6.742, -93.3125)` pixels with depth delta `5.2969`, despite a stationary scene. The retained previous transform was the initial identity. Clangd references found no remaining caller of `age_out_motion`.

After restoring that call, the stationary floor patch `[x=300:600,y=300:400]` reports XY minima `(-1.276e-5,-2.879e-5)` and maxima `(3.833e-5,1.436e-5)` pixels, with depth delta exactly zero. The same static FSR2 capture loses the large stair steps and sphere crosshatch. The black reactive correction alone did not resolve that symptom; it is a separate source-backed opaque-mask contract correction. No FSR2 jitter algorithm or SDK tuning was changed.

The corrected raw diffuse and RELAX diffuse visualizations use the same captured frame, exposure mapping `pow(max(x,0)/(1+max(x,0)),1/2.2)`, and no spatial filtering. They show actual noisy direct-light samples becoming stable diffuse shading. These are demodulated inputs/outputs, not final material color. Smooth metals remain dark in this diffuse-only visualization.

### Manual interactions on the ordinary editor binary

After removing all private instrumentation, the real visible demo received window keyboard input for movement, camera cuts, pause/resume, both resize directions, independent viewport creation/destruction, fog, FSR2, light reordering/removal, off-screen caster visibility and deformer/source removal. The log records each action and frame. Escape closed the process without invalid-RID, thread-guard, or leak reports. A small temporary Win32 helper addressed only the known demo process/window; it was an input device substitute, not a test or assertion harness.

Paired stationary captures show the off-screen box's wall shadow disappearing when hidden and returning when visible. The orthographic shadow wrapper's B control removes all analytic visibility shadows while retaining the visible surfaces. The case table and expected mirrored/cull-mode topology are in the demo README. Scene wrappers expose the same cases through editor F6; F5 runs the gallery.

## Final ordinary builds and rendering

All private readback, shader dumping, deliberate crash and extra copy-usage instrumentation is removed. Final source and executable SHA-256 hashes are in [manifest.json](rtxdi-stage7-evidence/manifest.json). The embedded Git version remains the parent documentation revision because these builds precede this task commit; it must not be mistaken for the compiled source revision. The manifest identifies the exact owned source bytes that the task commit lands. The final anisotropy fix is included in both final targets.

| Build command from repository root | Result |
|---|---|
| `scons platform=windows target=editor accesskit=no d3d12=no -j16` | Clean instrumentation-free build 31.73 s; final anisotropy rebuild 32.41 s, success. |
| `scons platform=windows target=template_debug accesskit=no d3d12=no -j16` | Default template build 44.45 s, success. Its default path-override restriction correctly rejects direct project loading. |
| `scons platform=windows target=template_debug accesskit=no d3d12=no disable_path_overrides=no -j16` | Development template build 259.74 s, success; this existing SConstruct option permits direct project execution. |

Final template runs load the gallery, `shadows_expanded.tscn`, `energy_directional.tscn`, `energy_emission.tscn` and `unsupported.tscn` from the owned project, using the real Vulkan device. Gallery/expanded/energy/unsupported runs use the normal rendering thread; the animated dual-view fog/FSR2 run uses `--render-thread separate`. Each recorded final CLI run exits zero without an `ERROR` report. Final editor FSR2 uses the separate rendering thread and exits zero. Earlier ordinary editor interaction and merged-shadow runs cover both normal and separate modes. The final energy centers are gray `(128,128,128)` and emission `(32,64,127)` in the PNGs; the one-level directional difference from the earlier diagnostic capture is quantization/convergence, not another lighting contribution.

Canonical command shapes are recorded in the [evidence index](rtxdi-stage7-evidence/README.md); final captures and compact runtime/build logs are adjacent. The final unsupported scene visibly marks all three custom-light, anisotropy and blend cubes, with explicit compiler/material diagnostics. Supported gallery startup also reports construction of an unsupported inline shader and re-spirv's `OpTypeForwardPointer` limitation; these are warnings/fallback-parser messages, not errors or a claimed validation-layer-clean run.

Actual named GPU timestamps in [pass-timings.json](rtxdi-stage7-evidence/pass-timings.json) give these five-report medians for the static native-resolution template gallery: TLAS 0.0467 ms, surface 0.0289 ms, guides 0.0229 ms, direct lighting 0.7582 ms, RELAX/HDR 0.2571 ms. The dual/fog profile also records BLAS updates, retained analytic shadow maps, clustered fog, FSR2 and tonemapping. These are the engine profiler's reported timestamp intervals, not a locked-clock benchmark or additive FPS promise. No retired forward opaque lighting or PT pass appears in these profiles; the current `_render_scene` dispatches surface/DI/NRD composition as its lighting path.

| Presentation | Internal/output sizes | Total / texture / buffer bytes at capture |
|---|---|---|
| Static template gallery | 1280×720 / 1280×720 | 454843296 / 358092288 / 84067558 |
| Static editor FSR2 | 858×483 / 1280×720 | 343157664 / 287229440 / 42982630 |
| Animated template dual + fog + FSR2 | Primary 858×483 / 1280×720; child 384×216 native | 413041856 / 344967680 / 52756694 |

Allocation values use the supported Performance video/texture/buffer monitors and include the renderer's scene-wide allocations, NRD pools, DI resources and miscellaneous overhead. They are not process-wide VRAM residency or per-pass allocation attribution. Native rendering without temporal AA retains ordinary pixel stair steps; moving low-roughness highlights and shadow boundaries retain finite-sample noise. The former false self-shadow bands, excessive mixed energy and stationary temporal crosshatch are corrected. This demonstration is not an open-world scalability or long-duration memory benchmark.

Navigation used clangd against the root compile database for named C++ methods, then targeted implementation/header reads, ClassDB/XML checks for the demo surface and git history for fork-local authority. GLSL and imported SDK contracts were read directly; bounded text inventories were only lower bounds. The new AS-validity query is intentionally unbound and adds no ClassDB/XML surface. Applicable failure-taxonomy classes: 1–9, with primary risks in RID lifetime, RD/shader synchronization, render-thread ownership, imported optimizer authority and proof provenance. Partial compositor-init cleanup is source-inspected; no unsupported-device hardware failure was fabricated.

Fresh exact-commit hostile review and proof audit remain for the parent to record before final acceptance.

Unsupported-device hardware coverage is unavailable on the sole RTX 4090 device. No simulated failure is presented as real unsupported-device initialization. Earlier Vulkan validation captured Streamline device-feature and swapchain VUIDs separately; those runs do not establish validation-layer cleanliness. D3D12/Metal, multiview, MSAA, secondary reflections, DDGI, glass and transmission are outside this accepted Vulkan mono DI milestone.

Bulky raw texture readbacks, SPIR-V and full build logs are local diagnostic artifacts in `%TEMP%`; they are not build output committed to the repository. Selected PNGs, compact logs, hashes and final provenance are retained in `rtxdi-stage7-evidence`. The temporary capture path used existing renderer textures with private copy/readback instrumentation and is absent from the final production diff.
