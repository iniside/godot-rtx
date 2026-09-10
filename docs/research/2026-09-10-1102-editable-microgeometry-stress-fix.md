# Editable microgeometry stress scene

Source revision: `0af41a64e2a69741588de7b6fbbfa913a73959ea`.

The stress scene now stores 5000 Lucy and 5000 Thai external GLB scene
instances directly in `demos/rtxdi_manual/microgeometry_stress/scene.tscn`.
A Python text writer produced the serialized transforms; mesh data remains in
the imported assets. The scene file is 1,956,048 bytes and contains no embedded
ArrayMesh or `.godot/imported` references.

The runtime script no longer generates the population or overwrites the saved
floor and camera. It retains import checks, population diagnostics, HUD and orbit
controls. Standard GLB instances add one Node3D parent per rendered mesh; there
are still 10000 MeshInstance3D nodes sharing two mesh resources. SceneTree
scalability remains outside this correction.

Validation on 2026-09-10: the current ordinary editor binary ran the game on
Vulkan for 300 frames with orbit, exited 0 without ERROR lines, and reported
`lucy=5000 thai=5000 native_instance_rids=10000 shared_mesh_resources=2
serialized_roots=10000`. Evidence is in the local capture directory
`%TEMP%/godot-render-repair-20260909/`, `editable-stress-game.log` and its
`.receipt.json`. No engine rebuild, automated tests or performance comparison
was performed for this scene-only correction.

The additional ordinary Vulkan editor loaded the scene and admitted both assets
without ERROR lines, but did not finish its 240-frame autoquit within 180 seconds.
The capture helper timed out that process; normal editor shutdown and visual
appearance are not verified. Evidence: `editable-stress-editor.log` and its
receipt in the same capture directory. The owner's existing editor was untouched.
