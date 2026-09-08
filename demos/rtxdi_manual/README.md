# Hybrid DDGI / path tracing / reconstruction manual demonstration

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

The manual-case root nodes expose **Preset**, **Light Mode**, **Expanded Multimesh** and **Renderer Mode** in the Inspector. Open `WorldEnvironment` -> its Environment resource -> Ray Tracing to edit DDGI cascade settings and PT samples per pixel, maximum bounces and accumulation. PT fields become visible when the Environment rendering mode is Path Traced; samples and accumulation additionally require denoiser None. The root's Renderer Mode selects the initial runtime mode. The main gallery stores these settings directly in the scene. NRD/RR previews use one matching primary sample; raw PT uses the selected samples per pixel.

Use keys **1–5** to compare Hybrid NRD, Hybrid RR, PT NRD, PT RR and raw progressive PT at the same camera. **T** toggles ordinary DLSS SR for either NRD mode. **U** toggles native/67% input resolution, retaining DLSS in RR/SR modes and selecting FSR2 otherwise. RR requests NVIDIA DLSS viewport scaling and bypasses NRD; unsupported combinations are diagnosed in the renderer Output. There is no script-exposed RR capability query in this build, so the HUD labels this a request rather than claiming successful evaluation.

Raw PT first disables temporal AA, frame generation and upscaling, sets native scale, enables progressive accumulation and pauses geometry motion. Space can resume motion; camera movement or scene changes reset accumulation. Fog remains an independent control: turn it off for surface-lighting reference comparisons. Keep exposure fixed and record samples/bounces. Modes also apply to the optional second mono viewport, whose camera and renderer history remain independent.

To request the bundled **DLSS SR 4.5** model explicitly, set Project Settings -> Rendering -> Streamline -> **DLSS Preset** (`rendering/streamline/dlss_preset`) to **M**, then start the gallery and press **1**, **T**. The default `?` preset can select K at Quality and does not establish 4.5. RR uses its separate D/E models; selecting RR is not selecting SR 4.5. Inspect the renderer's runtime provenance and successful evaluation before claiming the effective model. This demonstration does not change the project's saved scaling or preset settings.

| Control | Action |
|---|---|
| RMB + mouse; WASD / Q / E | Look; move horizontally / down / up. |
| Space | Pause/resume moving and deforming geometry. |
| 1 / 2 / 3 / 4 / 5 | Hybrid NRD / Hybrid RR / PT NRD / PT RR / raw PT. |
| T | Toggle ordinary DLSS SR for Hybrid NRD or PT NRD. |
| C | Cut between two cameras. |
| V | Show/hide a second independent mono viewport. |
| F | Toggle volumetric fog. |
| U | Toggle native/67% resolution; retain selected DLSS or use FSR2 with NRD. Disabled in raw PT. |
| Z | Resize between 1280×720 and 1024×640. |
| L | Remove the oldest remaining analytic light. |
| R | Reverse and re-register the analytic light nodes. |
| B | Toggle analytic-light shadows. |
| O | Show/hide the off-screen caster in the gallery. |
| Delete | Remove the deforming node and release its private runtime mesh. |
| F12 | Save a screenshot to the project's user-data directory; the Output log prints its path. |
| Escape | Close the running scene. |

In the shadow scenes, columns from left to right are **cull back**, **cull front**, **cull disabled**, **explicit double-sided shadows**, **mirrored MeshInstance**, and **negative-local-scale MultiMesh**. The upper row faces up; the lower row faces down. Default back culling casts from the upper row, front culling from the lower row, and disabled/double-sided shadows from both. Mirrored MeshInstance preserves its owner-facing convention; local mirroring inside MultiMesh reverses winding, consistently in both build modes. The explicit double-sided lower-row surface is invisible to the camera but still casts a shadow. Press B for a direct shadow-on/off comparison.

In `test.tscn`, use WASD or the arrow keys to move, Q/E to descend/ascend, and the mouse to look while captured. Escape or F10 releases/captures the mouse. Space pauses the emissive cube, B toggles analytic-light shadows, and F toggles volumetric fog. Keys 6, 7, 8 and 9 select DDGI probes, probe state, cascade weights and isolated material-weighted DDGI indirect; 0 returns to the normal image. G independently freezes or resumes the camera-following DDGI anchor while camera transforms and lighting updates remain live. The HUD reports the active renderer mode, denoiser, DDGI cascade count, probe spacing, rays, update budget, PT samples/bounces, debug view and freeze state from the runtime resources.

Hybrid uses RTXDI direct lighting plus camera-following DDGI diffuse bounce lighting. Camera-ray PT provides the secondary-surface comparison without DDGI feedback. Primary fog and retained postprocessing remain shared; alpha blending and unsupported geometry/material cases retain their diagnostics. The presence of these controls is not GPU validation evidence.

Optional command-line capture of the same real scene:

```powershell
G:\Projects\godot-rtx\bin\godot.windows.editor.x86_64.console.exe --path G:\Projects\godot-rtx\demos\rtxdi_manual --gpu-profile -- --still --delay=8 --capture=C:/Temp/gallery.png
```

Create the destination folder first. `--mode=hybrid_nrd|hybrid_rr|pt_nrd|pt_rr|pt_raw`, `--sr`, `--light`, `--expanded`, `--dual`, `--fog`, and `--upscale` select presentation states; `--capture` saves one viewport image and closes after the delay. For example, add `--mode=pt_raw --still` for progressive PT, or `--mode=hybrid_nrd --sr` for NRD plus ordinary SR. There are no automated assertions or tests. Historical baseline evidence is maintained in [Stage 7 rendering status](../../docs/research/2026-09-07-0848-rtxdi-stage7-rendering-status.md); it does not validate the new PT/RR paths.

For the DDGI motion diagnostic, run `test.tscn` as the main scene or with the editor's Run Current Scene command. `--ddgi-view=normal|probes|state|weights|indirect` selects the initial output and `--ddgi-freeze` freezes only the DDGI anchor. `--ddgi-motion=forward-return|rotation` starts a deterministic 96-rendered-frame capture after the existing `--delay`. It requires `--capture=<existing-folder>/<name>.png`; the script writes `<name>-000.png` through `<name>-095.png` and logs the post-draw engine frame, wall-clock timestamp, frame delta, camera pose, phase, capture result and current renderer/DDGI settings for every image. The images are observations, not performance measurements.

The forward-return sequence moves along the initial camera forward vector for 8 rendered frames, holds at the destination for 40 frames, returns exactly over the next 8, then holds for 40 recovery frames. The default 4 m travel is 0.5 m per rendered frame, equivalent to 30 m/s at 60 FPS before capture overhead. `--ddgi-distance=<meters>` sets the total travel. At the authored camera position, 0.25 m forward stays inside the initial base-grid cells and 4 m crosses boundaries. For another starting position, confirm the minimum cells: travel smaller than the probe spacing can still cross a nearby boundary. The rotation control keeps the camera position fixed, yaws 24 degrees over 8 frames, holds for 40, returns over 8 and holds for 40. Both sequences pause the emissive cube before the warmup delay, disable camera processing and input during the fixed trajectory, restore those states at completion, and keep motion tied to rendered frame count so separate debug modes remain comparable despite PNG readback time. A representative launch is:

```powershell
G:\Projects\godot-rtx\bin\godot.windows.editor.x86_64.console.exe --path G:\Projects\godot-rtx\demos\rtxdi_manual res://test.tscn --rendering-method forward_plus --rendering-driver vulkan --max-fps 60 -- --delay=8 --capture=C:/Temp/ddgi/moving.png --ddgi-view=weights --ddgi-motion=forward-return --ddgi-distance=4
```

`migrated/zdm2.glb` is copied from `gi_demo/zdm2.glb`, which is derived from the Cube 2: Sauerbraten map "zdm2" and licensed under [CC BY 4.0 Unported](https://github.com/Calinou/game-maps-obj/blob/master/sauerbraten/zdm2.txt). The converted source is available from the [game-maps-obj repository](https://github.com/Calinou/game-maps-obj). `migrated/cube.glb` is copied from the same Godot global-illumination demo solely to preserve its emissive-cube setup.
