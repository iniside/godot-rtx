# Dense microgeometry workload

Open `scene.tscn` in the `demos/rtxdi_manual` project. At runtime it creates
5,000 Lucy and 5,000 Thai statuette `MeshInstance3D` nodes beneath `Instances`,
sharing two imported `ArrayMesh` resources. The editor scene contains the
population script, resource references, camera, environment, ground and sun;
the 10,000 generated nodes appear in the remote scene tree after startup.
There is no duplicated mesh serialization, MultiMesh, per-object script,
physics body or per-frame object loop.

The 100 by 100 checkerboard uses a 25 cm gap beyond the larger normalized
footprint on each horizontal axis. Each object is 2 m tall. Lucy's source Z axis
is rotated onto world Y; the Thai source already uses Y for height. Imported
AABBs determine normalization, centering, ground placement and cell spacing.
All objects remain static. The camera is still by default; `--orbit` moves
only the camera on a deterministic path. There is one game viewport.

Environment settings match `../microgeometry/scene.tscn`: RT geometry error 4,
offscreen multiplier 2, Quarter RTXDI and DDGI interpolation, four DDGI cascades
and two updates per frame. The sky, tone mapping, ground material and sun also
use that scene's values. This fixture changes workload, not renderer quality.

## Source assets and normal import

Preserve the owner-provided originals under repository-root `rawcontent/`.
Copy them byte-for-byte into this directory before the first import:

```powershell
Copy-Item -LiteralPath rawcontent/lucy.glb -Destination demos/rtxdi_manual/microgeometry_stress/lucy.glb
Copy-Item -LiteralPath rawcontent/thai_statuette.glb -Destination demos/rtxdi_manual/microgeometry_stress/thai_statuette.glb
.\bin\godot.windows.editor.x86_64.exe --headless --editor --path demos/rtxdi_manual --import
```

The large GLB copies and generated `.godot` cache are not part of this fixture's
text commit. Use the original assets with these SHA256 identities:

| Source | Bytes | Vertices | Source triangles | SHA256 |
| --- | ---: | ---: | ---: | --- |
| `rawcontent/lucy.glb` | 785561992 | 14027872 | 28055742 | `ab6d71b655b68ce3164d375a83d7a63091b88da2d2ce735c55d62775ff474e82` |
| `rawcontent/thai_statuette.glb` | 280001084 | 4999996 | 10000000 | `29a8a750d19778a8bb8a46436d29238c7c3fcbf73b3a9a15bffa6b8173321d24` |

Both inputs contain one indexed triangle mesh, one mesh node and no skins or
animations. These counts and bounds were read from the GLB JSON accessors;
they do not establish successful Godot import. The normal scene importer calls
`import_scene_micro_geometry()` after post-processing and emits `.mgdata`
alongside its `.scn`. Microgeometry conversion is automatic for eligible meshes;
`meshes/generate_lods` remains at its normal default and is not its enable switch.
There is no custom importer or import script.

Inspect the actual imported resources without populating the field:

```powershell
.\bin\godot.windows.editor.x86_64.exe --headless --path demos/rtxdi_manual res://microgeometry_stress/scene.tscn -- --inspect-import
```

`MICRO_STRESS_IMPORT` logs each mesh's AABB, content ID, `.mgdata` path and
native `MicroGeometry.get_statistics()` values. Missing microgeometry or empty
level/page counts stop the scene with an error. `--inspect-import` exits after
these resource diagnostics; it is not GPU rendering evidence.

## Runtime and measurement

```powershell
.\bin\godot.windows.editor.x86_64.exe --path demos/rtxdi_manual --rendering-driver vulkan res://microgeometry_stress/scene.tscn
.\bin\godot.windows.editor.x86_64.exe --path demos/rtxdi_manual --rendering-driver vulkan --gpu-profile res://microgeometry_stress/scene.tscn -- --orbit
```

Optional `--capture=user://microgeometry-stress.png --delay=10` captures the
single viewport after population and the requested delay. The capture log
records the PNG save result; a successful save does not establish image quality.

`MICRO_STRESS_POPULATED` counts the actual child mesh nodes by shared resource
identity and counts their valid native rendering-instance RIDs. The expected
values are `lucy=5000 thai=5000 native_instance_rids=10000`; the ground is excluded.
Its setup time covers normalization, object creation, placement and counting,
not the earlier PackedScene resource load. No population scan runs afterward.

Exclude asset loading, population and subsequent streaming/selection warmup
from steady-state renderer measurements. Retain the executable and DLL hashes,
source revision and working diff, GLB/import-output hashes, effective project
settings and native profiler logs. Use the existing timestamp budget 2048 and
unchanged resolution/quality when comparing still and orbiting runs. Report
SceneTree setup separately from renderer CPU/GPU time; fixing SceneTree logic
at 10,000 nodes is outside this renderer fixture's scope.

## Validation receipts, 2026-09-09 UTC

Normal headless editor import finished in 436.19 seconds with exit 0 and empty
stderr. Native resource inspection and a three-frame headless scene run also
exited 0 with empty stderr, establishing parser/load/population and teardown
closure. The population log reports exactly 5,000 Lucy, 5,000 Thai and 10,000
valid native instance RIDs sharing two meshes. The measured field is
143.78761 by 127.59603 m, with cells 1.437876 by 1.275960 m. Population took
93.133 ms in this one headless run; this is not renderer or GPU frame time.

These runs use the pinned Step 3 double editor at
`C:/Users/lukas/AppData/Local/Temp/godot-render-repair-20260909/step3-bin/godot.windows.editor.double.x86_64.exe`,
SHA256 `4e0ff3dd0979531bdc0d999a229cf11c34da307b4899052b2e3013f517d7cbff`.
Its source coverage is recorded by the parent task at
`0d1ed0abe9f9395c394c87a3f60ea73ba3179b02`; its embedded version banner reports
the earlier build-start revision `93f37d12b`. Exact commands, exit receipts,
stdout/stderr, input hashes and output hashes are retained under
`C:/Users/lukas/AppData/Local/Temp/godot-dense-microgeometry-20260909/` in
`import.started.json`, `import.finished.json`, `inspect.receipt.json`,
`population.receipt.json`, `inputs.json` and `import-outputs.json`.

| Imported mesh | DAG clusters | Leaf clusters | Levels | Pages | Metadata bytes | Encoded page bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Lucy | 634477 | 316198 | 19 | 37918 | 35988688 | 1119944724 |
| Thai statuette | 229774 | 114600 | 18 | 13505 | 13008892 | 402321650 |

Metadata sizes come from the native `.mgdata` header; other statistics come
from `MicroGeometry.get_statistics()`. Content IDs are
`1da9dd08343174cbd84988b595e3d2aaa4c5f8b79c2ccff7bf39467e50a4b597` (Lucy)
and `01cd040e38f190013d41b7fd8811bd6fd044b3594d0ef36affa81dca6c6039f3` (Thai).

| Output | Bytes | SHA256 |
| --- | ---: | --- |
| Lucy `.mgdata` | 1155933508 | `000c560c5c902930cdea38e20c30e35dda0965d7864efc9a8d82b3c89d47ec54` |
| Lucy `.scn` | 812590558 | `ea84c2d6abac26e36d71802d3d4cef507a5e4a15b72a717a8de61d6ac3831232` |
| Thai `.mgdata` | 415330638 | `3ee7d9f8897c7c09cdf093d1cce5f2c4af159754d6ebf7fb09e8843edd48b976` |
| Thai `.scn` | 292588810 | `253c30a7ec110827598de02f29ddad206e799c7bbacd0c506851175f65c148a9` |

Headless RID validity establishes renderer API instance allocation only. The
configured `forward_plus`/`vulkan` labels returned by RenderingServer in that
run do not establish a real Vulkan device. GPU cut/BLAS sharing, page streaming,
appearance, steady-state frame time and capture remain unverified.

## Renderer capacity boundary

Source baseline: `93f37d12b4871a0f154a501d2434987d52811adb` plus concurrent
renderer work. Source inspection finds the existing RT preparation sums full
DAG cluster counts across instances and rejects work above `UINT32_MAX / 8`.
The builder caps each cluster at 128 triangles, so these inputs need at least
297,311 leaf clusters combined, or 1,486,555,000 for 5,000 instances of each.
This already exceeds the 536,870,911 range ceiling. Two membership buffers and
cluster references alone would require at least 23,784,880,000 bytes before
internal DAG clusters, selection, BLAS, scratch and shared geometry allocations.

The actual imported DAG counts yield 4,321,255,000 work items and
69,140,080,000 bytes for those three buffers alone. The fixture preserves the
full workload. Renderer capacity must be resolved at its authority before a
successful Vulkan performance run can be claimed. No automated tests are
included or authorized.
