# Native stress scene editor load time

Verified 2026-09-11 UTC, source `9bff64d7b8` (measurements) and `f743a52341` (final counters), Windows Vulkan primary double
editor, `demos/rtxdi_manual/microgeometry_stress/scene.escn` (10004 records,
10000 mesh instances sharing two imported meshes), scene restored from the
editor session. Three `--benchmark --verbose` launches (600, 600 and 300 iterations) were
captured with wall-clock timestamps per stdout line. No optimization, tests
or proof audit was performed.

## Counters added

`9bff64d7b8` adds `Editor / Restore Session Scenes` and per-load
`Scene Load` benchmark keys (`<file> (Load N)`, unique per session after
`f743a52341`) in `editor/editor_node.cpp`, a one-line
`EntityScene load_subset` breakdown (`scene/resources/entity_scene.cpp`),
asset load counters in `entity_decode_asset`
(`scene/entity/entity_component_schema.cpp`), and gated timing lines in
`Node3DEditor::set_scene_document`, `RendererSceneCull::scene_publish_entities`
and `EntitySceneEditor::_document_changed`. All new output appears only with
`--benchmark`. Launch:

```powershell
bin/godot.windows.editor.double.x86_64.console.exe --editor --path demos/rtxdi_manual --rendering-driver vulkan --benchmark --verbose --quit-after 600
```

## Measurements

| Phase | Run 1 (first launch after rebuild) | Run 2 (warm caches) |
| --- | ---: | ---: |
| Process start to scene visible in editor | 126.4 s | 18.1 s |
| Startup: Servers (Rendering) | 21.6 s (19.4 s) | 2.6 s (0.28 s) |
| Startup: Display | 2.2 s | 2.3 s |
| Editor First Scan | 3.0 s | 0.27 s |
| Editor help regeneration (Run 2) | 4.8 s | none |
| Restore Session Scenes | 15.17 s | 9.93 s |
| Scene Load: Resource Load (manifest, 19.8 MB) | 0.34 s | 0.33 s |
| Scene Load: Load Subset | 14.72 s | 9.46 s |
| load_subset: prepare / read_record | 0.28 s | 0.27 s |
| load_subset: prepare / describe (includes asset load) | 11.56 s | 6.38 s |
| load_subset: asset load (12 real loads, 29994 cache hits) | 11.54 s | 6.42 s |
| load_subset: prepare / world write_component | 2.76 s | 2.69 s |
| load_subset: commit | 0.07 s | 0.07 s |
| set_scene_document transforms / publish / sync | 15 / 27 / 62 ms | 17 / 32 / 83 ms |
| scene_publish_entities apply / dirty (render thread) | 26 / 28 ms | 35 / 37 ms |
| EntitySceneEditor refresh_catalog | 5.5 ms | 6.1 ms |
| Streaming first zero pending pages after load | +4.6 s | +2.0 s |

Run 1 spent about 75 s between First Scan and session restore compiling 108
shader variants (20 `RtMaterialHitShaderRD`, 24 `SceneForwardClusteredShaderRD`
among them). `ShaderRD::setup` hashes `GODOT_VERSION_HASH` into the cache key
(`servers/rendering/renderer_rd/shader_rd.cpp:151-184`), so every new commit
invalidates `.godot/shader_cache`; run 2 had zero cache misses. The editor
help cache is regenerated on the same trigger.

## Findings

- With warm caches the document load is 9.5 s of the 18 s to a visible
  scene. Two thirds of it (6.4 s) is `ResourceLoader::load` of the two
  imported mesh scenes (812 MB and 293 MB `.scn` plus `.mgdata` manifests),
  triggered from `_describe`/`_validate_fields` on the first record that
  references each asset; all later records hit the resource cache.
- The remaining 2.7 s is `EntityWorld::write_component` for 10004 records
  (about 0.27 ms per record), a Variant dictionary to flecs decode.
- `_prepare` installs every record into a temporary world and `_commit`
  copies components again; the copy is cheap (70 ms), the decode is not.
- Render publication, transform update and the entity dock are all under
  100 ms and are not load-time problems.
- Microgeometry streaming reaches zero pending pages about 2 s after load,
  then keeps rebuilding cuts and pages while the camera is static
  (pending pages rise to 1763 at +20 s); that is a runtime issue outside
  this measurement.

## Why Load Subset is slow (temporary probes, 2026-09-11 UTC)

Uncommitted probe counters were added on top of `af99b2d5d3`, run twice
(cold and warm shader cache) and reverted. Warm run, 10004 records,
`load_subset` 8.20 s:

| Cost | Time | Cause |
| --- | ---: | --- |
| 12 real `ResourceLoader::load` calls | 5.17 s | each first-referenced asset is loaded three times: `lucy.res` 3 x 1.29 s, `thai_statuette.res` 3 x 0.45 s |
| `ResourceLoader::get_resource_type` in `_validate_fields` | 1.52 s | one call per asset reference per record; opens the `.res` file and reads its header only to fill `dependency["type"]` |
| `set_serialized` (`entity_decode_struct`) | 2.06 s | `EntityComponentTraits<T>::fields()` rebuilds the whole field schema vector, including `entity_make_field` defaults, for every component of every record |
| 29994 cache-hit loads | 0.11 s | cheap |
| `_read_record` (seek, read, decode) | 0.26 s | |
| `_component_changed`, `load_entity`, commit | 0.10 s | |

The triple load comes from the resource cache being weak (`Resource`
refcount) combined with three decodes per first reference:

1. `EntityFieldSchema::validate` (`scene/entity/entity_component_schema.h`,
   `entity_make_field`) decodes into a temporary `T value`; for `Ref<Mesh>`
   that is a full load whose result is dropped, freeing the mesh, its RID and
   its microgeometry metadata (`Microgeometry metadata admitted ... retired`
   grows by one asset per drop and `.mgdata` is reloaded each time).
2. `EntityScene::_validate_fields` (`scene/resources/entity_scene.cpp`) calls
   `entity_decode_asset` again for the dependency record and drops it.
3. `EntityWorld::write_component` -> `set_serialized` decodes a third time;
   this copy is kept by the component.

In the cold run the first `lucy.res` load took 4.92 s instead of 1.29 s
because material shader variants compiled at the same time.

## Limits

Two runs only; no variance estimate. Timing is wall clock on the main
thread except the render-thread publish line. Asset load time includes GPU
buffer submission queued by mesh creation but not its execution. The
per-load key fix in `f743a52341` was relaunched once: keys print as
`scene.escn (Load 1): ...`, no errors, load subset 14.05 s, and the launch was
cold again (130 shader cache misses, 138 s total) because the fix commit
changed the version hash.
