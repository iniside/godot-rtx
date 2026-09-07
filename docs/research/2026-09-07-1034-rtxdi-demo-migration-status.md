# RTXDI demo scene migration

Owner-authorized migration of `gi_demo/test.tscn` and correction of empty editor
scenes in `demos/rtxdi_manual`. Task baseline: `b01c5849a8d10397be3e977347b0a49f45c90db5`.
Source HEAD during validation: `73616056ee8049323d7077a81c930db1e6d87ba7`.
Executing agent: core-implementer, gpt-6-astra, high. Comments: default NONE.

## Changed authority and closure

All six existing case `.tscn` files now own their editable geometry, materials,
lights, cameras, environment and HUD. The runtime construction functions in
`main.gd` are removed; its remaining code handles input, motion, presentation and
capture. The shadow scenes retain two rows and five culling/mirroring columns,
plus the two-element negative-scale MultiMesh column. Energy and unsupported
material cases retain their corresponding authored materials. R still removes
and re-adds light nodes to exercise registration.

Imported geometry is referenced through `PackedScene` source paths and named
child overrides. The MultiMesh uses a saved `plane_mesh.tres` resource with its
102 bytes of cluster data. There are no `geometry.gltf::ArrayMesh_*` references.
The earlier unfinished converter produced unstable resource references, lost
material overrides and a null MultiMesh mesh; those on-disk defects are repaired.

`test.tscn` is a compact 5,013-byte scene with one normal zdm2 instance, four
original analytic-light placements, original camera placement, emissive cube
and two camera-attached imported meshes. Its copied map and cube are byte-identical
to the originals: map SHA-256 `D099555AC483C420F4BC7112E43F829F4394CEF0152AF35921C3F0E5EC998239`;
cube SHA-256 `6AD6F379B9E1AF6ABBED14CED3AC1829CCEDBBBED79F15E11953C34BC2333487`.
Legacy GI/probe/decal/PT switching is removed. The copied camera script retains
the authored orientation when mouse input begins. Attribution and F5/F6 controls
are in the demo README. Original `gi_demo` files are untouched.

The owner-modified `project.godot` is excluded. Its SHA-256 remains
`41CC7B1055D440B3267640675BD79942FD2EF4FA19AAE378592755B3E9FBD582` after validation.

## Resource evidence and visual boundary

The ordinary editor binary SHA-256 is
`55967155821B21DFF843D2FEFA663B84160405167714EAE7C98764D43EA795A2`.
One headless import completed the two previously missing SVG texture imports.
A temporary read-only resource inventory loaded and instantiated all seven scenes
through the actual ResourceLoader and printed their existing nodes and materials.
It contained no assertions and authored no scene files.

| Scene | Visible mesh nodes | Lights | Cameras |
|---|---:|---:|---:|
| main | 15 | 4 | 2 |
| shadows_merged | 11 plus 2 MultiMesh elements | 1 | 2 |
| shadows_expanded | 11 plus 2 MultiMesh elements | 1 | 2 |
| energy_directional | 1 | 1 | 2 |
| energy_emission | 1 | 0 | 2 |
| unsupported | 18 | 4 | 2 |
| test | 4 | 4 | 1 |

Readback confirmed culling modes 0/1/2 and double-sided shadow mode 2 in both
rows, two populated MultiMeshes with cluster data, gray directional material and
the intended emission-only material. The final initial blend-shape value of 0.65
was restored after this inventory. [Resource readback](rtxdi-demo-migration-evidence/resource-inventory.txt)
and [import output](rtxdi-demo-migration-evidence/import.txt) retain the evidence.

Current Vulkan runtime and editor-viewport visual verification are **not complete**.
The repaired map, a standalone resource-inspection script, and the unchanged
previously working directional-registry project all stalled before their first
script statement on the ordinary binary. Logs reached Vulkan 1.4.351, Forward+,
Streamline, RTX 4090 and the existing inline-shader warning, but no scene-ready
marker or capture. The migrated scene retry after the owner's editor independently
closed behaved the same. GPU memory at that point was 5,541/24,564 MiB.
This evidence does not identify an engine defect or attribute the stall to scene
serialization. [Unchanged control](rtxdi-demo-migration-evidence/unchanged-control-startup.txt)
and [migrated startup](rtxdi-demo-migration-evidence/migrated-startup.txt) retain the
actual boundary. Owned stalled processes were stopped; the owner's editor was
not closed or manipulated. Headless resource inspection establishes resource
closure only, not rendering correctness or clean runtime teardown.

No build, formatter or automated tests were run or authored for this scene change.
Actual populated-editor and rendered-map screenshots remain required before
claiming visual closure. The owner-used double-precision editor predates the
current RTXDI implementation and cannot establish current renderer behavior.

## Private deforming-mesh lifetime correction

Coordinator review of the migrated ownership found that an authored PackedScene
can retain the imported deformer mesh after its visible node is removed. The
gallery now duplicates that node's ArrayMesh during `_ready`, and Delete queues
the node for release and clears the script's node handle. The private runtime
mesh is owned by that MeshInstance; the serialized scene retains its editable
imported source. This restores the private mesh-RID release scenario without
runtime geometry construction. Shared source materials and shadow-mesh resources
are not claimed to be released by this action.

Clangd located `Resource::duplicate` at `core/io/resource.cpp:517` and
`ArrayMesh::_bind_methods` at `scene/resources/mesh.cpp:2303`. Source and Resource
XML confirm duplication constructs a new Resource and copies storage properties;
ArrayMesh bindings include blend-shape names and surfaces. `_get_surfaces` retains
cluster data, `_set_surfaces` creates the new mesh RID, and the ArrayMesh
destructor releases its RID. Git blame identifies the duplication implementation
as upstream. This correction has static ownership evidence only; actual delayed
GPU release remains within the pending runtime validation boundary.

The five temporary converter/single-import diagnostic files were removed with
one exact, nonrecursive `Remove-Item -LiteralPath` after their paths and task
ownership were inspected. The earlier bundled command rejection did not block
this narrower cleanup.

## Navigation and failure classes

Applicable classes: 1, 2, 3, 4, 6, 7, 8, 9. Expected topology is authored scene resources
through ordinary import/ResourceLoader/SceneTree into the existing RTXDI/NRD path.
No engine API, ClassDB registration, shader, backend or SCons changes were made.
The private runtime ArrayMesh uses the existing MeshInstance/ArrayMesh ownership
and queued Node deletion path.
Backend and template axes are not newly validated by this scene-only work.

Targeted GDScript/scene/glTF reads and `git show c8ba1736e1:.../main.gd` supplied
the original topology and control behavior. Bounded textual inventories of scene
references are lower bounds; actual resource loading supplied the closure evidence.
For the cluster-preservation question, clangd `find` against the root compile
database located `ResourceImporterScene::_generate_meshes` at
`editor/import/3d/resource_importer_scene.cpp:2697` and `ArrayMesh::_get_surfaces`
at `scene/resources/mesh.cpp:1514`. Actual source shows cluster generation before
mesh saving and cluster data serialization; `git blame` identifies generation
as fork-local `a643a15e48a`, while external mesh saving is upstream. ResourceSaver
and ArrayMesh class XML were inspected. No bound API was changed.

All six demo siblings and the migrated scene were loaded. Canonical renderer
status maintenance and fresh hostile/proof review are owned by the coordinator.
