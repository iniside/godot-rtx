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

## Remaining work

Step 3 world lifecycle/hierarchy/transform replacement is in progress from
`4e2db6d6ab`. Document, renderer, editor,
subsystems and native import/export steps have not landed. Existing Node-world
operation is not evidence of the target entity model.

Scope remains the approved plan: no scripting model, Node plugin compatibility,
2D scenes, HTML/CSS game UI or automatic world streaming in this implementation.
