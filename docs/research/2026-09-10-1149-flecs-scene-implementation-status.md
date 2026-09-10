# Native Flecs scene implementation

Owner approved the [plan](../plans/2026-09-10-1023-flecs-native-scene-plan.md)
on 2026-09-10. Task-start revision: `3a6a695fc859ea8aa271adf8eaddb03349f304c5`.
Plan commit: `e4f73a8286`. Implementation remains incomplete.

## Step 1: retained renderer inputs

Committed at `05996de60b041f5c36a95a55972bdfdfdf41233d`.
The [manifest](../../demos/rtxdi_manual/entity_migration/manifest.json) records
nine authorized scenes, effective instance overrides, retained literal resource
properties, source asset identities and finite native control recipes.
The temporary extractor does not execute scripts or instantiate a Node world.

Fresh source review PASS inspected the exact commit and cumulative
`e4f73a8286..05996de60b`. Independent read-only artifact inspection confirmed
47/47 source hashes, 14 serialized scene/resource snapshots, six glTF metadata
snapshots, and all 10000 serialized stress transforms (5000 Lucy and 5000 Thai
sharing two population assets). The original dirty scenes were not modified.

This is source/data preservation evidence, not native import, editor, game,
rendering or performance proof. The extraction command is recorded in
[validation.json](../../demos/rtxdi_manual/entity_migration/validation.json);
there is no separately archived command-output receipt. Verification used
independent read-only comparison of actual outputs against source bytes/data.
No automated tests, builds or game execution were performed for this step.

Large source GLBs remain at their existing owner-untracked paths. Their bytes
are not duplicated in the migration commit; hashes and import configuration
are retained. They must remain available to the later native import/export.

## Step 2: native foundation accepted

Committed at `8b645f23a7ac57c1124664cf7d715aaba055e5dc`: plain C++ EntityWorld,
generational handles, durable identity catalog, typed component changes,
Clang-generated native schemas/Flecs Meta, and pinned Flecs import recipe.
No per-entity Object/Node/Resource wrapper was introduced.

Final pre-review ordinary editor, double editor and template_debug builds pass.
Commands use `platform=windows accesskit=no d3d12=no angle=no -j16`, with
`target=editor`, `target=editor precision=double`, and `target=template_debug`.
Local logs: `logs/entity-step2-editor-final02.log` (26.20s),
`logs/entity-step2-double-final.log` (31.98s),
`logs/entity-step2-template-final.log` (19.79s).
Bootstrap without generated outputs and regeneration after generator/config
changes passed. A semantic component-declaration edit was not separately
exercised. No automated tests or native-world execution are claimed.

Fresh source review rejected one asset-codec error: a `container::subresource`
address sent directly to ResourceLoader depends on an existing cache entry.
Correction `4e2db6d6ab11c8368545acafc5083bfb5d9b6062` loads the container and
retains ownership while resolving the requested subresource. Fresh final source
review PASS traced text, binary and imported resources through their existing
cache/ownership contracts. Final correction builds pass in all three axes:
`logs/entity-step2-asset-fix-editor.log` (30.03s),
`logs/entity-step2-asset-fix-double.log` (27.97s),
`logs/entity-step2-asset-fix-template.log` (26.12s).
Actual cold-load and native-world execution remain unverified.

## Step 3: native lifecycle accepted

Committed at `9588beb0d4c01d5a6da844c64edc29bffecac95c`, from `4e2db6d6ab`.
Native game MainLoop/window/viewport and EntityWorld services replace game
PackedScene/autoload/current-scene ingress. Catalog relationships map to Flecs
Parent; native transforms maintain separate simulation and presentation poses.
First real world construction exposed a nullable Flecs strdup callback error;
the corrected callback now allows native startup and shutdown.

Ordinary/double/template builds pass. Latest empty native Vulkan launch exits
0; nonempty scene startup exits 1 with the expected missing-native-loader error
and completes cleanup. ProjectManager UI launch exits 0. Logs are local under
`gpuprofile/ecs_step3_runtime/`. These runs do not prove normal editor startup,
nonempty native scenes, hierarchy behavior or performance. Renderer-field
StringName orphan diagnostics remain disclosed in the native logs.

Fresh source review REJECT identified three required corrections: active
editor root-World3D dereferences after that owner was removed; KEEP_WORLD
moving transformed descendants when the moved group has no transform; and
default-environment setting registration absent from fresh editor startup.
Correction commit `7e4513db52a8a174e9505917ec138530d159af30` gives the editor
its direct EntityWorld, replaces preview Nodes with renderer-owned UI data,
registers the retained setting during common initialization, and precomputes
KEEP_WORLD changes across transform-less groups before relationship mutation.
Required unloaded frontier records reject the operation before mutation; later
document commands load the required subset. Hierarchy correctness here is
source/math evidence, not executed native hierarchy proof.

Normal editor execution also exposed Camera3D registration and GridMap editor
startup dereferences. Matching binary/map stacks identified both; nullable-world
camera registration is corrected and excluded GridMap authoring no longer
automatically registers its plugin. No World3D fallback was introduced.

Final ordinary/editor, double/editor and template_debug builds pass, respectively
27.77s, 27.30s and 29.33s. Local evidence is in `gpuprofile/ecs_step3_fixes/`:
`editor-build6-map.log`, `double-build3.log`, `template-build.log`, source/binary
hash lists and the exact source patch. Ordinary diagnostics add `linkflags=/MAP`.
Actual normal `--editor --path rawcontent --rendering-driver vulkan --quit-after
180 --verbose` reaches the main window, completes existing Lucy/Thai imports,
and exits 0. `native-empty` exits 0; `partial-start` exits 1 with the expected
missing EntityScene loader error and completes cleanup. Their named receipts
and stdout/stderr are retained in the same local directory.

SPIR-V parser/OpDemote diagnostics, RTXDI magenta-material warnings and renderer
StringName orphan messages remain disclosed. Earlier native logs contain the
same shader diagnostic category, but do not establish identical materials or
visual correctness. No warning-free, nonempty native rendering, performance or
automated-test result is claimed. Fresh final source review PASS examined the
exact correction/original commits and cumulative `4e2db6d6ab..7e4513db52`,
confirming all three round-one findings closed. It checked all twelve archived
source hashes and actual build/run receipts; native hierarchy execution and
nonempty rendering remain unverified.

Until native editor/physics/import replacements, the following old UI routes
are explicitly unavailable: ruler, snap object to floor, curve collider snap,
3D viewport file drops (scene/mesh/audio/material/texture), and adding preview
sun/environment to the Node scene. Their native replacements remain assigned
to approved later steps; disabling old entry points does not complete them.
GridMap authoring remains permanently excluded by the approved plan.

## Step 4: native document accepted after renewed corrections

Implementation baseline: `2dd1bf401d93a724341355f9b55c5b9cc4f5e9da`.
Frozen task commit: `c1927f7324946c489b3f3d213b0cd491a96cff81`.
The 18-file change introduces EntityScene catalog/resident-world ownership,
addressable binary `.escn`, loader/saver and dependency metadata, native command
history and prefab provenance/overrides. Runtime accepts native `.escn` paths
and UID addresses; the old PackedScene ESCN importer is removed in this step.
Fresh round-one source review REJECT identified four concrete defects; the
implementation has not passed source review yet.

Final ordinary/editor, double/editor and template_debug builds pass in 24.84s,
43.36s and 30.28s. Local evidence under `gpuprofile/ecs_step4_document/` includes
`build_editor07.log`, `build_double01.log`, `build_template01.log`,
`staged-source.patch`, the owned path list and source/binary hash lists.
Actual normal Vulkan editor and native empty game exit 0. Missing `.escn` and
legacy `.tscn` input exit 1 with the intended startup diagnostics and cleanup.
Named `editor`, `native-empty`, `missing-scene` and `wrong-type` receipts and
stdout/stderr record those executions. Existing renderer diagnostics remain;
no warning-free or visual correctness claim is made.

Round-one findings in `c1927f7324`:

- Applying overrides from an older prefab instance stages an incomplete source
  catalog, then can remove a newer source element from another user instance.
- Lazy prefab catalog reconciliation can resurrect descendants of a locally
  deleted hierarchy, leaving a live child under a deleted parent.
- CREATE validates absence in a partial staging catalog rather than the
  authoritative document, permitting replacement of an existing ID/tombstone.
- Unloaded-save field validation skips the typed codec for asset/nested values,
  accepting the wrong Resource subclass or nested shape until materialization.

The fourth defect was independently identified by the author after committing
and confirmed by review. Correction `22158a5585a2476c8cd87fc2a5a0f171f2139e10` addresses all four
findings. Fresh final review confirmed those cases closed, but rejected two
additional concrete prefab defects described below. Complete source identity must be available without
materializing all component records; fixes preserve that partial-read contract.

The correction's real editor run also exposed malformed `EntityScene.xml`:
its self-closing tutorials element is incompatible with the current DocTools
parser's explicit closing-tag loop. The correction now includes the owning XML
and regenerated editor documentation; final editor execution exits 0 without
the malformed-document diagnostic.

Correction evidence is local under `gpuprofile/ecs_step4_fixes/`, including
`committed-source.patch`, four owned-source hashes, all 18 cumulative source
hashes, executable hashes and named build/run receipts. Final ordinary/editor,
double/editor and template checks pass in 40.76s, 37.43s and 15.09s; the fixed
C++ template build previously passed in 23.17s. Refreshed `editor02` Vulkan
RTX4090 execution exits 0. Native-empty/missing/legacy receipts exit 0/1/1 on
identical corrected C++ before the XML-only rebuild, with their earlier binary
hashes retained separately. Shader diagnostics and orphan StringNames remain.
This does not execute the four nonempty document correction scenarios.

Additional bounded source check (2026-09-10, `c1927f7324`): synchronous
`ResourceLoader::load` selects `LOAD_THREAD_FROM_CURRENT` outside worker-pool
tasks (`core/io/resource_loader.cpp:725`); EntityScene binds its owner lazily
(`scene/resources/entity_scene.cpp:30`), and metadata loading does not bind the
resident world. No additional ownership defect was established for current
native startup. This is clang-nav/source/XML/history evidence, not execution
of a successful native document load or a threaded streaming contract.

Final review REJECT at `22158a5585`, covering original `c1927f7324` and the
cumulative `2dd1bf401d9..22158a5585`, identified these two later-corrected defects:

1. **P1 — KEEP_LOCAL reparent mode is lost when applying prefab overrides.**
   Override creation at `entity_scene_commands.cpp:370` omits the selected mode;
   reconstruction at `:1092` uses KEEP_WORLD. A at root x=1 reparented under
   P at x=10 with KEEP_LOCAL should retain local1/world11, but apply produces
   local-9/world1. Preserve/replay the selected mode or consistently apply the
   authored parent/local-pose result.
2. **P2 — newly inherited unloaded records fail refresh/revert/apply.**
   Reopen a saved user after its source adds B: lazy reconciliation creates B's
   metadata without local bytes. `refresh_prefab` at `:966` and apply at `:1185`
   clear source provenance before preparation, so B reaches `_read_bytes` and
   fails ERR_FILE_CORRUPT. Prepare new inherited records through their source
   provenance while retaining stored snapshots where needed; do not require
   loading or saving the whole instance as a workaround.

Implementation stopped under the repository's maximum-two-review-round rule.
The owner explicitly renewed work with "kontynuuj" after receiving both defects.
The renewed correction task started at `cf4bad9a9bd10207f77a4210947463b983cadf46`.
Correction `2a7768e58ce1f6a360c90bdb6ac5539733634b56` preserves and validates
the selected reparent mode and retains provenance during stored/inherited
record preparation. Resident ECS values remain authoritative. Fresh source
review PASS covered the exact commit, renewed range and cumulative step 4,
confirming both scenarios closed and earlier corrections preserved.

Ordinary/editor, double/editor and template_debug builds pass in 40.94s,
41.49s and 32.84s. Fresh Vulkan RTX4090 editor and native-empty executions exit
0; source/executable hashes, build logs and receipts are in
`gpuprofile/ecs_step4_renewed_fixes/`. The named nonempty prefab scenarios still
have source reasoning rather than executed UI proof. Steps 1–4 are accepted
at their documented intermediate evidence boundary. The full migration remains
incomplete.

At step 4 acceptance, successful nonempty `.escn` load/save, prefab apply/revert,
undo/redo and subset round trips had not been executed. The finite converter
below now exercises native save/reload; prefab and UI workflows remain pending.
Their production authoring UI is step 6;
complete renderer fixtures need later component/import work. These remain
required final behavior validation through the real editor/native scenes.
No new fixture, test harness, script, extra CLI proof API or automated test was
introduced to stand in for that topology.

## Step 5: native renderer integration in progress

Task baseline: `2a7768e58ce1f6a360c90bdb6ac5539733634b56`. The full renderer
frontend/consumer replacement and render-component integration is delegated as
one responsibility. Implementation is uncommitted; no native-rendering result
is claimed yet.
Real Vulkan exercise of native geometry is required before this renderer step
can be considered complete; finite production-content dependencies must be
addressed explicitly rather than replacing them with empty-world startup proof.

The existing `energy_directional` fixture is reserved for the first native
Vulkan exercise after render schemas stabilize. Read-only comparison confirms
its scene, geometry.gltf/import, plane wrapper/mesh and current dirty project
configuration match the preserved `entity_migration/sources.json` hashes.
Resolved inputs and defaults remain in the existing preservation artifacts;
no new conversion has run. Retain all four glTF meshes, including three hidden
siblings and Deformer's Stretch morph target, both cameras, the directional
light and environment. Required shared identities are four meshes, one material
and one Environment; Plane already has `geometry_parts/plane_mesh.tres`.
Conversion must use standalone shared assets or verified stable asset addressing,
not retain old scene wrappers as the runtime world. File-local resource IDs are
not ResourceUIDs. Existing project main_scene remains unchanged until migration.

Renderer tracing confirms additional active consumers in geometry implementations,
backend override parity, editor previews/interface/gizmos and retained scene
parser types. Those consumers belong to the same replacement step. Native
double translation must reach the existing high/low GPU motion path even in
ordinary builds; the old REAL_T_IS_DOUBLE condition cannot be used as a proxy
for EntityPosition precision. No native renderer execution is established yet.

Finite-conversion integration research (2026-09-10, document APIs unchanged
since `2a7768e58c`) selects a tools-only editor command after the first
EditorFileSystem import scan. An earlier Main startup hook would depend on
existing glTF import artifacts. The existing `wait_for_import` lifecycle can
keep the command alive until assets are available, with game scene opening
disabled. Root `editor/SCsub` already includes editor C++ sources. Read shared
meshes through PackedScene/SceneState without instantiating Nodes, save/reload
standalone resources and verify ResourceUIDs, then use one CREATE plus typed
ADD_COMPONENT command batch and EntitySceneIO::save. Relevant source anchors:
`main/main.cpp:1708,4410,4602,5044`, `editor/editor_node.cpp:915,8529`,
`scene/entity/entity_scene_commands.cpp:229,434`, and
`scene/entity/entity_scene_io.cpp:212`. This is clangd/source/history research,
not an implemented converter or successful native scene round trip. The
converter writer will run after the renderer source freezes, with serialized
source ownership and builds.

Owner sequencing correction (2026-09-10): prioritize a visible native
energy_directional scene before expanding the remaining renderer refactor.
After the current coherent ordinary build, temporarily freeze source and pass
the build/source slot to the finite converter. Then execute mesh, material,
camera and light on Vulkan and fix any actual rendering blockers first. The
scene near the origin does not depend on completing every 100 km/nonmesh/tool
path. Those obligations remain in step 5 after this first-image gate; the
temporary checkpoint is not source completion or final renderer acceptance.

First-image checkpoint: ordinary editor build passes in 30.56s at
`111c6a19b57b479cb1dc4325d25af744e6550156` plus the uncommitted renderer changes.
`gpuprofile/ecs_step5_renderer/converter_checkpoint_manifest.json` records
138 owned source paths/hashes and binary/log hashes; the paired build log is
`converter_checkpoint_build.log`. Engine executable SHA256 is
`927a1830f4f2b8b2eb4e2bbb9f1c612a9f850fea23bf01e76033a03b33d0c161`.
The renderer writer has stopped and released source/build ownership to the
bounded finite-conversion context. Double/template and nonempty Vulkan
validation are still pending. This is compile evidence for an intermediate
dirty source snapshot, not a reviewed or completed renderer implementation.

Finite conversion now succeeds: `editor/entity_scene_energy_converter.*` and
its tools-only Main flag save `demos/rtxdi_manual/energy_directional.escn` with
six standalone UID-backed assets. Native readback compares every encoded
component and confirms eight records, four meshes, one visible mesh, Stretch,
two cameras with only primary current, a directional light and environment.
Ordinary build03 passes in 31.05s; convert03 exits 0. Evidence is under
`gpuprofile/ecs_energy_converter/`, including `manifest.json`, `owned_diff.patch`
and `convert03.log`. All 138 frozen renderer source hashes remain unchanged.
The finite flag uses existing recovery startup to suppress saved-scene reopen;
the earlier crash before conversion is not root-caused by the passing retry.

**Initial native image gate FAILED: black viewport.** The owner observed black
in the normal Vulkan window; root confirmed an entirely black 1280x720 frame
from the existing MovieWriter viewport readback. `capture02` renders 120 frames
and exits 0 in 14.03s with executable SHA256
`ec9f4ce3b7d326ce9dec76b64a0f1dd5f9bde745b131ac647d0b4c82c239635f`.
Command, logs and receipt are under `gpuprofile/ecs_step5_first_image/`;
the inspected image is `capture02/energy00000119.png`. MovieWriter forces
readback synchronization, so this is image evidence, not performance proof.
The earlier `game01` was deliberately stopped to restart with capture and
does not establish clean exit. Windows Computer Use capture was unavailable
after its native pipe failed retries/reset; no desktop screenshot claim is made.
The renderer writer has regained source/build/runtime ownership solely to
diagnose and repair this black image before extending the remaining refactor.

The black-image investigation identified transposed converter transforms:
the preserved values follow VariantParser's row-major Basis constructor, but
the converter used set_column. Correcting that constructor and reconverting
restores primary camera position `(0,6,0)` and forward `(0,-1,0)`, confirmed
by saved-component readback. Matched Vulkan `capture03` exits 0 and its inspected
`energy00000119.png` now contains the plane on a black background. The plane
is magenta, so that capture still fails material rendering.
This establishes native geometry/camera visibility only. Material publication
and classification are the next bounded investigation; broader refactoring
remains stopped.

Matched `capture04` now displays a gray plane on black: root inspected
`gpuprofile/ecs_step5_first_image/capture04/energy00000119.png` after the
author's 120-frame Vulkan run exited 0. Native publication had unconditionally
called the procedural-bounds setter after disabling procedural geometry;
that setter recreated procedural state and caused UNSUPPORTED/DEFORMED flags
on the ordinary mesh. Guarding the bounds call fixes the magenta result
without changing shader classification or renderer settings. Ordinary build
passes in 30.100s. This is the first visible native geometry/camera/material
image, not proof of light-energy response, 100 km precision, other render
components or full step 5 completion. The two first-image fixes and converter
remain uncommitted along with the larger renderer change.

Existing RTXDI GPU diagnostics confirm executed directional lighting in
`capture05` on the same source, executable and scene: frame 60 contains one
infinite light and no local/environment lights. Of 1024 sampled pixels, 624
have surfaces; initial proposals and final shading record directional samples,
positive target PDFs, visible shadow results and valid reservoirs for those
624 surfaces, with 624 final BRDF evaluations. The run exits 0. Its requested
limit is `--quit-after 120`, but hashed MovieWriter stdout and PNG numbering
show 130 captured frames. The receipt's frame count is corrected to 130;
`capture05_receipt_original.json` preserves the original erroneous 120 entry.
`capture05_receipt.json` and `capture05/directional-frame-60.json` contain
the source/data hash checks and raw counts. These are sampled executed events,
not whole-frame extrapolations, GPU timing or light-energy-change response.

Fresh read-only proof audit PASS confirms the bounded conversion, actual
native Vulkan image, the two failing-branch corrections and sampled directional
lighting. It verified 22 capture04 manifest artifacts, all six serialized UIDs
and unchanged asset hashes, original preserved scene/GLTF/wrapper inputs, and
the corrected capture05 receipt. This is an intermediate proof acceptance;
the unfinished source task still requires its full commit and hostile review.

Owner clarification after seeing the plane: a gray rectangle is insufficient
as the practical mesh-rendering checkpoint. The preceding proof remains valid
only for its limited geometry/camera/material/light path. Broader refactoring
was stopped again until the already-preserved renderer `main` scene was converted
and shows recognizable 3D meshes, materials and shadows on the native path.
This advances the next scene from the approved finite allowlist, not a new
test harness or a claim that the entire gallery is already migrated.

The main gallery checkpoint now renders recognizable shaded meshes, checker
panels, a deformed mesh and cast shadows; root inspected capture02 and the
owner confirmed the image. Native conversion saves 67 records, 60 mesh entities
(15 visible), four lights, two cameras and one environment. Ordinary build
passes (33.83s), conversion exits 0 and fresh Vulkan capture exits 0 with 120
frames. Initial converter shutdown failure and the resulting missing persisted
Environment UID were corrected using existing ResourceUID cache persistence
and normal SceneTree quit. Evidence: `gpuprofile/ecs_main_converter/manifest.json`
and `gpuprofile/ecs_main_first_image/capture02/main00000119.png`, at HEAD
`fd59c0dcbc` plus the recorded uncommitted source; console-launcher SHA256
`aa66dd0691d2ef72e8f8ec9d9ba625f0e2e17216e8dc4ec981cfa5328dd4ab59`.
Fresh bounded main proof audit PASS independently verifies 150 artifact hashes,
138 checkpoint source entries, preserved population/input hashes, typed readback,
the actual image and both execution logs. Full Step 5 source review remains open.

The owner observed roughly 1 FPS during capture. A separate ordinary Vulkan
run of the same binary and scene, without MovieWriter, with `--disable-vsync
--print-fps --quit-after 3000`, exited 0 and reported 198–219 FPS across 13
windows (`gpuprofile/ecs_main_first_image/normal01.log`). MovieWriter reported
67 seconds for capture and 116.72ms/frame encoding alone; that elapsed duration
is not ordinary runtime FPS. This verifies only the static gallery at 1280x720
on RTX 4090, not dense-world performance, animation, or an old/new comparison.
The owner authorized resuming the remaining implementation after this result.

Resumed shadow precision checkpoint at `a49e5c7b96` plus dirty source builds
ordinary editor (58.60s, final 31.27s) and renders the unchanged main gallery
for 120 Vulkan frames with exit 0. Root inspected the new image: recognizable
geometry/materials/shadows remain visible. Evidence is under
`gpuprofile/ecs_step5_renderer/shadow_capture01/`; console-launcher SHA256
`a9d9fff0624b86503b6c13b7627e1f2c1f73017f7a6f197acbba3461cafc4ba2`.
This is an origin-scene check only, not far-coordinate or isolated raster-shadow
proof. Fog/GI, particles/colliders and remaining tool consumers are still open.

Fog source closure builds ordinary editor (58.01s); subsequent VoxelGI/lightmap
closure builds after correcting the Dummy override (37.23s). A finite scratch
copy of main translates all 67 parentless entities by `(100000,100000,100000)`;
201 same-width position payloads change, with original scene/assets and the
copy's header/manifest otherwise unchanged. Data and provenance are under
`gpuprofile/ecs_step5_renderer/far_main/`. Matched ordinary Vulkan runs of
origin and translated main each exit 0 with 120 captured frames using the same
console launcher SHA256 `62cacf8168d2cc87cf95a79b1e541146e211d0dfc00aacbdd9e5d26a628e6040`.
Root inspected both: gallery geometry/layout/materials/shadows remain visible;
images are not pixel-identical. `gi_origin_far_manifest.json` records source
and artifact identities. Fresh bounded far proof audit PASS independently
decodes all translated positions, compares 76 unchanged dependencies and their
actual loads, verifies all 11 frozen artifact hashes, inspects both images and
verifies the engine/launcher companion metadata. This supports
static translated-gallery visibility only, not isolated fog/VoxelGI/raster-shadow,
motion, dense-world performance or the double build axis. Particle simulation,
collider publication and remaining tool consumers are now the active work.

The far proof audit exposed an executable-identity metadata error: Windows
`console_wrapper_windows.cpp` launches the sibling GUI executable; the console
hash does not identify the rendering engine. The actual GUI engine was retained
after both captures, before another build, with SHA256
`d75e3c196dac1fe1404294ef9d93f1576a466890a09cbbcff55cc37ba3b3c03c`.
This is post-capture collection with recorded no-build continuity, not a
contemporaneous engine hash. Earlier console-only receipts likewise cannot
cryptographically pin their historical engine binary. Exact historical source
reconstruction is also unavailable for this checkpoint: source hashes were
retained, but no complete source snapshot before subsequent particle edits.
The saved scene data, actual Vulkan images and capture logs remain valid
bounded observations. Future checkpoints retain both engine and launcher plus
the exact owned source snapshot before further edits.

Combined particle/tool checkpoint ordinary editor build passes (97.61s,
`gpuprofile/ecs_step5_renderer/particles_tools_build06.log`). It includes native
particle collision/attractor publication, heightfield origin handling, relative
simulation and current/previous draw origins, generated ParticleProcessMaterial
provenance with retained public custom-shader semantics, and static import/tool
preview closure. Retired polygon/skeleton authoring and Node drag/drop consumers
are removed; Animation assets remain, with playback explicitly unavailable until
native animation. Ruler helper data is native, while placement remains Step 6.
This checkpoint has compile evidence only. The retained nine scenes contain no
particle simulation population; PPM spawn/motion/collider/sub-emitter behavior
requires later native-authoring execution before final migration closure.
An audit-found mask-change pairing invalidation correction is being applied
before the next frozen build/runtime checkpoint. Pose-only updates must retain
their incremental path. Double/template and complete Step 5 review remain open.

The following ordinary build07 passes (31.82s), and the native main Vulkan
capture again exits 0 with 120 frames; root inspected recognizable meshes,
materials and shadows. `particles_tools_checkpoint/` retains 168 owned source
snapshots, deletion/diff records, 24 scene/asset files and matching-basename
engine/launcher copies. Normal editor startup instead crashes while restoring
the old `microgeometry_stress/scene.tscn`. A `/MAP` relink has byte-identical
`.text` to the captured crash engine, mapping the stack to
World3D::get_scenario ← WorldEnvironment::_notification ← cached editor scene
attachment. Old Node-world environment mutation dereferences the removed world.
Evidence: `gpuprofile/ecs_step5_renderer/editor_crash_symbols/decoded_stack.json`.

The owner explicitly requires old `.tscn` worlds not to load, including session
restore. The bounded correction rejects them at editor load authority before
instantiation and retires WorldEnvironment world mutation while retaining
parsed shared assets. Normal cached startup must be rerun; recovery mode alone
does not close this failure. The owner also prioritizes converting the retained
10000-instance microgeometry stress scene to native `.escn`. Preparation runs
read-only until the crash correction releases the source/build slot. No new
Node-world compatibility route is authorized.

The crash correction now builds (37.82s) and normal Vulkan editor startup with
the same cached stress `.tscn` exits 0 (19.92s), explicitly rejecting that path
before instantiation. `editor_crash_fix_editor01/stderr.log:523` records the
rejection; retained `editor_layout_before.cfg` establishes the cached input.
The editor subsequently saves its empty open-scene list normally. Exact engine,
launcher, symbol map and 172 owned source snapshots are retained under
`gpuprofile/ecs_step5_renderer/editor_crash_fix_checkpoint/`. This closes the
observed old-world startup crash, not native editor authoring.

Source/build/runtime ownership is now with the finite stress converter. Its
verified recipe has 10004 records: 5000 Lucy, 5000 Thai, one floor, sun, camera
and environment. All seven preserved input hashes match; exact JSONL transforms
are reused. Existing MicroGeometry saver must preserve content/pages in two
standalone `.mgdata` files, referenced by shared saved meshes, rather than
retaining imported-cache paths. Renderer implementation is paused until this
conversion and first native stress execution hand back the slot. Remaining
renderer source work includes particle cycle/reference/history fixes, native
occluder coordinate publication and final precision/ownership review.

Stress conversion run03 exits 0 after 78.07s, validating all records/components,
order and 5000+5000 sharing in the decoded native world. The first standalone
Vulkan stress image fails visibly (sky only). Source evidence identifies a
preservation discrepancy: literal Transform3D bases use rows, while the
preservation script's Euler-derived bases use columns. Stress Camera/Sun use
Euler properties, so the converter must derive their bases from authored
SceneState values using actual Node3D Euler semantics. Exact literal mesh
bases/placements remain unchanged; Lucy's literal source basis also contradicts
the old recipe prose sign. Failed image/input are preserved under
`gpuprofile/ecs_stress_converter/`; corrected conversion/readback is in progress.

The owner now explicitly requires rendering verification in the actual editor,
not standalone. No further standalone stress image/FPS launches are authorized.
Converter data correction may finish, then native `.escn` document opening and
display in the real editor 3D viewport takes priority over remaining renderer
expansion. This advances the entry/display portion of approved Step 6; it does
not substitute a separate viewer or claim complete native Inspector/outliner/
undo authoring. Old `.tscn` world rejection remains mandatory.

Corrected stress data conversion exits 0 (build04 29.61s, conversion wall69.29s),
with scene SHA256 `7db8fe4b2d5b5f636506dc2a2f107f1fe80f97f4276c57f90b8eebf942998b71`.
All original source hashes remain unchanged. Eight standalone asset files total
4.159GB. `gpuprofile/ecs_stress_converter/manifest.json` preserves commands,
failed earlier captures, exact owned source/diff and asset identity evidence.

Native editor entry/display now builds (88.75s, followup35.58s) and the first
actual editor capture exits 0 with 120 frames. Root inspected
`gpuprofile/ecs_native_editor/capture01/editor00000015.png`: real menus/docks,
3D viewport and populated stress grid are visible. The tab-owned EntityScene
replaces Node3DEditor's unrelated empty world; camera seeding and native
light/environment presence preserve the authored view. Full native editing
remains unavailable. The stale selected-tab highlight was corrected. In the
ordinary editor without MovieWriter/fixed timestep the owner confirmed RMB+WASD
navigation. Magenta grid rendering remains recorded.

Native Ctrl+S initially skipped editor-state serialization because the legacy
save branch expected a Node root. Build05 (34.25s) routes native Save, Save All
and clean exit through existing editor-state serialization. The owner then
saved a view, closed normally and confirmed its restoration after reopening.
The native editstate remained byte-identical across save/close/reopen:
SHA256 `0244ee5812e2c2f05c9ed75f03ec24c3fbd01ec46b813212d5772722b8c4150b`.
The scene asset remained unchanged. Evidence lives in
`gpuprofile/ecs_native_editor/final_build05/` and `owner_saved_camera/`.
This confirms editor camera persistence, not native entity editing or its save.

Bounded baseline artifact audit found historical stress logs, but no retained
replayable exact pre-entity executable in their named artifact locations.
`%TEMP%/godot-render-repair-20260909/micro-step4-history-final.log` and its
receipt cover the older runtime-populated scene, while `editable-stress-game`
log/receipt cover the retained serialized topology for only 300 frames and
explicitly do not establish a performance comparison. Their executable paths
have since been overwritten. Historical numbers therefore remain references,
not a controlled A/B gate; final native measurements must state this limit.
This read-only audit does not block the first native image or authorize
recreating the old Node world. No benchmark was run during this audit.

Intermediate tool closure will also remove Node-based polygon/skeleton authoring
and import-dialog animation playback. Shared mesh/material/static import
previews and parsed Animation assets remain required. Retired playback controls
must report unavailability rather than appear to play a frozen preview. Native
animation playback/authoring returns with steps 7c and 8; step 5 must not add a
second preview-only evaluator or retain the old Node tick as a bridge. This is
an implementation sequencing boundary, not a final animation scope exclusion.

## Remaining work

Native renderer/editor prerequisite source is checkpointed at `b839a4a17a`,
explicitly unfinished. It records the preserved pre-usability source, excluding
owner configuration/assets/docs and generated files. Renderer completion and
its final proof/review remain deferred; this checkpoint is not acceptance.

The owner-prioritized usability slice is implemented at `72506855cd`: a
256-row paged/filterable native list, one selected EntityId shared with the
typed component Inspector, and viewport mesh ray picking. Native transaction
payloads enter EditorUndoRedoManager history without advancing a second native
cursor. Native scene save and dirty-close checks are connected. Ordinary
build04 passes (37.67s). Build03 crashed after the owner clicked an entity:
selection rebuilt Tree while it blocked mouse-event mutation, then dereferenced
a null item. Build04 defers selection with a document guard, resolves edit
history from the owning document, and preserves the selected page. Corrected
selection is now owner-confirmed without a crash. The owner requests clearer
Inspector grouping by component; collapsible component sections are the next
UI refinement. Full edit/undo/save behavior and first-click shared-mesh BVH
cost remain unverified. Fresh exact/cumulative review round 1 rejects three
concrete issues: text edits not committed on selection/save, wrong default type
for numeric array additions, and ray picking clipping incorrectly against
camera near/far planes. Correction `e4df780cf5` adds the requested collapsible
component groups, commits pending text to its original document/entity, uses
typed array defaults and picks against the actual near/far-plane segment.
Ordinary usability build05 passes in 96.87s. The owner accepts the grouped
Inspector as v1 ("inspector dziala jak na v1, to jest dobrze"). Fresh round 2
rejects one remaining P2: typing a number into SpinBox and pressing Ctrl+S
without Enter/focus loss saves the old value. The native flush sees only
value_changed callbacks, while SpinBox has not evaluated its active text.
Recommended correction: synchronously call the existing SpinBox::apply() on
active numeric edits for their original document/entity before flushing and
saving. No source correction was made after the second-round verdict; the
two-round limit applies. The practical v1 acceptance is not a final source
review pass or proof of every edit/undo/save branch. Evidence:
`gpuprofile/ecs_native_editor/usability_build04/` and preserved build03 logs.
Camera state and native scene hashes remain unchanged at launch. This is not
completion of full Step 6 gizmos, creation/deletion or prefab authoring.

The owner's normal close of build04 exposed a separate editor teardown crash;
process disappearance was not an exit-0 result. Root relinked the untouched
build04 libraries with the exact dry-run linker arguments plus a map file,
without recompiling or replacing the live binary. Archived and mapped `.text`
match at RVA 4096, raw size 99403776, SHA256
`2c42d0009c3069d3da0bb254778c7913d36469e9ca582ea889ed9932bb62e2d0`.
The stack resolves to Timer::stop, SceneImportSettingsDialog::_cleanup,
its destructor, then Node predelete and SceneTree::finalize. Evidence is in
`gpuprofile/ecs_native_editor/usability_build05/close_symbols/`.
Correction `e4df780cf5` moves child-dependent cleanup into the dialog's
PREDELETE notification, before Node deletes its children. The real Vulkan
editor run with `--quit-after 120` exits 0 and exercises SceneTree finalization
without the C++ crash. `usability_build05/close_receipt.json` confirms identical
camera and scene bytes. Remaining draw-list/swap-chain and admission errors
are recorded, not a claim of clean renderer shutdown. Ordinary interactive
reopen is responsive (GUI 82004), and the owner accepts the grouped Inspector.
Full edit/undo/save interaction remains unverified, with the numeric-save bug
above explicitly open. The microgeometry admission limit remains a
separate deferred problem.

Owner sequencing decision 2026-09-10, working source over `20d7f5cbbacd`:
advance the coherent Step 6 usability slice before completing Step 5. Deliver
an entity list, an editable Inspector for the selected native entity, and
viewport mesh click selection sharing the same native selection. Preserve the
saved camera. Remaining renderer completion is deferred behind this slice;
the microgeometry admission limit is a separate deferred problem. No new
scripting, UI framework or streaming scope is added.

The Step 6 entry research at `a49e5c7b96` supplied the current document/tab,
Inspector and undo boundaries. Native tabs own EntityScene, and native save
uses ResourceSaver without the legacy PackedScene packing flags. Component
edits use schema addresses and native transaction payloads under the existing
EditorUndoRedoManager chronology. Native restore failure is checked before
advancing history. Shared Resource inspection remains the existing editor path.
These source integrations still need the bounded runtime verification above;
they do not establish complete prefab, multi-edit, component add/remove, gizmo
or create/delete workflows.

Renderer completion, the remainder of Step 6, scene subsystems and native
import/reimport/export remain open. The finite energy, main and stress native
scenes provide the current renderer inputs; completing the retained nine-scene
set and its editor Vulkan verification remains required later work. Existing
Node-world operation is not evidence of the target entity model.

Scope remains the approved plan: no scripting model, Node plugin compatibility,
2D scenes, HTML/CSS game UI or automatic world streaming in this implementation.
