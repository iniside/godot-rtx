# Make the stress scene visible in the editor

Owner reports an empty `microgeometry_stress/scene.tscn` in the editor.
Baseline `f9329167c6`. The file contains an empty Instances node; its script
creates5000 Lucy and5000 Thai meshes only in runtime `_ready`.

## Step1 — Store the existing population in the scene [independent]

Own `demos/rtxdi_manual/microgeometry_stress/scene.{tscn,gd}`. Serialize the
existing10000 native MeshInstance3D render instances with the same transforms, spacing,
two shared imported mesh resources, floor/camera/environment and HUD. Retain
imported microgeometry/DAG references without embedding10000 resource copies
or persisting import-cache paths. Use Godot resource serialization for authoring
where needed; do not hand-edit generated import data.

Remove the runtime population path in the same change. Keep runtime camera
controls, timing HUD and useful existing import/population diagnostics. Runtime
must render the saved scene without duplicating or resetting edited geometry.
No @tool regeneration, MultiMesh substitution, renderer changes, quality changes
or editor/SceneTree optimization is requested.

Serialization finding, 2026-09-10: ResourceSaver expands direct imported mesh
references into embedded data; standalone GLB subresource paths do not load.
Use ordinary external PackedScene instances of the two GLBs instead. Their
existing Node3D roots add one transform parent per mesh; preserve the same mesh
world transforms by accounting for the imported child transform. The earlier
no-wrapper restriction was an implementation assumption, not an owner request;
replace it with this standard import path rather than inventing a resource
loader/format or copying mesh payloads. The render population remains10000;
total scene nodes increase. Runtime must traverse those imported children for
its existing diagnostics without creating another population.

Validate resource loading and the existing game on one viewport with the current
binary, confirming10000 instances/two mesh resources and no creation of a second
population. Check editor-visible serialized content. No automated tests, C++
build, new benchmark fixture or performance campaign. At most one brief code
check; preserve the owner's running editor and unrelated dirty files. Update
the canonical scene status with results and actual limits.
