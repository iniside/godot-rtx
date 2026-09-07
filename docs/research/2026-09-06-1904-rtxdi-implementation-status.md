# RTXDI implementation status

Owner approval: 2026-09-06, "zaczynaj".
Approved plan commit: `e02ea8c87dbad71ca5db85cbba605c47e9825477`.
Implementation task baseline: `33b555c7f225afd1e586bf749f2b485c1de260df`.
Plan: [RTXDI replacement](../plans/2026-09-06-1854-rtxdi-renderer-plan.md).

## Current stage

Step 1 landed in `89b35e1d1c287bc3e68ac289c2aede7363f08dc3`; its fresh hostile
review returned PASS on 2026-09-06. Step 2 landed in
`ee7ae4e52888eba32b1e2b62c93cb64bab4aebaa`; its first fresh review returned
REJECT for missing bindless descriptor-indexing preflight. Scoped fix
`de61204e58d62a8aee25fa3a4fee5a836a7fc0ce` adds an internal RD feature backed by
the four Vulkan descriptor-indexing bits and a compositor preflight check.
Fresh round 2 review returned PASS on 2026-09-06. The fix has a clean staged diff check but no
separate compile yet; Step 3 owns concurrent renderer edits.
Step 3 landed in `afbe198fefaa10ab4679cffa15e258106d4efdfb`; first fresh review
returned REJECT for four concrete defects: shared shader binding collisions,
missing area-light range attenuation, inconsistent environment PDF re-evaluation,
and inclusion of SKY_ONLY directionals in scene direct lights. Scoped fix
`1c31998c6f0ad703f39c879f2f1cbc9b83004f52` closes those findings; fresh round 2
review returned PASS on 2026-09-06. No additional identity/lifetime defect was
confirmed by either round.
Step 4 landed in `c52c9519259323e4795c3359605858e27a6c70af`; first fresh review
returned REJECT for missing previous-depth history, missing authoritative camera
cut invalidation, and parameter-name-based standard-material classification that
allows unsupported procedural emission. Scoped fix
`4f340213e8946d5de5a50bf1758b17b646c2e55e` adds per-set depth snapshots,
authoritative camera identity and generated-material provenance. Fresh round 2
returned REJECT: the new shared `RendererSceneRender::render_scene()` camera RID
parameters were not added to GLES3/dummy overrides. The three round-one behavioral
findings are closed. Seven focused C++ translation units passed clangd checks;
scoped diff checks passed. No full build or GPU verification was performed.
The owner authorized the remaining signature fix with "to popraw" on 2026-09-07
local time (2026-09-06 UTC). Commit
`826dc09a5a259b0f6478c364c126e911df2f79e5` adds the two camera RIDs to the GLES3
declaration/definition and dummy override. Both affected translation units passed
MSVC `/Zs` syntax compilation using root compile-database flags. Fresh bounded
review of this owner-requested correction returned PASS on 2026-09-06 UTC at
`6e539007f842b9cfd2ce845e8a99636af176e7f8`. The last reported Step 4 defect is
closed; this is not full build or runtime validation.
Step 5 resumed on owner instruction "zacznij kolejny krok" (2026-09-07 local,
2026-09-06 UTC), from `3e1d7ee4356c655634dba5d0f391d985e9dbf741` with the preserved
partial work. A fresh core-implementer uses `gpt-6-astra`, high effort, because
this step combines ReSTIR algorithm integration with GPU resource lifetime and
dispatch synchronization. Step 5 landed in
`5f9177d425f4f1d8241c554bf4816bb12f523ff1`; fresh review returned REJECT on
2026-09-06 UTC for three confirmed surface/coverage/reprojection defects listed
below. After the requested stop, the owner authorized "popraw i kontynuuj" on
2026-09-07. Corrective commit `5a532d08b36a4d262a7d372dd50f4c81b151fefa`
implements those fixes. Fresh final round 2 review returned REJECT on 2026-09-07
for shadow-facing semantics; it confirmed the three earlier findings are closed.
The owner subsequently authorized "no to popraw te cienie" on 2026-09-07.
A bounded shadow-facing correction landed in `29a810ad370a3cd3d4519040e2973f9dbabe9d01`
from `1868d9649f83ed9606b453c7b3817f6d0d745e65`; fresh bounded review returned
PASS on 2026-09-07. The remaining reported shadow-facing defect is closed at the
source/compile boundary.
Step 6 NRD/HDR began from that
corrective commit in a separate core-implementer context (`gpt-6-astra`, high
effort for GPU lifetime, pinned SPIR-V integration and frame composition), then
paused with its unwired partial source preserved. It resumed after the shadow
correction passed review and landed in `a661887676ce3c1e1f51ecb1a44bb9c2483627e7`.
Fresh Stage 6 round 1 review returned REJECT for sky-light exposure and
PRE_OPAQUE guide ordering. Corrective commit `217396c25cc8a54804007c99e7445d35ac00da49`
implements both fixes; fresh final round 2 review returned PASS on 2026-09-07.
Stage 7 landed in `c8ba1736e1a67fad02bb20d85b6328fab0c3d51b`, from
`217396c25cc8a54804007c99e7445d35ac00da49`, executed by core-implementer
(`gpt-6-astra`, high). Editor/default-template builds and actual Vulkan rendering
on RTX 4090 passed the recorded manual scenarios. The development template used
`disable_path_overrides=no` to load the demo. Final source/binary hashes, images,
timings, limitations and removed diagnostic provenance are in the
[Stage 7 evidence](2026-09-07-0848-rtxdi-stage7-rendering-status.md).

Fresh hostile review round 1 and the required proof-auditor have NOT run.
The runtime rejects fresh spawns with `agent thread limit reached` even after all
three child contexts report completed; interrupting the completed writer does not
release a slot. No reviewer was reused and no PASS was fabricated. Continue in a
session able to create fresh contexts: review exact `c8ba1736e1` and cumulative
`217396c25c..c8ba1736e1`, then audit its evidence. Two review rounds remain available.
The owner-requested old-scene migration below follows closure of this gate.

Step 1 evidence: pinned importer completed 159 NRD SPIR-V tasks; a temporary
native GLSL reservoir/random-sampler closure passed glslangValidator Vulkan 1.2.
This did not cover the later complete RAB/DI passes. SCons built 15 imported
host translation units and linked the editor/console executables with
`platform=windows target=editor accesskit=no d3d12=no -j16`. The disabled optional
SDKs were missing on this machine. These are compile/link results, not GPU proof.

The independent review verified pinned upstream files and the recorded RTXDI
patch, all 159 embedded SPIR-V variants and their binding offsets, and all 15
host objects in the linked archive. It found no defects within Step 1's scope.
Full DI shaders, template and real-device dispatch remain later-stage gates.

Step 2 evidence (2026-09-06): the same Windows Vulkan editor build command
completed and linked both binaries after initialization, ownership and API
replacement. Six changed XML files parsed successfully; staged diff check passed.
This does not prove startup failure behavior or GPU rendering. The excluded owner
file `rt_test_scenes/capture.gd:28,30,31` still references removed PT properties;
it was not migrated. Step 7 uses a new owned demonstration project.

Step 3 evidence (2026-09-06): scoped staged diff check passed, and clangd checks
of the changed translation units reported no compiler diagnostics. This is not
a full compile/link or shader validation. The commit replaces camera-bounded RT
population and the 64-light packing with resident scene collection, per-viewport
current/previous light snapshots and invalidating remaps. Shared sampling is in
`shaders/raytracing/rtxdi_light_sampling_inc.glsl`. It restores bindless finalization
after geometry/material/light texture registration. Real-device correctness of
stable identities, PDFs, deformation and resource lifetime remains unverified.

Step 3 fix evidence (2026-09-06): narrow SCons shader-header generation passed;
flattened closest-hit default and ray-query-shadow variants passed glslangValidator
for Vulkan 1.3, with bindings 13–16 each declared once. The shared sampling include
now declares no descriptors. A GLSL-reserved local name found by the diagnostic
was also corrected. Changed-line clangd reported zero errors; scoped diff checks
passed. These are shader/source diagnostics, not full renderer or GPU proof.

Step 4 evidence (2026-09-06): clangd parsed both changed C++ translation units
with exit zero; static assertions lock the changed InstanceData offsets; shader
conditional nesting and scoped staged/committed diff checks passed. This is not
a full surface-shader compile or GPU verification. The commit adds the six-MRT
surface variant, two-set viewport history and motion, and visible diagnostics for
unsupported materials. It removes normal-view legacy opaque lighting/GI and
blended drawing. Fog shadows and reflection capture remain for Step 6 closure.

Commit metadata correction: Step 4 fix `4f340213e8` was executed by
`core-implementer (gpt-5.6-sol)`. Its message accidentally contains literal `\n`
sequences on one physical line, so Git does not parse the intended body/trailer.
The original message still contains the truthful identity. History was preserved;
this entry records attribution and the formatting defect without claiming it was
rewritten or that code validation covers commit-message formatting.

## Resume point

Step 5 first review target: `5f9177d425f4f1d8241c554bf4816bb12f523ff1`.
Round 1 findings at that historical target:

- P1: export an actual oriented triangle geometric normal. The gate at
  `shaders/raytracing/rtxdi_application_bridge_inc.glsl:265` consumed
  interpolated shading normal from `shaders/forward_clustered/scene_forward_clustered.glsl:1369,2905`.
  Smooth shading can admit light below the actual triangle plane.
- P1: preserve material filter/repeat/coverage sampling semantics in ray visibility
  and emissive coverage. `rtxdi_application_bridge_inc.glsl:328` used linear,
  repeat and LOD 0 regardless of the standard material; a clamped raster hole can
  therefore cast an opaque shadow. Raster authority: `scene/resources/material.cpp:702,732`.
- P2: add jitter displacement to the motion passed to SDK temporal reprojection.
  `rtxdi_di.glsl:201` forwards motion from which raster removed jitter; the sampled
  previous buffer requires `(previous_jitter - current_jitter) * 0.5 * viewport_size`.
  Raster producer: `scene_forward_clustered.glsl:2896-2904`; SDK consumer:
  `Rtxdi/DI/TemporalResampling.hlsli:64`.

The review confirmed reservoir rotation, descriptor/BDA dependencies, full masks,
PDF measures, NRD factors and resource lifetime at the reviewed source boundary.
It checked the linked-editor log and shader artifacts, but did not run GPU code.
These findings were initially reported without implementation at the owner's
requested stop; the later "popraw i kontynuuj" instruction authorizes their fix
and continuation through the remaining approved stages.

Correction evidence, 2026-09-07 at `5a532d08b3`: the raster producer exports the
oriented deformed triangle plane separately from the shading normal. Parsed
material texture metadata selects the viewport's existing sampler palette at
bindings 33–44; shared ray/emitter alpha coverage uses projected-triangle UV
gradients. The 112-byte material layout is unchanged. Jitter displacement is
added only at the RTXDI temporal consumer, preserving non-jittered NRD motion.
The patch also excludes the surface variant from the legacy motion-output block
after actual surface shader compilation exposed that collision.

Four DI compute variants and specialized/UBERSHADER surface vertex/fragment
variants compiled for Vulkan 1.3. The surface diagnostic used an empty material
code scaffold; it is not coverage of all generated standard-material variants.
The Windows editor and console linked in 34.81 seconds using
`scons platform=windows target=editor accesskit=no d3d12=no -j16`.
Evidence artifacts: `%TEMP%/rtxdi-step5-fix-glsl/compile.log` and
`%TEMP%/rtxdi-step5-fix-build-final.log`. These are compilation results only;
real-device execution remains unverified.

Final round 2 result, 2026-09-07, frozen `5a532d08b3`, cumulative
`41bcc80c4..5a532d08b3`: REJECT for one P1 shadow-facing defect.
`shaders/raytracing/rtxdi_application_bridge_inc.glsl:406–409` uses no facing-cull
flag and confirms every covered candidate. A single-sided plane hit from its
culled side therefore blocks DI even with ordinary `SHADOW_CASTING_SETTING_ON`.
`doc/classes/GeometryInstance3D.xml:97–104` distinguishes this from `DOUBLE_SIDED`;
the retained raster shadow path honors the distinction at
`forward_clustered/render_forward_clustered.cpp:521`. The required correction
must respect material-facing shadow culling and the explicit double-sided
override for mesh and MultiMesh instances; a global cull flag alone is not enough.
The two-round review stopped the prior task. The three round-one findings are
closed; the owner's subsequent "no to popraw te cienie" authorizes a separate
bounded correction of the remaining shadow-facing defect. Commit `29a810ad37`
exports material culling and the explicit double-sided override in geometry
metadata, filters candidates from the light side, and corrects expanded MultiMesh
facing for negative local determinants. The query direction is receiver-to-light,
opposite the raster shadow view. Mesh and merged/expanded MultiMesh paths carry
the metadata; material masks and alpha coverage remain in the confirmation path.
Fresh review of this separately authorized correction returned PASS. It checked
material back/front/disabled culling, the double-sided override, owner mirror
compensation and expanded/merged MultiMesh parity against frozen source and
Khronos traversal semantics. Runtime coverage remains outstanding.

Shadow-correction evidence, 2026-09-07: four DI variants passed Vulkan 1.3 GLSL
compilation; the SCons `render_raytracing.windows.editor.x86_64.obj` target passed
in 3.69 seconds. Logs are `%TEMP%/rtxdi-shadow-facing-glsl/compile.log` and
`%TEMP%/rtxdi-shadow-facing-object-build.log`. The full editor attempt was blocked
by preserved incomplete Step 6 `nrd_effect.cpp` fields/types (`spir_v` and label
`String` versus `Span<char>`), recorded in `%TEMP%/rtxdi-shadow-facing-build.log`.
No real-device facing or rendering validation has run.

Step 6 evidence, 2026-09-07 at `a661887676`: the RD adapter owns NRD RELAX
instances, pools and dispatch resources per render buffer. Frame preparation,
denoising and HDR composition are wired before sky/post. Analytic/emissive/sky
inputs use unexposed radiance; composition applies exposure and material factors
once. Retained fog consumes the exposure-independent sky bake with its per-view
energy multiplier. Legacy main-view GI resources and active reflection/VoxelGI
capture scheduling are removed. The provisional NRD compilation errors noted
above are closed by this completed stage.

The final Windows editor/console build passed in 33.44 seconds; log:
`%TEMP%/rtxdi-step6-final-build.log`. Twenty-four DI/frame and sixteen retained
fog shader variants compiled for Vulkan 1.3 under `%TEMP%/rtxdi-step6-final-glsl`.
The exact patch is `%TEMP%/rtxdi-step6.patch`. These are source/compile results,
not real-device dispatch or visual proof. No automated tests ran.

Stage 6 round 1 review, 2026-09-07 at frozen `a661887676`: REJECT for two defects.
`environment/sky.cpp:1089` still exposes directional energy before procedural/
physical sky evaluation, so the nominally raw bake and HDR composition apply
exposure twice. Correct the sky-light input authority, retaining one exposure at
each visible-sky/fog/HDR output. `render_forward_clustered.cpp:2118` also invokes
PRE_OPAQUE before surface/depth (`:2138`) and normal/roughness preparation
(`effects/nrd_effect.cpp:206`), contrary to the retained compositor input contract.
Prepare current surfaces/guides before that callback and keep lighting/HDR after.
Both corrections landed in `217396c25c`: SkyRD directional inputs are raw, with
sky-fog exposure retained at its own coefficient, and guide preparation is split
from NRD processing so current surface/depth/guides precede PRE_OPAQUE without
later overwriting callback guide changes. NRD packing, descriptors, lifecycle and
old-path removal passed the other first-round source review attacks. Fresh final
round 2 returned PASS against the corrective commit and cumulative
`5a532d08b3..217396c25c`, confirming both corrections and the remaining Stage 6
source/compile contract. This does not establish runtime correctness.

Correction evidence: Windows editor/console build passed in 31.45 seconds;
template_debug/console passed in 24.86 seconds. Logs:
`%TEMP%/rtxdi-step6-correction-editor.log` and
`%TEMP%/rtxdi-step6-correction-template.log`; exact patch:
`%TEMP%/rtxdi-step6-correction.patch`. The correction changes no shader source or
layout. Binaries were built from the corrected working source before its commit
and report prior documentation HEAD `4.8.dev.custom_build.f8675d8ed`; this metadata
is not the source target SHA. No GPU execution or automated tests ran.

Initial Step 7 template evidence: the Windows Vulkan `template_debug` build with
`accesskit=no d3d12=no -j16` passed in 302.18 seconds. Binary reports
`4.8.dev.custom_build.a66188767`; SHA-256
`8D8D6A3846C0F144B27671018F548EFE853CABF967DC166F664F482F668BCC48`.
Log: `%TEMP%/rtxdi-step7-template-build.log`. This predates the pending Stage 6
corrections and does not prove rendering. Local `nvidia-smi` on 2026-09-07 reports
RTX 4090, driver 616.64 and 24564 MiB; this initial build/device evidence preceded
the runtime investigation below.

Stage 7 diagnostic history, 2026-09-07, source baseline `217396c25c`, now closed
by `c8ba1736e1` except independent review: actual Vulkan 1.4.351 startup on RTX 4090
exposed initialization and dispatch defects not covered by source review.
`RenderingServerDefault::_init()` created the compositor before camera attributes,
which renderer initialization dereferenced; `/MAP` symbols located that crash.
Corrected allocation order gets through startup. NRDEffect's constructor also
queried the directional-light limit before LightStorage initialization; the
correction uses the same canonical renderer constant as storage initialization.
These corrections are committed; independent final review remains pending.

Pinned NRD SPIR-V is valid before re-spirv and invalid afterward. Immutable
diagnostic blobs live under `%TEMP%/rtxdi-step7-optimizer-failure/`: input
`681D0771CDAE5F6DC4F38E7DF59C22CA9EAC6A7DB5E707AA87845EB077680F5C`, pre-optimizer
`318C1D13E8432A1E297BB0FD23DDE832D8E26A30A55F341DF12637F782EB0B3C`, post-optimizer
`E0836C09CE40DF0A3E3D13620A382F46E2FFE334CB2C8EBF9BD4E60B5DA37426` (SHA-256).
The lost IDs 307/310/312/314/316/327 are loop Phi/increment definitions still
referenced by surviving Phi instructions. Source diagnosis: re-spirv's
`Shader::parseData()` omits loop-continue operands from its acyclic dependency
graph, but `Shader::sort()` derives liveness out-degrees solely from that graph;
optimizer dead-code removal deletes the omitted live definitions. The working
recorded dependency patch preserves the omitted liveness counts without adding
sort cycles. Corrected live Vulkan output at `%TEMP%/rtxdi-nrd-fixed-post-optimizer.spv`
passes `spirv-val --target-env vulkan1.3` (independently checked by the parent) and
matches the valid pre-optimizer SHA-256 above. No NRD-specific bypass is used.

Ordinary Vulkan now creates all 15 NRD pipelines and dispatches DI. The working
bindless correction finalizes the existing texture owner against the actual DI
compute layout rather than the old raygen layout. Isolated directional and emission
cases now render correctly: directionals were missing from the RT light list in
`renderer_scene_cull.cpp`; the working fix appends them alongside the raster list.
Directional raw/NRD diffuse values are approximately 0.931/0.929, with final gray
127/255. An emission-only authored sRGB (0.125, 0.25, 0.5) produces linear HDR
(0.01434, 0.05087, 0.214), zero DI and final (32, 64, 127), demonstrating one emission
addition in this case. All 15 captured optimized NRD modules validate for Vulkan 1.3.

The mixed-light gallery still fails acceptance. Targeted pinned SDK source reads
identify an initial-reservoir combination defect: `RTXDI_SampleLightsForSurface`
returns finalized inverse-PDF weight, but the custom environment combination uses
it directly as an unfinalized running weight sum. `Reservoir.hlsli` requires a fresh
empty accumulator populated through `RTXDI_CombineDIReservoirs` before finalization.
The working correction restores bounded mixed energy: the same wall HDR sample
changes from approximately (7.28, 2.54, 10.33) to (0.1255, 0.0724, 0.07965).
Remaining sphere bands disappear with the diagnostic viewport LOD threshold zero:
raster-selected simplified triangles differ from the full CLAS triangles. The
working RTXDI surface-pass correction selects the matching base geometry. The
ordinary demo threshold has been restored; a new gallery capture has smooth
spheres, visible patterned emission and coherent shadows (parent visually checked).

Native stack capture and exact executable disassembly attribute shutdown's invalid
RID to the deformed BLAS free in `RenderRaytracing::cleanup_caches()`. RD registers
that BLAS as dependent on MeshStorage's index buffer; freeing the source buffer
recursively destroys the BLAS before the retained cache releases it. Existing mesh
change/deletion notifications occur after source buffers are freed. The cache
lifetime correction uses an unbound RD acceleration-structure validity query at
deformed-cache release and rebuild decisions. The subsequent build passed in
52.34 seconds. A separate-render-thread run with motion, two viewports, fog and
FSR2 exited zero without ERROR/invalid-RID messages in that capture. A later run
exposed a separate render-thread teardown defect: after joining the rendering
worker, `RenderingServerDefault::finish()` resets its thread ID but does not call
the existing `RenderingDevice::make_current()` on the main thread. DisplayServer
later deletes RD on main, failing its `finalize()` thread guard. A working handback
correction and another ordinary shutdown check are required. Primary internal resolution
was 858x483 for 1280x720 output; the independent child was 384x216. Readback
instrumentation was still present. The upscaled capture has visible staircase
edges. Subsequent static motion readback explains a temporal integration defect:
floor samples carry tens of pixels of motion and nonzero depth change while still.
The retained `GeometryInstanceForwardClustered::age_out_motion()` lost its caller
with removal of the old PT prepass, leaving initial previous transforms stale.
The working correction restores its existing per-frame call before current
raster/RT instance uploads. A 29.85-second build and actual stationary readback
show floor motion below 0.00004 pixels and depth delta exactly zero. The resulting
FSR2 capture has smooth edges and no earlier sphere crosshatch (parent inspected).
Separate-thread shutdown exits zero without errors after the RD handback fix.
Separate source tracing found per-viewport previous camera
copy/update and mono UBO population intact. FSR2 also needs zero opaque reactivity:
old raster output applied `pass_alpha_multiplier = 0`, whereas new HDR alpha is 1
and the existing reactive view reads that alpha. A default black reactive binding
preserves the current opaque contract without changing HDR alpha. Final
diagnostics-free editor build passed in 31.73 seconds; movement, cut, resize,
second-view creation/destruction, fog/FSR2, light reorder/removal, off-screen
caster toggle and deformer/source deletion were exercised without shutdown errors.
Final template execution subsequently passed; review remains pending. The unsupported-material scene
exposed an anisotropy classification collision: `ShaderData::set_code()` maps
`ANISOTROPY` to `uses_anisotropy`, then overwrites it with `uses_tangent`. The
committed correction removes the collision and maps `ANISOTROPY_FLOW` through the
same anisotropy flag; the existing tangent propagation remains intact. The actual
unsupported-material wrapper now marks all three unsupported cases diagnostically.
Earlier numeric observations used temporary surface/DI/NRD/HDR readback instrumentation.
Surface base/shading inputs look coherent;
signed NRD roughness values near the normal Z hemisphere boundary decode correctly
and are not evidence of a packing defect. Final ordinary captures run without
temporary dump/readback instrumentation or an optimizer bypass. Actual scoped
rendering and pass timings are recorded in the final Stage 7 evidence linked above.
The manual project is owned at `demos/rtxdi_manual`; owner demo assets were excluded
from that commit.

Owner follow-on, 2026-09-07: after Stage 7 closes, migrate `gi_demo/test.tscn` into
the new demonstration project, adapt it to actual RTXDI/NRD capabilities, and
document editor launch controls. This explicitly authorizes that bounded use of
the previously excluded old demo. Read-only inventory: the scene uses self-contained
`zdm2.glb` and `cube.glb`, old GI/reflection/SSIL switch scripts, baked GI resources,
PT environment properties and primitive camera-attached meshes. Preserve the map,
light/camera placement and source attribution; replace retired controls and use
imported CLAS-capable geometry. No migration edits have been made yet. GLB JSON
inspection found no external buffer/image URIs in either file. Preserve the
`gi_demo/README.md` zdm2 attribution (Cube 2: Sauerbraten, CC BY 4.0 and its linked
credit source). Replace old baked-GI/probe/SSIL/PT controls rather than presenting
them as functional. Reuse the current demo's F5/F6 and camera-control conventions.
Delegate the migration as a separate bounded task after the Stage 7 review gate;
verify the imported scene with the actual editor/Vulkan and review its own commit.

Evidence date: 2026-09-06 UTC. Frozen Step 4 final review target:
`5075e8b3fc3b19213744d0e9ac9f3648ca710359`.

The last confirmed defect was the missing two camera RID parameters in
`drivers/gles3/rasterizer_scene_gles3.h:943`, its definition at `.cpp:2388`, and
`servers/rendering/dummy/rasterizer_scene_dummy.h:163` to match
`servers/rendering/renderer_scene_render.h:325`. Non-RT gameplay remains excluded;
these implementations still have to satisfy the shared C++ interface. The owner
subsequently authorized correction `826dc09a5a`; its three-line code change and
MSVC checks close the signature mismatch, confirmed by fresh independent review.

Attribution correction for `826dc09a5a`: the actual executing role/model was
`core-implementer (gpt-5.6-sol)`, selected under the adapter read at dispatch time.
Its trailer incorrectly says `Codex (GPT-6)`. This entry corrects the execution
record without rewriting history. Concurrent policy commit `8078509d21` was
preserved and is not part of the signature fix.

Step 5 implementation is committed in new `forward_clustered/render_rtxdi.{h,cpp}`,
new `shaders/raytracing/rtxdi_di.glsl`, `rtxdi_application_bridge_inc.glsl` and
`rtxdi_light_data_inc.glsl`, with narrow DI lifetime changes in
`render_raytracing.{h,cpp}` and extraction from `rtxdi_light_sampling_inc.glsl`.
The host resource/context/config/uniform-set/dispatch code is compiled and wired
after `commit_rtxdi_surface()`. Four separate RD compute lists use SDK reservoir
indices/pitches, with explicit compute BDA dependencies and per-viewport lifetime.

All four corrected native GLSL DI variants (initial, temporal, spatial, shade)
passed glslangValidator for Vulkan 1.3 using the pinned SDK. The Windows editor
and console linked with `scons platform=windows target=editor accesskit=no
d3d12=no -j16`; the final build took 42.43 seconds. The local evidence log is
`C:/Users/lukas/AppData/Local/Temp/rtxdi-step5-build-final.log`, and the four
flattened shaders/SPIR-V files are under the adjacent `rtxdi-step5-glsl/` folder.
No template build or GPU execution was performed. Additional imported GLSL
boolean fixes are recorded in
`thirdparty/rtxdi/patches/0002-glsl-di-boolean-parameters.patch` and applied to
InitialSampling, TemporalResampling and SpatialResampling; the importer applies
the recorded patch directory. No generated shader was edited. The diagnostic
`comp.spv` was removed. These temporary diagnostics are not durable runtime proof.

Initial candidate counts are 8 local, 1 infinite, 1 environment and 0 BRDF.
BRDF ray proposals are disabled because per-light caster masks do not share the
visibility domain required by that proposal's MIS support assumption. Temporal
correction uses current AS and can exhibit transient bias. Outputs are separate
RGBA16F diffuse/specular radiance and linear hit distance, demodulated with pinned
NRD material factors and sanitized/clamped for FP16. The next stages are NRD/HDR
composition and final editor/template/real-Vulkan visual validation. NRD/HDR
implementation landed in `a661887676` after the separately authorized shadow fix
passed review. Its corrective commit `217396c25c` passed final round 2; final
real-device validation is recorded in Stage 7 `c8ba1736e1`; its independent review
is pending. No automated tests have been run.

Intermediate builds/rendering may fail by explicit owner authorization. Final
completion requires the plan's real-device rendering gate. Automated tests are
not authorized. Preserve the owner's untracked demo/game content.

## Available validation environment

Read-only device/tool discovery on 2026-09-06 via local `vulkaninfoSDK.exe` and
PowerShell: RTX 4090, NVIDIA driver 616.64, device Vulkan 1.4.351, ray query and
acceleration structure features true, maxColorAttachments 8. These feature values
do not prove shader-format compatibility or correct renderer execution.

SCons, Python 3.14, VS18 BuildTools CMake/Ninja and Vulkan SDK 1.4.357.0
glslangValidator are available. No Godot process was running at inspection.
Step 1 rebuilt the normal editor binaries, but they do not yet contain the later
RTXDI render passes. Other existing binaries remain pre-task artifacts.

## Boundaries

The approved plan's opaque standard-PBR milestone excludes DDGI, indirect
reflections, glass/thin-leaf transmission and the future geometry DAG/voxels.
Fog still requires its existing shadow maps. No backward compatibility or
non-RT gameplay fallback is to be added.
