# Microgeometry manual fixture

`scene.tscn` is the dedicated Step 6 visual fixture for the GPU-driven microgeometry path. It keeps the 7.2-million-triangle dragon and the two-surface `seam_grid.obj` as normal `res://` imported resources, with the camera, environment, floor, lights, rigid instances, an alpha-scissored moving dragon and panel, retained deformer, second viewport, and HUD serialized for editor inspection.

The owner input remains unchanged at `rawcontent/xyzrgb_dragon.glb`. From the repository root, create the project-local import source with:

```powershell
Copy-Item -LiteralPath rawcontent/xyzrgb_dragon.glb -Destination demos/rtxdi_manual/microgeometry/xyzrgb_dragon.glb
Get-FileHash -Algorithm SHA256 rawcontent/xyzrgb_dragon.glb,demos/rtxdi_manual/microgeometry/xyzrgb_dragon.glb
```

Both files must be 202,136,944 bytes with SHA-256 `6992f4b0f141f8c30ae9fc911d9d8ee34f24ff18bb2db9feefac795e291fe0c2`. Keep the owner input, project-local copy, `.import` sidecars, and `.godot/imported` cache untracked and unstaged. The GLB POSITION accessor bounds are `(-100.871765, -56.013134, -67.100975)` to `(100.871765, 56.013134, 67.100975)`; the serialized primary transform scales it to about five units wide and places its minimum Y on the floor.

After building the source-matched editor, open the scene directly so the existing project main scene remains untouched:

```powershell
.\bin\godot.windows.editor.x86_64.exe --path demos/rtxdi_manual --editor res://microgeometry/scene.tscn
.\bin\godot.windows.editor.x86_64.exe --path demos/rtxdi_manual res://microgeometry/scene.tscn
```

Use a Vulkan ray-tracing device. The HUD shows delayed GPU selection counters for raster and RT clusters/triangles, residency, page/geometry/AS memory, CLAS/BLAS builds, residency pressure, and measured GPU time.

The standalone alpha panel is a native primitive fallback check. The moving imported dragon receives the same alpha-scissored material at runtime to exercise the intended microgeometry alpha path. `seam_grid.obj` contains 512 source triangles split evenly across two materials that share the center boundary. The importer calls the microgeometry builder for every surface and the builder uses 128-triangle clusters; treat both imported paths as fixture intent until the matched Vulkan run confirms eligibility through the counters and debug view.

- `F1`: normal rendering
- `F2`: selected raster cluster colors
- `F3`: selected RT cluster colors
- `F4`: freeze or resume the current microgeometry selection
- `Space`: pause or resume rigid and MultiMesh motion
- `M`: toggle the dragon MultiMesh
- `N`: save the primary imported `ArrayMesh` to `res://microgeometry/native_roundtrip.res`, load it with `CACHE_MODE_REPLACE`, and assign it back
- `I`: toggle the primary dragon from non-emissive to emissive and back
- `O`: toggle the offscreen dragon
- `G`: cycle RT geometry error through 0.5, 4, and 16 output pixels
- `H`: cycle offscreen error multiplier through 1, 2, and 8
- `P`: switch the main camera far plane between 18 and 200
- `V`: enable or disable the shared-world second viewport
- `U`: remove all live dragon instances, then reload them with `CACHE_MODE_REPLACE`
- `F12`: save a screenshot under `user://`
- Right mouse plus `WASD`/`QE`: look and move

For a delayed capture, pass fixture arguments after `--`:

```powershell
.\bin\godot.windows.editor.x86_64.exe --path demos/rtxdi_manual res://microgeometry/scene.tscn -- --view=rt --freeze-after=5 --still --emissive --secondary --orbit --save-native=res://microgeometry/native_roundtrip.res --capture=user://microgeometry-rt.png --delay=10
```

`--freeze` still freezes immediately. `--freeze-after=<seconds>` freezes one settled live selection, `--orbit` continuously orbits the otherwise free main camera, and `--save-native=<res://path.res>` runs the same native round-trip as `N`. `--rt-error=<value>`, `--offscreen-multiplier=<value>`, and `--far=<value>` set the existing Environment and Camera scalars for same-view comparisons. The generated `.res` is manual output; keep it untracked and unstaged.

The fixture contains no automated checks or assertions. A source or scene inspection does not establish GLB import, Vulkan rendering, page streaming, RT/raster selection, shader output, or capture correctness; record those only after running this scene with the matching binary and real device.

For selected-scene export with resource customization, copy `export_presets.cfg.example` to the project's `export_presets.cfg` only when no preset file exists, then run:

```powershell
.\bin\godot.windows.editor.x86_64.exe --headless --path demos/rtxdi_manual --editor --script res://microgeometry/manual_export.gd --export-pack "Microgeometry Selected Scene" C:/Temp/microgeometry-selected.pck
.\bin\godot.windows.template_debug.x86_64.exe --path C:/Temp --main-pack C:/Temp/microgeometry-selected.pck res://microgeometry/scene.tscn
```

The export script prints each customized `MicroGeometry` and its statistics. Increment its customization hash to force callbacks after a cached export. Keep the temporary project preset and PCK unstaged. This editor `--script` route has a known `Main::start()` default-SceneTree shutdown leak; its exit code alone does not establish a clean shutdown. Inspect the exported PCK with the template from a directory without project sources to check actual packaged payload reads.
