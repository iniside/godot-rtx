# id Tech 8 as a rendering reference

Research date: 2026-09-06 UTC. Project baseline: `33b555c7f225afd1e586bf749f2b485c1de260df`.
Scope: primary-source architecture research; no engine edits, build, tests, or local performance measurements. Existing project evidence comes from [project state](../reference/project-state.md) and the [RTXGI investigation](2026-09-06-1748-rtxgi-ddgi-integration-summary.md).

## Verified hybrid GI architecture

Sousa describes world-space light grids, cascaded/local irradiance volumes, and a spatially hashed radiance cache. Visibility hits are stored before deferred material shading; about 20,000 cache entries are shaded per frame and reused. Previous irradiance supplies further bounces. Updates cover one cascade and one local volume per frame, with 16/32/64 rays per probe. Final gather uses one ray per reduced-resolution pixel, consulting screen, radiance, then irradiance caches without shading hits. Filtering and upscaling follow. RT reflections were disabled on consoles; SSR and probes remain. The published hotspot GI costs are about 1.71 ms async on Series S at 900p, 1.4 ms on Series X at 1440p, and 1.7 ms serial on RTX 4080 at 4K. These are pass timings, not whole-frame budgets or portable performance guarantees. [Sousa, SIGGRAPH 2025, PDF pages 12–23, 28, 32](https://advances.realtimerendering.com/s2025/content/SOUSA_SIGGRAPH_2025_Final.pdf).

## The rest of the frame matters

The official GPC abstract describes replacing repeated geometry passes and Forward+ opaque shading with a triangle visibility buffer and compute shading. It also identifies tiled deferred lighting with software VRS. This supports treating material evaluation and shading density as separate optimization targets from triangle traversal. The linked geometry/VRS slide downloads failed through the web tool; this finding is limited to the official abstracts, not a reviewed implementation or detailed GPU comparison. [Lazarek/Hammer and Fuller/Hammer, GPC 2025](https://graphicsprogrammingconference.com/archive/2025/).

The same conference explicitly describes an additional full-path-tracing implementation using SHaRC and OMM, alongside DLSS/NRD. Keep that implementation separate from the baseline GI reference. The reviewed sources do not establish that baseline GI is NVIDIA RTXGI SDK, RTXDI, or that its radiance cache is the same implementation as SHaRC. [Khan/Stack, GPC 2025](https://graphicsprogrammingconference.com/archive/2025/).

Billy Khan identifies SER and OMM as useful PT optimizations; OMM targets alpha-tested workloads including vegetation. This is evidence for accelerating their representation, not evidence that our geometry-only near foliage can be replaced by their approach. [NVIDIA interview, 2025-09-30](https://developer.nvidia.com/blog/?p=106647).

## Implications for this fork — recommendations, not owner decisions

The existing raster + ReSTIR DI + DDGI direction remains appropriate to investigate. Use NVIDIA's source as the implementation reference for probe storage/update/query, while treating id Tech as a reference for system-level budgeting and reuse. A direct SDK integration should not be assumed to reproduce id's entire architecture or performance.

1. Keep the first DDGI integration bounded as previously researched: one fixed volume, existing direct lighting, debug visibility, and correct raster composition. Do not make it depend on RTXDI or the completed geometry system.
2. Define independent budgets for tracing, material evaluation, probe refresh, filtering, memory, and acceleration-structure maintenance. Measure them separately once implementation and measurements are authorized.
3. Evaluate shared hit records and deferred material evaluation when probe-hit shading becomes a demonstrated cost. A reusable surface-radiance cache is a substantial additional feature with its own invalidation contract.
4. Consider a low-resolution final gather only if direct probe lookup gives insufficient local detail. It introduces additional rays, filtering, history, and disocclusion handling; it is not free DDGI quality.
5. ReSTIR DI remains a separate direct-light estimator. If its results later feed cached indirect lighting, define exactly which lighting terms enter the cache and avoid counting indirect illumination twice.

For dense foliage, the unanswered costs remain animation/BVH updates, traversal through layered geometry, cache invalidation, and the transition to distant voxels. This research does not validate a Nanite-like DAG, CLAS, or voxel foliage implementation in id Tech 8. Our automatic DAG and exclusion of manual object LODs remain unchanged. Cache resolution levels are a different concern from object LOD authoring.

Before extending DDGI, resolve cache keys across streamed cluster changes, moving leaves, material/light changes, and geometry-to-voxel transitions. The urban risk cases remain thin walls and indoor/outdoor boundaries. No no-leak claim or dense-forest performance estimate follows from the cited material.
