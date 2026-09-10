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

## Step 4: native document committed, first review pending

Implementation baseline: `2dd1bf401d93a724341355f9b55c5b9cc4f5e9da`.
Frozen task commit: `c1927f7324946c489b3f3d213b0cd491a96cff81`.
The 18-file change introduces EntityScene catalog/resident-world ownership,
addressable binary `.escn`, loader/saver and dependency metadata, native command
history and prefab provenance/overrides. Runtime accepts native `.escn` paths
and UID addresses; the old PackedScene ESCN importer is removed in this step.
The implementation has not passed source review yet.

Final ordinary/editor, double/editor and template_debug builds pass in 24.84s,
43.36s and 30.28s. Local evidence under `gpuprofile/ecs_step4_document/` includes
`build_editor07.log`, `build_double01.log`, `build_template01.log`,
`staged-source.patch`, the owned path list and source/binary hash lists.
Actual normal Vulkan editor and native empty game exit 0. Missing `.escn` and
legacy `.tscn` input exit 1 with the intended startup diagnostics and cleanup.
Named `editor`, `native-empty`, `missing-scene` and `wrong-type` receipts and
stdout/stderr record those executions. Existing renderer diagnostics remain;
no warning-free or visual correctness claim is made.

After the frozen commit, the author identified a missing typed field-validation
boundary in `EntityScene::_validate_fields`: unloaded-save validation can accept
a valid Resource UID of the wrong subclass, or an incorrect nested field shape,
until materialization. This is uncorrected in `c1927f7324` and is included in the
ongoing first review. Source edits are held until the review's correction set.

Successful nonempty `.escn` load/save, prefab apply/revert, undo/redo and subset
round trips have not been executed. Their production authoring UI is step 6;
complete renderer fixtures need later component/import work. These remain
required final behavior validation through the real editor/native scenes.
No new fixture, test harness, script, extra CLI proof API or automated test was
introduced to stand in for that topology.

## Remaining work

The document step awaits review/correction. Renderer, editor, subsystems and
native import/export steps have not landed. Existing Node-world
operation is not evidence of the target entity model. The owner reiterated on
2026-09-10 that static-mesh components are required for renderer validation.
EntityMesh/EntityTransform already exist in the foundation; direct renderer
ingress in step 5 and the nine native renderer scenes in step 8 remain required
implementation and real-Vulkan validation, not optional follow-up work.

Scope remains the approved plan: no scripting model, Node plugin compatibility,
2D scenes, HTML/CSS game UI or automatic world streaming in this implementation.
