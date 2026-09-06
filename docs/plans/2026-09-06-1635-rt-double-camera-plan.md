# RT double precision camera contract

Status: owner approved implementation on 2026-09-06 after fresh plan review PASS.
Task baseline: `e45761a85904cd9b74734510fcdc381c60835790`.

## Context

Fix motion flicker and ghosting in `gi_demo/test.tscn`; performance remains
deferred. The owner confirms single precision has no flicker. Captured double
frames show brightness impulses every eight frames, absent in matched single
captures. The previous DLSS matrix fix does not repair the RT SceneData contract.

The RT setup omits USE_DOUBLE_PRECISION while the CPU UBO contains an additional
16-byte inv_view_precision field. The shader consequently reads taa_frame_count
as IBL_exposure_normalization. Double CPU camera upload also stores negative
camera translation as high/low parts, which RT currently treats as positive
world-space camera translation.

## Step 1 [independent]

One core-implementer, gpt-5.6-sol, high effort owns this complete replacement:

- `servers/rendering/renderer_rd/forward_clustered/render_forward_clustered_pt.cpp`:
  `_setup_rt` supplies USE_DOUBLE_PRECISION under REAL_T_IS_DOUBLE.
- `servers/rendering/renderer_rd/shaders/raytracing/raytracing_common_inc.glsl`:
  provide an internal inverse-view decode using the existing SceneData contract.
  Double translation is minus the sum of its stored high and low components;
  single translation and the rotation basis retain their current meaning.
- `servers/rendering/renderer_rd/shaders/raytracing/scene_raytracing_raygen.glsl`
  and `raytracing_custom_fragment_inc.glsl` in the same directory: replace all
  three translation-bearing inverse-view reconstructions with this decoder,
  covering primary rays and custom material/intersection builtins. Basis-only
  consumers and the existing world-to-view matrix remain valid.

The general define flows through SceneShaderRaytracing::init and ShaderRD into
all default/custom raygen, miss, closest-hit, any-hit, and intersection sources.
Existing raytracing SCsub includes own the changed shader inputs. The upstream
SceneData layout remains the sole authority; no new buffer, setting, public API,
ClassDB/XML contract, serialized data, scene change, or compatibility path.
No Object/RID/GPU lifetime or threading changes. This fixes the existing float
world-space RT pipeline in a double engine, not a new large-world pipeline.

## Validation and review

Build Windows editor precision=double and precision=single with d3d12=no.
Run the existing temporary motion recorder against the exact scene on RTX 4090
Vulkan: same original poses/settings, two speeds, 40 moving frames, 40 stopped
frames and settled frame 120. Compare pre-fix double, post-fix double and single;
inspect images and temporal metrics for the eight-frame impulses and after-stop
ghosting. Compilation alone does not prove visual correctness. Exercise runtime
shader compilation; disclose custom stages not visually exercised by this scene.
No new automated tests or fixtures. Preserve owner content and generated caches.

Commit the scoped implementation, freeze its SHA, then obtain fresh hostile
review of that commit and the baseline-to-target cumulative diff. The reviewer
routes through failure classes 2-9 as applicable. Navigation evidence is direct
4.8-dev source/include reads (root compilation database absent), bounded text
inventories as lower bounds, and git history establishing fork-local ownership.
