# Primary visibility and raster optimization status

Owner-approved plan: [primary visibility](../plans/2026-09-11-0756-megageometry-primary-visibility-plan.md).
On 2026-09-11 the owner defers path tracing and primary-RT/hybrid experiments,
prioritizing 8 ms whole-GPU time on the existing stress scene if feasible.
Step 2 raster primitive culling is active; no 8 ms result is claimed.

Step 1 is committed at `68a166e961`, allocation-failure cleanup at `bb95c49338`.
Canonical six surfaces/history now own raster and existing PT output, with
lazy R32F trace depth, shared primary reconstruction and one final history copy.
HZB reads live depth before/after recovery. The debug-only internal
`GODOT_PRIMARY_VISIBILITY` selector currently accepts only unset/`R` as working;
other candidates are not implemented. New counters for future producers remain
with their implementation steps rather than reporting fabricated values.

Both matching ordinary Windows editor builds pass with
`scons platform=windows target=editor accesskit=no d3d12=no -j16`.
Final brief source review passes exact `bb95c49338` and cumulative
`3724291fba..bb95c49338`. The cleanup removes a failed allocation's named entry
so later frames can retry without clearing live history. Failure injection was
not performed; no automated tests or proof audit was run.

Native Vulkan `primary-step1-r` ran 900 frames, exit 0, no ERROR lines, with
visible dense geometry. HZB retained 140481 rejected clusters and zero recovery
emission at 2689x1602. This was a correctness capture with original editor
sleep settings, not a matched speedup comparison. Full-PT capture
`primary-step1-pt` exited 0, but visual inspection was interrupted by the owner
and full-PT validation is explicitly deferred. Its temporary environment-mode
edit is removed; original environment SHA256 is
`BED94F4E1AD8ECDCDD6F48C0B9811B5F64C4DE79B8FA2C97A2D85C64B4BDCCD5`.

Logs are in `C:/Users/lukas/AppData/Local/Temp/godot-render-repair-20260909/`.
The Step 2 before binary `bin/godot.primitive_cull_before.exe` has SHA256
`A09E9B2525B889E7D447A48A251F3EE851366B6D004FBB789F8A0FBBFF41E3D3`.
Current comparison uses matching camera, quality, lighting and viewport across
before/after; actual viewport size is recorded rather than inferred from CLI.
