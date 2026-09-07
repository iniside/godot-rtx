# RTXDI / NRD manual demonstration

Open this project with the editor built from this fork, on the supported Vulkan RT device:

```powershell
G:\Projects\godot-rtx\bin\godot.windows.editor.x86_64.exe --editor --path G:\Projects\godot-rtx\demos\rtxdi_manual --rendering-method forward_plus --rendering-driver vulkan
```

Let the editor import `geometry.gltf`. Press **F5** to run the main gallery. To inspect an individual case, open its `.tscn` in the FileSystem dock, then press **F6** (Run Current Scene). Focus the running game window before using its controls. Stop and restart between the merged and expanded cases because the MultiMesh build threshold is read on first use.

| Scene | What to inspect |
|---|---|
| `main.tscn` | Animated gallery: rough dielectric and metallic spheres, masked checker, textured emission, all light types, environment and off-screen caster. |
| `shadows_merged.tscn` | Orthographic comparison of culling and shadow modes, with merged MultiMesh geometry. |
| `shadows_expanded.tscn` | The same scene with one TLAS instance per MultiMesh element. Compare the shadow topology with the merged case. |
| `energy_directional.tscn` | A gray plane lit by one unit directional light, without sky or specular contribution. |
| `energy_emission.tscn` | A plane showing authored emission alone, without incident direct lighting. |
| `unsupported.tscn` | Explicit magenta diagnostics for custom `light()`, anisotropy and alpha blending. |

The root node also exposes **Preset**, **Light Mode** and **Expanded Multimesh** in the Inspector. These select the same arrangements used by the case scenes. They do not enable a different renderer.

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
| Delete | Remove the deforming mesh and release its retained source mesh. |
| F12 | Save a screenshot to the project's user-data directory; the Output log prints its path. |
| Escape | Close the running scene. |

In the shadow scenes, columns from left to right are **cull back**, **cull front**, **cull disabled**, **explicit double-sided shadows**, **mirrored MeshInstance**, and **negative-local-scale MultiMesh**. The upper row faces up; the lower row faces down. Default back culling casts from the upper row, front culling from the lower row, and disabled/double-sided shadows from both. Mirrored MeshInstance preserves its owner-facing convention; local mirroring inside MultiMesh reverses winding, consistently in both build modes. The explicit double-sided lower-row surface is invisible to the camera but still casts a shadow. Press B for a direct shadow-on/off comparison.

This milestone renders direct diffuse/specular light, surface emission and the environment. It does not add bounced lighting or reflections of secondary surfaces. Very smooth metals can therefore be dark away from directly visible light samples. No PT environment settings or renderer enable switches are needed.

Optional command-line capture of the same real scene:

```powershell
G:\Projects\godot-rtx\bin\godot.windows.editor.x86_64.console.exe --path G:\Projects\godot-rtx\demos\rtxdi_manual --gpu-profile -- --preset=gallery --still --delay=8 --capture=C:/Temp/gallery.png
```

Create the destination folder first. `--preset`, `--light`, `--expanded`, `--dual`, `--fog`, and `--upscale` select presentation states; `--capture` saves one viewport image and closes after the delay. There are no automated assertions or tests. Final implementation evidence is maintained in [Stage 7 rendering status](../../docs/research/2026-09-07-0848-rtxdi-stage7-rendering-status.md).
