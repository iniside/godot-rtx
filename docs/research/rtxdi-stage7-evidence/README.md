# Stage 7 evidence index

The [rendering status](../2026-09-07-0848-rtxdi-stage7-rendering-status.md) records the source baseline, diagnoses and validation boundary. This folder contains authored screenshots and compact evidence, not an executable test suite.

| Artifact | Meaning |
|---|---|
| `mixed-energy-failure.png` | Rejected early mixed-light output, before reservoir combination correction. |
| `gallery-inspected.png` | Intermediate LOD-corrected composition; it predates motion-aging closure. |
| `motion-failure.png`, `motion-corrected.png` | Same stationary FSR2 presentation before/after restoring object-transform aging, with private readback enabled. |
| `raw_diffuse-inspection.png`, `nrd_diffuse-inspection.png` | Same frame 120 demodulated diffuse before/after RELAX. |
| `raw_specular-inspection.png`, `nrd_specular-inspection.png` | Same frame 120 demodulated specular before/after RELAX. |
| `offscreen-caster-hidden.png`, `offscreen-caster-visible.png` | Ordinary editor binary; paused geometry; the box outside the camera produces the visible wall shadow. |
| `shadows-final-on.png`, `shadows-final-off.png` | Ordinary editor binary; merged shadow wrapper before/after pressing B. |
| `shadows-merged.png`, `shadows-expanded.png` | Earlier diagnostic comparison of the two MultiMesh representations. |
| `final-moved-camera-light-removal.png` | Ordinary editor binary after camera movement/cut back, light removal and deformer/source destruction. |
| `final-cut-two-views.png` | Ordinary editor binary after cut, resizing, fog/upscale, light changes and deformer deletion, with two independent cameras. |
| `directional-energy.png`, `emission-energy.png` | Isolated diagnostic energy arrangements; exact raw/HDR values are in the status document. |
| `nrd-spirv-validation.txt` | All 15 ordinary optimized NRD modules validated by Vulkan SDK `spirv-val --target-env vulkan1.3`; file names are shader-name MD5s, contents have SHA-256 hashes. |
| `private-readback-provenance.patch` | Source delta from final production to the corrected-motion diagnostic binary: private inspection instrumentation and the later anisotropy classification fix reversed. It is evidence only and is not applied in the final build. |
| `final-gallery.png`, `final-editor-fsr2.png` | Instrumentation-free static native template and FSR2 editor compositions. |
| `final-dual-fog-fsr2.png` | Instrumentation-free template, separate rendering thread, animated scene, fog and two independent viewports. |
| `final-shadows-expanded.png` | Instrumentation-free template expanded MultiMesh; matches the merged on/off case topology. |
| `final-energy-directional.png`, `final-energy-emission.png` | Instrumentation-free template energy scenes. |
| `final-unsupported.png` | Instrumentation-free template after the anisotropy classification correction; all three unsupported cubes become diagnostics. |
| `manifest.json`, `builds.log`, `pass-timings.json`, `final-*.log` | Exact source/binary hashes, commands, recorded build results, named GPU timings and runtime outcomes. |

Shadow columns from left to right: cull back, cull front, cull disabled, explicit double-sided shadow casting, mirrored MeshInstance, negative-local-scale MultiMesh. Top row faces up; bottom row faces down. Default back culling casts on the upper row; front culling on the lower row; disabled and explicit double-sided on both. Owner mirroring preserves MeshInstance's facing convention. MultiMesh-local mirroring reverses winding in both expanded and merged modes. The invisible lower explicit-double-sided plane still casts a shadow. The lower MultiMesh face is dark because its geometric orientation faces away from the light.

Raw visualizations are RGBA16F texture readbacks decoded with NumPy, using RGB and `pow(max(x,0)/(1+max(x,0)),1/2.2)` followed by 8-bit conversion. No spatial filtering or reconstruction was applied. Their black background/emissive regions are expected because those images show direct-light channels, not composed HDR.

Diagnostic binary SHA-256 for the corrected motion readbacks: `1c872a1780213484a57fb7af7c0031355ac69f67f4a927501b84728a748bae62`. It contains the removed private patch. Bulky source snapshots, raw `.bin`, SPIR-V, full build/runtime logs and the `/MAP` crash diagnostic remain only under the session's Windows `%TEMP%` directory. They are local-retention evidence and may be cleared by the OS; durable hashes, selected images and compact logs establish what was inspected. No build binary, imported cache or owner game asset is committed here.

Final command shape from `G:/Projects/godot-rtx`:

```powershell
bin/godot.windows.template_debug.x86_64.console.exe --path demos/rtxdi_manual --gpu-profile --max-fps 60 -- --still --delay=6 --capture=G:/Projects/godot-rtx/docs/research/rtxdi-stage7-evidence/final-gallery.png
```

The dual run adds `--render-thread separate` before `--` and replaces `--still` with `--dual --fog --upscale`. The expanded, directional, emission and unsupported cases place their `res://<case>.tscn` after the project path and use a four-second capture delay. The final editor run uses the editor console executable, `--render-thread separate`, `--still --upscale` and a five-second delay. Each `final-template-*.log` and `final-editor-fsr2.log` records the actual process exit code. Manual interactive logs instead record explicit Escape shutdown, not a recovered process exit code. The template used `disable_path_overrides=no`; the ordinary default template also compiled successfully but intentionally refuses these development project-path commands.
