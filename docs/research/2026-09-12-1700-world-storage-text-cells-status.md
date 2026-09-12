# World storage: text cell scenes and streaming status

Verified 2026-09-12 UTC. Source: plan `be6d278d1b`, Krok 1 `ff0cc1162f` +
`2564d97e95`, Krok 2 `4e0fdb8419` + `a8b8c31ee9` + `1b6d99488e`, Krok 3
`ae910c308e`, Krok 4 `a06d8d1d65` + `dda2a58359` + `41772e02f3`; binary
`bin/godot.windows.editor.double.x86_64.exe` built from the working tree at
`dda2a58359` (owner's unrelated working changes present). Windows Vulkan
primary double editor, RTX 4090. No tests, no proof audit. Plan:
[2026-09-12-1139-world-storage-text-cells-plan.md](../plans/2026-09-12-1139-world-storage-text-cells-plan.md).

## Implementation

- Format (Krok 1): `<scene>.escn` is a `ConfigFile` text main file (uid on
  line 1, `document`, `revision`, `default_grid`, `default_range`,
  `[grids] name={size, range?}`, `[types]` schema manifest) plus a companion
  directory `<scene>/` with `.gdignore`, `global/<id>.escn`,
  `cells/<grid>/<x>_<y>_<z>/<id>.escn`, `<name>.cluster.escn` and
  `prefabs/<instance>.escn`. One entity per file, `VariantWriter` text
  (`{components, order, parent}`), friendly name in `EntityName`. Cell =
  `floor(origin / size)` of the serialized world pose; `EntityStreaming`
  (`2000000000000012`, field `1` = grid name) picks the grid, empty = default.
  No transform, environment, camera or directional light → `global/`.
  Unknown grid or malformed cell name is a hard error on load and save with
  the file path. Save is per-file atomic (temp + rename), moves by rename,
  writes only files whose text changed, deletes files of deleted entities
  (tombstone only for ids in a prefab `mapping`), prunes empty cell
  directories; the binary reader is gone.
- Residency (Krok 2): `CellKey{grid,x,y,z}`, cell maps built from section
  paths at load; `dirty` set from every command commit and prefab
  reconciliation (dirty entities are never unloaded; save scope = dirty plus
  moved descendants; a path change forces a complete save);
  `residency_serial`; `request_cells(cells, budget, &remaining)` in one
  `load_subset`, `release_cells` with refcounted ancestors and
  children-before-parents unload; `load_global()`; scratch `EntityWorld`
  reused by `_prepare`; one asset decode per reference; `unload_subset`
  drops records of file-backed entities.
- Scheduler (Krok 3): `EntitySceneStreaming::step` shared by
  `Node3DEditor` (start of `NOTIFICATION_INTERNAL_PROCESS`, cameras of all
  visible viewports) and `EntitySceneRuntime::process` (current
  `EntityCamera`): wanted = cells whose AABB is within `range` of any
  camera, nearest first, one `request_cells` per tick with a 256-entity
  budget (a cell larger than the budget still loads whole); release when
  farther than `range + size`. `EditorNode::load_scene` and
  `EntitySceneRuntime::setup` load `global/` only. Dock refreshes on
  `revision` or `residency_serial` and names non-resident entities from
  `Section::name`. Counters print under `--benchmark`.
- Editor filesystem (Krok 4): `EntitySceneIO::companion_directory` pairs the
  main file with its directory in the dock (move/rename with rollback,
  duplicate with a fresh uid, delete to trash), `EntityScene::relocate`
  keeps `storage_path` on the moved tree, `ACTION_FILE_ADD` skips the
  load+save `set_uid` for `.escn` that already carry a uid, session restore
  and recent scenes skip a scene whose directory is missing, Open/Save-As
  dialogs list `.escn`.
- Converter (Krok 5) needed no code change; the three demo scenes were
  regenerated with `--convert-energy-directional`, `--convert-renderer-main`
  and `--convert-microgeometry-stress` (headless).

## Test scene (Krok 6)

`demos/rtxdi_manual/streaming_test.escn` + `streaming_test/` (79 files,
310 KB) were authored as text by the agent from the converted
`energy_directional` records: grid `default` size 10, `default_range` 5,
25 cells (x,z ∈ −2..2, y = 0) with cube, sphere and plane per cell,
`global/` environment, sun and camera at (0, 1.5, 0). Ergonomics finding:
the loader requires every serialized field of every component, so a hand
written record must copy full component dictionaries (an `EntityGeometry`
block is 28 fields, a light 40) and the main file must carry the complete
`[types]` manifest; templates from a converted scene make this workable but
defaults-on-missing-fields would make authoring far lighter.

## Measurements

Windowed double editor, `--benchmark --verbose`, session restores three
scenes (stress, main, streaming_test), streaming_test current.

| Scene | Resource Load (catalog read) | `load_global` | Streaming after open |
| --- | --- | --- | --- |
| `streaming_test.escn` (78 entities, 25 cells) | 33 ms | 6.6 ms | 4 cells, 12 entities in one tick |
| `main.escn` (67 entities, 5 cells) | 25 ms | 7.3 ms | 5 cells, 63 entities |
| `microgeometry_stress/scene.escn` (10 004 entities, 9 cells of 64 m) | 3.19 s (10 005 files) | 7.8 ms | 9 cells over ~4.2 s |

Stress cells of 2250 entities cost ~0.92 s each per tick (`read_record`
645 ms = 0.29 ms per text file, `install` 250 ms), the first one 1.59 s
including the `.mgdata` asset loads; full residency 10 004 entities ~4.2 s
after open, versus 8.5 s warm document load before this work. Dock refresh
per streamed cell 6.5–8 ms at 10 004 entities. Zero `ERROR:` lines in all
runs; clean window close exits 0 with no scene file touched (`git status`
of every scene tree unchanged, main file mtimes unchanged).

Camera motion tool on `streaming_test` (start (0, 1.5, 0), no focus change):

| Segment | Counters |
| --- | --- |
| open | requested 4, resident 4 cells / 15 entities |
| +10 m X | requested 2, released 0 (hysteresis 15 m) → 6 cells |
| +10 m Z | requested 2, released 0 → 8 cells |
| +25 m X | requested 2, released 2 then 1 → 7 cells / 24 entities |
| −25 m X | requested 2, released 2 → 7 cells |

The camera at (0, 1.5, 0) sits on the shared corner of four cells, so four
are resident at open; with 10 m cells and a 5 m range a cell unloads only
once the camera is more than 15 m from its box, so a 10 m move adds cells
without releasing any.

Headless editor and headless runtime (`--headless --path . streaming_test.escn`)
report the same 4 cells / 12 entities with zero errors. Owner opened
`streaming_test.escn` in the windowed editor and confirmed the streaming
visually (2026-09-12).

## Limits and open items

- Not exercised in the windowed editor (needs UI input, which the harness
  cannot send without stealing focus): editing an entity then saving with
  partial residency, undo after the camera leaves the cell, dock
  move/duplicate/delete of a native scene, Save-As, rename with an open tab.
  These paths are review-verified only: Krok 1–3 passed hostile review
  (two rounds for Krok 1 and 2; the Krok 2 round-2 leftovers were fixed
  inline in `1b6d99488e`), Krok 4 used both rounds (`dda2a58359` fixed the
  rename → save failure, `41772e02f3` the case-only rename conflict found
  in round 2).
- Built: `target=editor precision=double` (validated) and
  `target=template_debug` single precision (links, not run); the single
  precision editor was not built.
- One editor run with `--quit-after 4000` exited with `0xC0000005` after a
  cold shader compile; the same command exited 0 on the rerun and every
  window close exited 0. Not reproduced, no backtrace (no PDB).
- A cell larger than the per-tick budget loads whole (stress: 2250 entities,
  ~0.9 s hitch); a smaller grid for dense content or a per-entity budget
  inside a cell is the fix.
- Catalog read is linear in files (0.3 ms per file); batch reads are the
  separate later task, as are runtime packing and the loader "Etap 1" fixes
  (asset triple load is now a single decode; `get_resource_type` per
  reference and the `fields()` rebuild remain).
- `.tmp` files left by a crash between create and rename inside a cell
  directory are invisible to the loader and never cleaned.
- Prefab instance files without a `cell` entry are accepted (uncelled until
  first save); no demo content has prefab instances yet.
