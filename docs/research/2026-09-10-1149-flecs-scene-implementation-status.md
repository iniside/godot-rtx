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

Successful nonempty `.escn` load/save, prefab apply/revert, undo/redo and subset
round trips have not been executed. Their production authoring UI is step 6;
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

Renderer, editor, subsystems and
native import/export steps have not landed. Existing Node-world
operation is not evidence of the target entity model. The owner reiterated on
2026-09-10 that static-mesh components are required for renderer validation.
EntityMesh/EntityTransform already exist in the foundation; direct renderer
ingress in step 5 and the nine native renderer scenes in step 8 remain required
implementation and real-Vulkan validation, not optional follow-up work.

Scope remains the approved plan: no scripting model, Node plugin compatibility,
2D scenes, HTML/CSS game UI or automatic world streaming in this implementation.
