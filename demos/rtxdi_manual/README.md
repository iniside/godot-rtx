# RTXDI / NRD manual demonstration

Open this project with the editor built from this fork, on the supported Vulkan RT device:

```powershell
G:\Projects\godot-rtx\bin\godot.windows.editor.x86_64.exe --editor --path G:\Projects\godot-rtx\demos\rtxdi_manual --rendering-method forward_plus --rendering-driver vulkan
```

Let the editor finish importing `geometry.gltf` and the assets under `migrated/`. Press **F5** to run the main gallery. To inspect an individual case, open its `.tscn` in the FileSystem dock, then press **F6** (Run Current Scene). Each scene stores its geometry, lights, cameras, environment, and HUD as editable nodes; the scripts only drive interaction and capture. Focus the running game window before using its controls. Stop and restart between the merged and expanded cases because the MultiMesh build threshold is read on first use.

| Scene | What to inspect |
|---|---|
| `main.tscn` | Animated gallery: rough dielectric and metallic spheres, masked checker, textured emission, all light types, environment and off-screen caster. |
| `shadows_merged.tscn` | Orthographic comparison of culling and shadow modes, with merged MultiMesh geometry. |
| `shadows_expanded.tscn` | The same scene with one TLAS instance per MultiMesh element. Compare the shadow topology with the merged case. |
| `energy_directional.tscn` | A gray plane lit by one unit directional light, without sky or specular contribution. |
| `energy_emission.tscn` | A plane showing authored emission alone, without incident direct lighting. |
| `unsupported.tscn` | Explicit magenta diagnostics for custom `light()`, anisotropy and alpha blending. |
| `test.tscn` | Migrated `gi_demo/test.tscn`: one zdm2 map, the original analytic-light and camera placement, an animated emissive cube, and imported camera-attached meshes under RTXDI + NRD. |

The `geometry_parts/*.tscn` scenes instance `geometry.gltf` through its stable source path and select the corresponding mesh child. `geometry_parts/plane_mesh.tres` stores the imported plane with its cluster data for the authored MultiMesh resource. No scene addresses importer-generated `ArrayMesh_*` identifiers.

The manual-case root nodes expose **Preset**, **Light Mode** and **Expanded Multimesh** in the Inspector. Preset identifies the authored arrangement, Light Mode filters its authored lighting, and Expanded Multimesh sets the corresponding ray-tracing build threshold. They do not enable a different renderer.

| Control | Action |
|---|---|
| RMB + mouse; WASD / Q / E | Look; move horizontally / down / up. |
| Space | Pause/resume moving and deforming geometry. |
| C | Cut between two cameras. |
| V | Show/hide a second independent mono viewport. |
| F | Toggle volumetric fog. |
| U | Toggle FSR2 between native and 67% internal resolution. |
| Z | Resize between 1280×720 and 1024×640. |
| L | Remove the oldest remaining analytic light. |
| R | Reverse and re-register the analytic light nodes. |
| B | Toggle analytic-light shadows. |
| O | Show/hide the off-screen caster in the gallery. |
| Delete | Remove the deforming node and release its private runtime mesh. |
| F12 | Save a screenshot to the project's user-data directory; the Output log prints its path. |
| Escape | Close the running scene. |

In the shadow scenes, columns from left to right are **cull back**, **cull front**, **cull disabled**, **explicit double-sided shadows**, **mirrored MeshInstance**, and **negative-local-scale MultiMesh**. The upper row faces up; the lower row faces down. Default back culling casts from the upper row, front culling from the lower row, and disabled/double-sided shadows from both. Mirrored MeshInstance preserves its owner-facing convention; local mirroring inside MultiMesh reverses winding, consistently in both build modes. The explicit double-sided lower-row surface is invisible to the camera but still casts a shadow. Press B for a direct shadow-on/off comparison.

In `test.tscn`, use WASD or the arrow keys to move, Q/E to descend/ascend, and the mouse to look while captured. Escape or F10 releases/captures the mouse. Space pauses the emissive cube, B toggles analytic-light shadows, and F toggles volumetric fog. The retired LightmapGI, VoxelGI, SDFGI, ReflectionProbe, SSAO/SSIL and path-tracing controls are absent from this migrated RTXDI + NRD scene.

This milestone renders direct diffuse/specular light, surface emission and the environment. It does not add bounced lighting or reflections of secondary surfaces. Very smooth metals can therefore be dark away from directly visible light samples. No PT environment settings or renderer enable switches are needed.

Optional command-line capture of the same real scene:

```powershell
G:\Projects\godot-rtx\bin\godot.windows.editor.x86_64.console.exe --path G:\Projects\godot-rtx\demos\rtxdi_manual --gpu-profile -- --still --delay=8 --capture=C:/Temp/gallery.png
```

Create the destination folder first. `--light`, `--expanded`, `--dual`, `--fog`, and `--upscale` select presentation states; `--capture` saves one viewport image and closes after the delay. There are no automated assertions or tests. Final renderer implementation evidence is maintained in [Stage 7 rendering status](../../docs/research/2026-09-07-0848-rtxdi-stage7-rendering-status.md).

`migrated/zdm2.glb` is copied from `gi_demo/zdm2.glb`, which is derived from the Cube 2: Sauerbraten map "zdm2" and licensed under [CC BY 4.0 Unported](https://github.com/Calinou/game-maps-obj/blob/master/sauerbraten/zdm2.txt). The converted source is available from the [game-maps-obj repository](https://github.com/Calinou/game-maps-obj). `migrated/cube.glb` is copied from the same Godot global-illumination demo solely to preserve its emissive-cube setup.
