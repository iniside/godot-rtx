# Dragon import inspection

This project imports the owner-provided `xyzrgb_dragon.glb` through Godot's
normal scene importer. The input has 3,609,600 vertices and 7,219,045 indexed
triangles in one static primitive. Its SHA-256 is
`6992f4b0f141f8c30ae9fc911d9d8ee34f24ff18bb2db9feefac795e291fe0c2`.
Keep this pre-existing input unchanged and outside the fixture commit.

Use a source-matched editor build of this fork. From the repository root,
set `$godot` to the full path of its executable and run:

```powershell
& $godot --headless --path rawcontent --editor --import --verbose --accessibility disabled
& $godot --headless --path rawcontent --script res://inspect_import.gd --accessibility disabled
```

The second command loads and instantiates the imported scene, then prints each
mesh's external `MicroGeometry` path, content ID, and native statistics. The
script is a manual inspection tool; its exit code alone is not a success check.
To rebuild the unchanged asset, open this project in the editor, select the GLB
in the FileSystem dock, and click **Reimport** in the Import dock. Then repeat
the second command. A repeated `--import` can skip unchanged assets.

Check the log for errors, the external `.mgdata` dependency, nonempty geometry
statistics, and the expected single mesh/surface. The `triangles` statistic sums
all hierarchy levels; it is neither the input count nor a selected rendering
count. `encoded_bytes` counts compressed page payloads; the complete `.mgdata`
also contains metadata. Godot creates the `.godot/` cache directory and the
`xyzrgb_dragon.glb.import` sidecar beside the input. Both remain untracked and
must not be staged with this fixture.

Open `project.godot` in the editor to inspect the imported dragon asset. The
project uses Forward+ and Vulkan. Headless import and resource inspection do
not validate raster/RT rendering, GPU page residency, or export. Recorded
commands, results, and the intermediate binary boundary are summarized in the
[implementation status](../docs/research/2026-09-09-0900-gpu-microgeometry-implementation-status.md).
