# DDGI GPU diagnostics

Debug-enabled builds support explicitly requested, bounded GPU snapshots. Set
`GODOT_DDGI_CAPTURE_PREFIX` to an existing directory plus a filename prefix before
starting Godot. Set `GODOT_DDGI_CAPTURE_FRAMES` to increasing comma-separated
positive frame numbers, for example `110,121,130,181,190`. The default is `120`.
Frame numbers belong to each DDGI render-buffer context, not the SceneTree.
Recreating a context starts its counter again and produces a new RID suffix.

`DDGI_CAPTURE` prints the completed snapshot prefix. Each prefix contains the
context RID and actual frame number, so two viewports do not overwrite each
other. An empty prefix disables all capture work. These readbacks synchronize
with the GPU and write large files; use a separate run for performance numbers.

The JSON metadata records cascade minimum cells, spacing, last scheduled updates
and camera dimensions. Binary files contain little-endian texture components,
with Y as array layers, Z as rows and X as columns. Irradiance and distance have
8-by-8 and 16-by-16 texel tiles per probe, respectively. Ray data has rays along
X and physical probe indices across rows and layers. The descriptor buffer is
the packed SDK `DDGIVolumeDescGPUPacked` layout, including the RT-relative origin.

Use the standard-library inspector to summarize a snapshot:

```text
python misc/scripts/inspect_ddgi_capture.py <snapshot.json> --positions <probes.csv>
```

The report includes validity, active classification, valid-probe age histograms,
position mismatches, nonfinite values and camera indirect radiance. The optional
CSV exports absolute probe cells and relocated world positions. SDK probe offsets
are normalized by spacing; the inspector converts them back to world units.
Irradiance atlas statistics remain SDK gamma-encoded values. Camera output is
linear, unexposed diffuse indirect radiance with surface material factors applied.

Validity and active classification are separate. A valid inactive probe does not
contribute to lighting. A newly relocated probe is invalid until its lighting and
position match. An ordinary scroll should retain the intersecting absolute cells
while entering cells lose their old validity. A nonoverlapping teleport should
clear every old irradiance slot, including cascades not selected for that frame's
lighting update. When examining age, distinguish invalid probes from valid probes
whose last update is older; an invalid probe has no usable lighting history.

These diagnostics supplement rendered captures. They do not establish temporal
image quality, RR correctness, export delivery or performance by themselves.
