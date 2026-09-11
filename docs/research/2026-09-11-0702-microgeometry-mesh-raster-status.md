# Microgeometry mesh raster status

Implemented the owner-authorized unique-vertex meshlet raster and shared
validation. Task start `3d36adda2495`;
[plan](../plans/2026-09-11-0656-microgeometry-mesh-raster-plan.md) at `538d773a3a`.
Backend `e5f74b83ee`, renderer `778721ab5e`, descriptor correction `2bd770c26d`
and simplified-identity correction `6e916a19c8` form the final source.

## Final result — 2026-09-11

Ordinary Windows Vulkan editor builds successfully. Final captures
`mesh-simplified-profile` and `mesh-simplified-noprofile` both exit 0 without
ERROR lines or timeout. Same 10000-instance native scene, saved camera,
visible axes, 1 px tolerance and actual 2689x1602 guide/HZB resolution.
Last-ten sample medians:

| Metric | Before | Final |
| --- | ---: | ---: |
| Whole GPU | 21.1695 ms | 19.1815 ms |
| Camera raster | 11.4683 ms | 11.2305 ms |
| Shadow raster | 3.4178 ms | 1.0920 ms |
| Unprofiled editor FPS | 46 | 54 |

The main measured gain is in shadow raster; camera raster remains expensive.
These paired runs do not establish a 9 ms frame budget or a broad benchmark.
Camera selection and every reported HZB query counter match the baseline at
frame 840: 785617 clusters / 67357876 triangles, 140481 initial HZB rejections,
and zero recovery emission. The new selection counter sums 49830642 unique
cluster vertices versus 202073628 triangle corners. This is selected geometry
work, not a hardware invocation-counter measurement. Camera dispatch uses two
material bins, with two additional recovery dispatches whose counts are zero.
No primitive culling, quality reduction or asset reimport is included.

Streaming settles at 3724 resident pages, 0 pending pages and 61333 resident
CLAS; 10000 instances still share 62 cuts/BLAS from two assets. The scene hash
remains `F276E9A2B9A18B8DE4AE672BF339D7D7925D589E29131EA99A1F61B939A8B19A`.
Both measurement editors exit before restoring the four original editor
sleep/vsync/continuous-update settings. No automated tests were run. Motion,
reveal, resize, freeze and final visual quality were not independently evaluated.

Final measured executable SHA256:
`B2B98BCAA77B72A0DBE98100F10CA5B09E097D5D6D901EB76E1BC19558A6D13B`.
It is built from `2bd770c26d` plus the frozen one-line shader correction later
committed as `6e916a19c8`; no rendering-source drift remains.

## Before measurement

Preserved binary `bin/godot.mesh_shader_before.exe`, SHA256
`E253D22FB915A3052854ECCF42AC22E782B89FAC88D2647BE7686CE5CB5A1245`, renderer
source `19f97459f7`. Same native stress scene10000instances, saved camera,
visible axes,1px, actual2689x1602 guide/HZB resolution. Temporary measurement
settings save originals in `mesh-editor-settings-original.json` beside logs.

Capture `mesh-before-profile`, ordinary Vulkan editor900frames, exits0 without
ERROR lines or timeout. Last-ten medians GPU21.1695ms, camera11.4683ms,
aggregate sparse traversal2.3541ms, emission0.4258ms. Frame840 has785617 emitted
camera clusters /67357876 triangles,140481 HZB rejects, recovery emits0.
Logs/receipts: `C:/Users/lukas/AppData/Local/Temp/godot-render-repair-20260909/`.
Capture `mesh-before-noprofile` exits0 without ERROR or timeout, last-ten
median46FPS. Both measurement editors finish before the implementation build.

## Implementation boundaries

RD/Vulkan has no existing mesh-stage dispatch, so Step1 extends that authority.
ShaderRD retains the common vertex source template with per-variant stage
selection; renderer Step2 reuses material code/outputs/fragment source.
Ordinary meshes remain on their existing path; obsolete corner-indexed
microgeometry drawing is removed. No serialized asset/import change.

Step1 adds the MESH stage without renumbering existing values, internal queried
device limits and indirect dispatch through RD/graph/Vulkan, per-variant raster
stage selection in ShaderRD, descriptor/barrier/reflection/cache closure and
stage source/SPIR-V properties/XML. Unsupported drivers reject mesh stage and
retain ordinary rendering. Source was inspected and scoped diff check passed;
the dependent renderer step builds successfully with
`scons platform=windows target=editor accesskit=no d3d12=no -j16`.

First capture `mesh-after-profile` is stopped by the root after repeated
`RenderingDevice::_draw_list_draw_indirect` set-0 layout mismatch errors. The
owner also observes missing meshes. Any FPS from this failed draw path is
invalid. The actual device reports 128 workgroup invocations, 256 output
vertices/primitives, 32768 output bytes and 28672 shared bytes; feature support
alone does not establish working raster execution.

The layout correction expands vertex/mesh resource visibility symmetrically
in the reflection consumed by both RD format identity and Vulkan descriptor
and push-constant layouts. Actual shader execution stages remain unchanged.
The correction applies after loading cached reflection and retains the
unsupported-backend default. It preserves the existing shared uniform owners;
RD draw validation remains enabled.

Capture `mesh-fixed-profile` exits 0 with no ERROR lines, but the owner still
observes absent meshes. HZB rejects 0 instead of 140481, and selection emits
926098 clusters / 79120798 triangles. Its 18.314 ms GPU result is invalid for
speedup comparison. `micro_geometry_builder.cpp:127-140` deliberately stores
`INVALID_ID` triangle identities in nonleaf clusters; the new raster range
check incorrectly rejects them. The correction removes only that unused
identity check, retaining local triangle-index and source-vertex bounds.

One brief source review passes through `2bd770c26d`; it does not establish
runtime correctness and predates the one-line simplified-identity correction.

Slang O0 is deliberate: `675d3cbe7f` and
`2026-09-07-1126-shader-unification-status.md:101-108` document descriptor
stripping at O2 despite PreserveParameters. No global compiler optimization
change is authorized by this raster replacement. Downstream mesh optimization
support is a bounded implementation risk, not a promised gain.
