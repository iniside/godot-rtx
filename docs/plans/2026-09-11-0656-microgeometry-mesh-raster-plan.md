# Microgeometry mesh-shader raster replacement

Owner authorizes starting the proposed combined replacement: amortize shared
validation per cluster and process unique meshlet vertices through mesh shaders.
Task start `3d36adda2495`, renderer baseline `19f97459f7`. Preserve10000-instance
native stress scene, saved camera,1px, materials, shadows, HZB recovery, streaming,
instancing, debug views and ordinary/deformed mesh behavior. No import/asset
format rebuild, global quality change, foliage/voxels, XR/VR/multiview/split
screen, tests, proof auditors or unrelated scheduling work. Primitive culling
is a separate subsequent optimization; this replacement preserves triangles.

## Evidence and overlap

Selection emits one20-byte indexed command per surviving cluster
(`micro_geometry_select.slang:711`). `_render_micro_geometry` binds an identity
index array and issues indirect-count drawing per bin. The ordinary vertex
entry (`scene_forward_clustered.slang:842`) calls corner-indexed pulling; line82
repeats metadata checks and attribute-layout validation. Existing fragment,
material code injection, VertexOutput and pipeline variants are the authorities
to reuse. No parallel microgeometry indexed fallback remains after replacement.
CLAS/RT selection and asset residency remain separate existing consumers.

No mesh stage/dispatch infrastructure exists in RD/Vulkan. ShaderRD already
owns stage templates and variant defines; extend the vertex-template stage
selection rather than duplicate the renderer/material shader. Vulkan mesh
features and device limits must be queried, not inferred from the4090 fixture.
Unsupported backends retain ordinary upstream mesh rendering and explicitly
report unsupported microgeometry mesh raster. No new user setting is needed.

Source navigation: root clangd symbols followed by current implementations,
shader-native reads and bounded inventories; backend research covers stage,
reflection, cache, descriptor and draw-graph contracts. Primary references:
[Vulkan mesh shading](https://www.khronos.org/blog/mesh-shading-for-vulkan),
[Slang mesh outputs](https://docs.shader-slang.org/en/latest/external/core-module-reference/global-decls/setmeshoutputcounts-037d.html).

## Step1 — mesh-stage and indirect dispatch infrastructure [independent]

Own `servers/rendering/rendering_device_commons.{h,cpp}`,
`rendering_device_driver.h`, `rendering_device.{h,cpp}`,
`rendering_device_graph.{h,cpp}`, `rendering_shader_container.{h,cpp}`,
`renderer_rd/shader_rd.{h,cpp}`, Vulkan `rendering_device_driver_vulkan.{h,cpp}`,
`modules/slang/shader_compile.cpp`, `modules/glslang/register_types.cpp`,
D3D12/Metal `rendering_shader_container_*.cpp`, and
`doc/classes/RenderingDevice.xml`.

Append MESH shader stage and bit without renumbering shipped enums; close stage
maps, stage names, reflection, shader-container versions/cache keys, binding/XML
and backend rejection. Vulkan enables EXT meshShader and queries corresponding
limits; graphics pipelines with mesh stage omit vertex input/input assembly.
Add mesh-stage synchronization and descriptor-stage visibility. Preserve existing
vertex/compute/RT contracts, and do not use unsupported parser optimization on
mesh SPIR-V. Slang stage is MESH; entry remains main.

Internal contracts: RD/RDD `mesh_shader_is_supported()` and
`mesh_shader_get_limits()` expose a MeshShaderLimits value with workgroup count,
total, size/invocations, output vertices/primitives/components/memory and shared
memory/granularity limits. Unsupported drivers return false/zero. RD
`draw_list_draw_mesh_tasks_indirect(DrawListID,RID,uint32_t)` feeds graph and
driver `command_render_draw_mesh_tasks_indirect(CommandBufferID,BufferID,uint64_t)`.
One dispatch consumes12 bytes; no unused direct/count/script draw APIs.
Follow existing descriptor binding, buffer bounds/usage, transfer validation,
indirect-read tracking, command ordering and resource lifetime.

`ShaderRD::VariantDefine` gains optional fourth constructor argument
`RD::ShaderStage rasterization_stage=SHADER_STAGE_VERTEX`; MESH variants compile
the existing vertex template into MESH. Include stage in cache identity/native
source reporting. Existing callers retain VERTEX. This infrastructure lands
before Step2 uses it; mesh variants stay disabled on unsupported devices.

## Step2 — replace microgeometry indexed raster [independent]

Own forward-clustered `render_forward_clustered.{h,cpp}`,
`scene_shader_forward_clustered.{h,cpp}`, `micro_geometry_selection.{h,cpp}`,
their `scene_forward_clustered.slang`, `scene_forward_clustered_inc.slang`,
`micro_geometry_inc.slang`, `micro_geometry_select.slang`, and exact storage
header declaring shared raster parameter structs if a layout change needs it.

Reuse existing vertex computation/material injection/fragment outputs. Replace
microgeometry corner pulling with shared cluster/context validation once per
workgroup and unique local-vertex evaluation once per vertex. Preserve source
VERTEX_ID, INSTANCE_ID, current/previous transforms and attributes. Invalid
metadata yields a uniformly empty workgroup; protect per-vertex/per-index accesses
and retain generation/material/asset checks. Support existing128-vertex and
128-triangle imported limits with device/output-memory validation. Emit existing
triangles and winding unchanged; ordinary vertex entry remains for nonmicrogeometry.

Create GPU mesh-dispatch arguments per bin after selection and recovery; flatten
2D group IDs to selected cluster indices, with padding guards and checked device
workgroup limits including padded total. Retain selected-buffer offset/count
semantics for initial, recovery and frozen combined draws. Bind counts/context
through existing raster uniform owner and narrowly updated push/parameter ABI.
Replace per-cluster indexed command generation, identity index buffer allocation,
binding and drawing, and all associated allocation/accounting/retirement at the
same authority. Preserve selector RT consumers; do not force RT to consume mesh
dispatch semantics. No second selectable raster implementation or new asset cache.

Select MESH only for micro_geometry material variants; preserve fragment
reflection, specialization, depth/color attachments, cull/wireframe state and
debug substitutions. Gate unsupported hardware clearly before consuming the new
path. Existing shader SCsub glob owns the changed sources; change no generated files.

## Step3 — ordinary build, native scene measurement, handoff [inline]

Freeze source and build ordinary Windows Vulkan editor. Use preserved baseline
`bin/godot.mesh_shader_before.exe`, SHA256
`E253D22FB915A3052854ECCF42AC22E782B89FAC88D2647BE7686CE5CB5A1245`.
Run actual native stress editor, one viewport, same saved camera/1px/resolution,
visible axes. Compare selected clusters/triangles, raster GPU and whole-frame GPU,
and unprofiled FPS. Record actual limits/mesh pipeline execution and meaningful
counts without per-vertex global atomic instrumentation. Check streaming/CLAS
settles and no shader/device errors. Preserve owner visual evaluation; explicitly
state movement/reveal/freeze coverage actually performed. Restore temporary
editor measurement settings and retain scene hash. No automated tests/audits.

Commit completed source steps and get one brief final source review, as requested
by owner. Update project-state and measured status. No9ms claim without actual
result. Compiler optimization-level policy is an open bounded investigation;
do not silently change all renderer shaders while introducing the mesh stage.
