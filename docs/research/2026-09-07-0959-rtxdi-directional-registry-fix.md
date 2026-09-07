# RTXDI directional registry cap fix

Named fix for the Stage 7 round 1 review: the complete RTXDI registry inherited
the eight-directional-light fog/shadow bound. Authority: the
[approved plan's final contract and Step 3](../plans/2026-09-06-1854-rtxdi-renderer-plan.md).
Original reviewed target: `c8ba1736e1a67fad02bb20d85b6328fab0c3d51b`;
original Stage 7 baseline: `217396c25cc8a54804007c99e7445d35ac00da49`.
Actual fix start: `b01c5849a8d10397be3e977347b0a49f45c90db5`.
Executing agent: core-implementer, gpt-6-astra, high; selected for the
scene-culling-to-RT-registry contract. Comments: default NONE.

## Authority and minimal closure

`RendererSceneCull::_render_scene`, in
[renderer_scene_cull.cpp](../../servers/rendering/renderer_scene_cull.cpp), now
appends RT directional RIDs directly from `scenario->directional_lights` after
ordinary culling has completed. It preserves the existing explicit visibility,
instance-layer/camera-layer overlap and nonnull light-data checks. The prior RT
append from the bounded `directional_lights` vector is removed in the same fix.
The original fog/shadow population and its eight-light cap remain unchanged.

The former population stops at `MAX_DIRECTIONAL_LIGHTS` near line 3354; the
replacement is near line 3675. `RenderRaytracing::build_light_registry` near
line 3113 consumes the entire provided RT array and still rejects sky-only
directionals. No public API, ClassDB binding, XML, shader layout, resource owner,
thread dispatch or SCons wiring changes. The RT result container retains its
existing frame clear near line 3403 and existing rendering-server ownership.

Navigation: clangd `find`, `refs`, `hover` and `def` used the root compile database
for `_render_scene`, `_scene_cull`, `build_light_registry`, `MAX_DIRECTIONAL_LIGHTS`
and the scenario list. Targeted source/header reads confirmed control flow and
the empty-AABB bypass near line 1678. `git blame` identified the old cap as
upstream and the capped RT append as fork-local commit `c8ba1736e1`. Bounded `rg`
inventories of RT append/clear sites and light bindings were textual lower bounds.
`Light3D` and `DirectionalLight3D` bindings and their class XML were read for the
manual scene's energy, shadow, mask and sky-mode properties; none were changed.

## Build and real-device energy evidence

`scons platform=windows target=editor accesskit=no d3d12=no -j16` compiled and linked
the ordinary editor successfully in 29.88 seconds. `git diff --check` passed for
the source change. No formatter or automated tests were run or authored.

The rebuilt editor SHA-256 is
`55967155821B21DFF843D2FEFA663B84160405167714EAE7C98764D43EA795A2`.
Its embedded revision is the fix-start revision above; the build includes this
uncommitted source change. Built `renderer_scene_cull.cpp` SHA-256:
`E4572E41922BA0F50CBCE731320478E5979F50927F36B00D7DBBC449051431E6`.

Manual captures used a separate project at
`%TEMP%/rtxdi-directional-registry-fix`, with no edits to the concurrently migrated
demo. The scene has one imported plane from the owned demo geometry, albedo
`Color(0.5, 0.5, 0.5)`, zero material/light specular, a top-down camera, black
background without sky, linear tonemapping, and equal downward white directionals
in light-only sky mode. No shadow maps or fog are enabled. Each capture waits ten
seconds and saves the actual viewport image without assertions or pass/fail logic.
The authored directional count is logged; there is no GPU registry readback hook.

All four actual rendering runs used Vulkan 1.4.351, Forward+, Streamline and
NVIDIA GeForce RTX 4090, at 640 x 360 internal/output resolution with no MSAA or
upscaling, and exited zero. The corrected energy scaling demonstrates that lights
beyond the eighth contribute through the production RT path.

| Executable | Directionals | Energy each | Center RGB, 8-bit PNG | Frame | Capture |
| --- | ---: | ---: | --- | ---: | --- |
| Rebuilt editor | 1 | 1 | 128, 128, 128 | 1168 | [Unit reference](rtxdi-directional-registry-fix-evidence/fixed-1.png) |
| Rebuilt editor | 9 | 0.0625 | 97, 97, 97 | 1174 | [Nine lights](rtxdi-directional-registry-fix-evidence/fixed-9.png) |
| Rebuilt editor | 16 | 0.0625 | 128, 128, 128 | 1198 | [Sixteen lights](rtxdi-directional-registry-fix-evidence/fixed-16.png) |
| Prior Stage 7 template | 16 | 0.0625 | 92, 92, 92 | 1187 | [Capped comparison](rtxdi-directional-registry-fix-evidence/baseline-16.png) |

The prior template hash is
`35A5595E77FEFDB176DD494A32D298C847A89CF87E4E20C62B6C658B0D8B59AF`, matching the
[Stage 7 manifest](rtxdi-stage7-evidence/manifest.json). Its embedded revision is
`eeed72b42987b2d29f9e1a99e2dd57cc5ff91172`, with Stage 7 source provenance recorded
in that manifest. It is an older template comparison, not a same-target rebuilt
pre-fix editor. Half the intended linear illumination produces approximately
92/255 sRGB; the fixed sixteen-light image matches the one-light unit reference.

[Capture log excerpts and complete capture stderr](rtxdi-directional-registry-fix-evidence/captures.txt)
retain the actual device, parameters, pixels and frame counts. The local project
can be rerun with the ordinary editor using
`--path <temporary-project> --rendering-method forward_plus --rendering-driver vulkan -- --count=16 --energy=0.0625 --capture=<output.png>`.
Its `main.gd` SHA-256 is
`528E1C7B7E73A85173461AAD0D7A6155EBC0E3EB3AE317C207B52169479D6467`;
the temporary project, import cache and full build logs remain local.

## Boundaries and siblings

Applicable failure classes: 2, 3, 4, 5, 6, 7, 8 and 9. The expected topology is
all eligible scenario directionals -> RT RID array -> complete infinite-light
registry -> existing ReSTIR DI and NRD composition, independently of the bounded
fog/shadow sibling. The fix does not allocate or free RIDs, keep additional
pointers across frames, change bindings, or introduce another lighting authority.
Ordinary local-light collection and registry sky-only filtering were source-read
and remain unchanged. No directional cap is raised in fog/shadow resources.

The fixed template, separate rendering-thread mode, visibility/mask permutations,
fog with more than eight lights, and other backends were not rerun for this
bounded fix. D3D12/Metal remain outside the approved Vulkan milestone. The
capture logs retain the existing unsupported-inline-shader warning and
`OpTypeForwardPointer` parser messages; this is not validation-layer-clean proof.
Initial headless asset import completed its import work but exited with
`EditorNode::is_cmdline_mode` null-singleton error (`-1073741819`) during editor
teardown. Subsequent real Vulkan project runs loaded the imported plane and
exited cleanly. That import teardown issue is recorded, not changed here.

Fresh final round 2 review of the fix and original Stage 7 cumulative diff, plus
canonical project-state/status maintenance, are owned by the coordinating agent.
