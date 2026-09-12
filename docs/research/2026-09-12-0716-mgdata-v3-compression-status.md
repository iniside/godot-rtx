# Microgeometry `.mgdata` v3 compression status

Verified 2026-09-12 UTC. Source: plan `3608e8b74f`, Krok 1 `e656e5b415` +
`dcab8fd312`, Krok 2 `f06247d0a5`, Krok 3 `c7f99ac2a2`; binary built from
the working tree at `dcab8fd312` with the Krok 3 edits (reports
`custom_build.dcab8fd31`). Windows Vulkan primary double editor, RTX 4090,
driver 616.64. Owner-approved architecture: disk format transcoded on the
GPU at page install into a CLAS-readable memory format, implicit tangents,
global position grid (import option, default 1/16 cm in mesh units). No
tests, no proof audit. Plan:
[2026-09-11-2048-mgdata-v3-cluster-compression-plan.md](../plans/2026-09-11-2048-mgdata-v3-cluster-compression-plan.md).

## Implementation

- Disk page per cluster: 24-byte header (grid minimum, bits per axis,
  vertex-id bits, counts, topology bytes, `first_primitive`) plus 10 bytes per
  UV component (two u16 intervals with the largest gap excluded, bit count,
  split flag); sections positions bitstream, octahedral normals u8×2, RGBA8,
  UV bitstreams, customs, generalized strips (reset/left/ref bits, 5-bit
  reference delta with a 7-bit escape), zigzag delta vertex ids. zstd and
  SHA-256 per page unchanged. No cross-cluster references. Pages are cut by
  both the memory (≤ 64 KiB) and decoded-disk (≤ 64 KiB) limits.
- Memory format in the pool: `R16G16B16_SNORM` positions in an isotropic
  per-asset frame, oct8 normal in bytes 6–7, UV u16×2 over the surface range
  (f16 when the range exceeds 8), RGBA8, customs; 12-byte stride for
  position+normal+UV, 8 for position+normal; u8×3 indices; u32 vertex ids;
  4-byte aligned blocks from `MicroGeometryData::compute_cluster_layout`.
  No tangent slot.
- Transcoder in `micro_geometry_page.slang`: one workgroup per cluster,
  staging ring of 16 × 64 KiB, per-page `ClusterTranscodeInfo` offset table
  in `page.clas_resources`, out-of-range indices clamped to degenerate
  triangles with a device fault counter. CLAS builds read snorm16 from the
  pool. The write-then-build ordering is tracked by the render graph
  (`RESOURCE_USAGE_GENERAL` on the pool) and both explicit dependencies were
  kept.
- Shaders decode by `attribute_formats`; positions decode to frame space,
  the frame is folded into the TLAS instance transform (current and previous)
  and as the right-most factor of emissive light transforms; raster applies
  it in `micro_geometry_pull_vertex`. Tangents: per vertex from UV gradients
  in the mesh shader (groupshared, 4312 B, probe threshold 8 KiB), per
  triangle in RT, with the previous arbitrary-axis fallback when UVs are
  absent or degenerate.
- Source triangles and vertices are renumbered at import to leaf-cluster
  order; `primitive_lookup` and `geometry_resolve_primitive` use
  `Cluster::first_primitive`.
- Importer format versions bumped 3 → 4. This does not reimport existing
  projects: `EditorFileSystem` revalidates importer versions only when the
  import settings hash changes (`editor_file_system.cpp:430-440`). The v2
  products in `.godot/imported` were removed by hand to force the reimport.

## Sizes (reimport 516 s total for four assets, headless double editor)

| Asset | v2 `.mgdata` | v3 `.mgdata` | ratio | v3 B/tri DAG (file) | v3 B/tri source | payload B/tri (builder) | import |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Lucy (28.06 M tri) | 1155.9 MB | 406.8 MB | 2.84× | 7.26 | 14.50 | 6.55 | 325 s |
| Thai statuette (10.0 M) | 415.3 MB | 140.8 MB | 2.95× | 7.05 | 14.08 | 6.33 | 102 s |
| xyzrgb dragon (7.22 M) | 296.4 MB | 97.3 MB | 3.05× | 6.75 | 13.48 | 6.04 | 75 s |
| seam_grid (512 tri) | 14.3 KB | 13.8 KB | 1.04× | 14.8 | 27.0 | 12.31 | 29 ms |

Manifest share rose from 3.1 % to 9.7 % (Lucy 39.6 MB): cluster records
grew 48 → 56 B and surfaces 64 → 160 B. zstd now gains only 9 % on the
dense pages (367 MB encoded vs 402 MB decoded for Lucy) versus 53 % on v2.
The plan target was ≤ 7 B/tri DAG for the payload; measured 6.0–6.6 B/tri
payload and 6.8–7.3 B/tri including the manifest. Nanite's published figure
is 5.6 B/tri including hierarchy. The remaining gap is the manifest and the
absence of vertex deduplication.

Position grid note: the stress assets are authored in millimetres (Lucy
frame scale 798.5 units), so the default 1/16 cm step is 0.000625 mesh units,
about 0.8 µm after the runtime 2 m normalisation. A coarser step
(`meshes/micro_geometry_position_step`) would reduce position bits further;
not measured.

Native assets regenerated with `--convert-microgeometry-stress` (81 s):
`native_assets/microgeometry_stress/lucy.mgdata` 406.8 MB,
`thai_statuette.mgdata` 140.8 MB; `lucy.res` remains 1.9 GB and
`thai_statuette.res` 680 MB (source arrays, Krok 5).

## Editor runs on `microgeometry_stress/scene.escn`

Launch: `--editor --path demos/rtxdi_manual --rendering-driver vulkan
--benchmark --verbose --quit-after N`.

| Run | Shader cache | Scene visible at | `load_subset` | asset load | ERROR lines | transcode faults |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 1 (600 frames) | cold, 129 misses | 226.8 s | 12.94 s | 9.72 s | 0 | none reported |
| 2 (1500 frames) | warm | ~9 s | 8.46 s | – | 0 | none reported |
| 3 (2400 frames) | warm | ~9 s | 8.53 s | 5.47 s | 0 | none reported |
| 4 (9000 frames) | warm | ~9 s | – | – | 0 | none reported |

Asset load fell from 6.4 s (v2 warm) to 5.5 s because the `.scn` files are
smaller; the triple load per asset from the loader analysis remains.

Streaming settles in about one second after load at 315 resident pages,
16 124 CLAS, 107 shared cuts, about 173 000 selected clusters, roughly 300
pages/s installed through the staging ring. This is far fewer clusters than
the 5–7 million recorded on 2026-09-11 with v2; the cause is the owner's
commit `f0ddf48fa2` (2026-09-11 17:36), which removed the hybrid-mode 1 px
clamp so the scene's configured RT geometry error (4 px, offscreen
multiplier 2) now drives selection. It is not a v3 effect; no v2 run at the
same revision exists for a matched comparison.

Editor viewport HUD in run 4: 165 FPS, GPU about 6 ms, CPU about 3.6 ms.

## Visual check

Window captures of run 4 at 28, 43 and 58 s after launch (scratchpad, not
committed) show the populated 100×100 grid with recognizable Lucy and Thai
geometry, smooth shading, correct shadows and no cracks, missing clusters or
exploding triangles at full-window scale or in 24 % crops. The nearest
statuette shows a coarse top surface consistent with the 4 px RT error
setting. The stress environment uses Mega Geometry primary rays with RTXDI
and DDGI, so the RT decode path (positions, normals, TLAS frame) is
exercised by these captures. There is no normal-mapped or emissive material
on a clustered mesh in any `.escn` scene, so implicit-tangent quality under
normal maps and emissive light sampling on clustered geometry remain
unverified. `.tscn` scenes (seam_grid, dragon) are not used by this project.

## Limits

- No automated tests, no proof audit, no Nsight capture.
- Transcoder timing per page and the raster tangent pass cost were not
  measured separately; the 6 ms GPU frame is the only aggregate.
- The `transcode_faults` statistic is not printed by the existing verbose
  cut report; absence of faults is inferred from clean geometry and zero
  ERROR lines, not from a printed counter.
- Camera motion under v3 (previous transform with the frame): the owner
  watched the editor during the motion-tool runs on 2026-09-12 and confirmed
  the scene renders correctly while the camera moves. The tool's own result
  was cancelled because the capture helper changed window focus, and FPS
  during those runs is not representative for the same reason; no numeric
  motion measurement is claimed.
