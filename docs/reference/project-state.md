# Project State

Read this compact index before project research. Follow the linked source or
canonical decision document when detail matters; do not treat this page as an
implementation plan or runtime proof.

## Owner Direction

The target renderer is hybrid rasterization with ReSTIR DI and DDGI. It keeps
an automatically simplified cluster DAG, excludes manual object LODs, uses
real near-field foliage geometry without alpha-cutout silhouettes, and moves
distant foliage to a voxel representation. The decisions, reasons, and open
research questions live only in [rendering-direction.md](rendering-direction.md).

Evidence: owner decision recorded 2026-09-06. This describes direction, not
landed implementation.

## Source State

Verified 2026-09-06 at source baseline `ca35348c11660535467348349d7a257a44c61e9a`
by targeted reads of this checkout:

- Static CLAS population enters
  `RenderRaytracing::_populate_cluster_blas()` in
  [render_raytracing.cpp](../../servers/rendering/renderer_rd/forward_clustered/render_raytracing.cpp).
  The Vulkan backend records
  `vkCmdBuildClusterAccelerationStructureIndirectNV` in
  [rendering_device_driver_vulkan.cpp](../../drivers/vulkan/rendering_device_driver_vulkan.cpp).
- The path-tracing renderer in
  [render_forward_clustered_pt.cpp](../../servers/rendering/renderer_rd/forward_clustered/render_forward_clustered_pt.cpp)
  replaces the opaque pass and renders at internal pre-upscale resolution.
- The ray-generation shader has a `USE_SER` path using
  `hitObjectTraceRayEXT`, `reorderThreadEXT`, and
  `hitObjectExecuteShaderEXT` in
  [scene_raytracing_raygen.glsl](../../servers/rendering/renderer_rd/shaders/raytracing/scene_raytracing_raygen.glsl).
  Ordinary `vkCmdTraceRaysKHR` submission does not establish that SER is absent.
- DLSS Ray Reconstruction guide production and consumption are anchored in
  [render_raytracing.cpp](../../servers/rendering/renderer_rd/forward_clustered/render_raytracing.cpp)
  and [dlss.cpp](../../servers/rendering/renderer_rd/effects/dlss.cpp).
- D3D12 ray-tracing and CLAS methods remain unsupported stubs in
  [rendering_device_driver_d3d12.cpp](../../drivers/d3d12/rendering_device_driver_d3d12.cpp).

Method limits: repository-root `compile_commands.json` was absent, so clangd
navigation was unavailable. A targeted renderer text inventory found no
RTXGI, DDGI, ReSTIR, or NRC implementation in the inspected paths; this is a
bounded lower bound, not proof of global absence. Git history identified the
listed renderer work as fork-local.

## Known Gaps

No runtime performance was verified. The active build, enabled features, and
hardware behavior are unknown. The older
[Mega Geometry research](../research/2026-09-02-1904-megageometry-camera-relative-research.md)
predates landed CLAS work and is historical evidence only.
