# GPU microgeometry implementation status

Started 2026-09-09 from `43c02e86622aa45e7fce2b2a53a1b180f2035f3c`.
Owner authorized implementation and supplied `rawcontent/xyzrgb_dragon.glb`.
[Execution plan](../plans/2026-09-09-0900-gpu-microgeometry-plan.md) recorded in
`414a19fdef1c3bbd799d6fd13cccf8f36ae3c6cf`; fresh plan review returned PASS,
examining the exact commit and cumulative baseline-to-plan diff. This is a plan
gate, not implementation/runtime evidence.

## Input evidence

GLB header/JSON inspected directly; actual import evidence appears below:

- Size: 202136944 bytes; GLB version 2.
- SHA256: `6992f4b0f141f8c30ae9fc911d9d8ee34f24ff18bb2db9feefac795e291fe0c2`.
- One mesh/primitive, 3609600 vertices, 7219045 indexed triangles.
- POSITION, NORMAL, TEXCOORD_0; one opaque nonmetallic material, roughness 0.6.
- No skins or animations.
- Input is pre-existing untracked owner content and remains unchanged.

## Execution

Step 1 committed `fd74e8ca4f181b960f493c61d07fb2a1fc87f659`: derived resource,
builder, import/reimport/export and minimal compilation closure. Fresh review
rejected three defects: duplicate-content resource path takeover, external mesh
identity loss on repeated reimport, and same-content reuse without verifying
existing payload integrity. Fix commit `0f3f9757c22056b7fd4df1ad9b851860b0ee306d`
shares canonical resources after preparation, preserves cached mesh identities,
and validates existing page digests. Both affected objects compiled; fresh final
round 2 returned PASS against the frozen fix, original and cumulative history.
All 16 owned C++ object targets compiled successfully. At this step, full editor link stopped at two old RT call signatures; Step 5
subsequently removed that blocker. Import execution is recorded below; GPU
validation remains outstanding.
Step 3 RD/graph/backend work ran in a disjoint independent context while Step 1
owned scene/import/RS attachment files. Its buffer-level API does not depend on
the new asset schema. Legacy static AS callers temporarily prevent a full editor
build, as allowed by the plan.
Step 3 committed `e27014049e5527796e06c0aba53140e9827b1e52`. Fresh review rejected
one raw CLAS memory-visibility defect: the Vulkan generic/buffer barrier path
removed AS-read access requested by the new graph dependencies. Fix commit
`57fa1e2e07a1dc2b04a7f10177e61d2cbe30fabd` preserves read visibility; fresh round
2 returned PASS against the frozen target and cumulative task diff. The reviewer
matched the fixed Vulkan syntax receipt to the committed source. Author reports
MSVC syntax validation of RenderingDevice, graph
and Vulkan translation units, and the fixed Vulkan TU. GPU execution is not yet
validated.

Step 1 ordinary editor build required `accesskit=no d3d12=no` because the local
SDKs are unavailable. The Vulkan build reaches the declared old RT call errors;
this does not validate D3D12 or establish a successful editor build.

Step 2 committed `0f4733e65e1f250f0d3e0ac515c6692f4f90315e`, task baseline
`0f3f9757c22056b7fd4df1ad9b851860b0ee306d`. It includes shared page storage,
worker reads, group readiness and terminal pins, generation checks, bounded
residency feedback, persistent scene/material records and dirty registration.
A narrow native RD completed-submission serial gates pool slot retirement.
Seven owned C++ objects compiled; full build still stops at the two deferred
RT calls (now render_raytracing.cpp:1419/1422). Evidence logs are
`C:/Users/lukas/AppData/Local/Temp/godot-micro-step2-owned-build.log` and
`C:/Users/lukas/AppData/Local/Temp/godot-micro-step2-final-build.log`.

Fresh Step 2 review resumed on 2026-09-09 after the owner retried. Earlier spawn
attempts returned `agent thread limit reached`; the latest fresh reviewer started
successfully. Round 1 returned REJECT for two defects: material ingestion before
BindlessBlock initialization cached zero texture indices, and procedural RT state
transitions did not dirty persistent deformation eligibility. Fix commit
`f97a605510271cb1f7cc0d0a3033837e49074732` initializes the existing BindlessBlock
at material ingestion and dirties both procedural setters. The ForwardClustered
object compiled; RT retains only the two deferred old-call errors. Fresh final
round 2 returned PASS at `f97a605510271cb1f7cc0d0a3033837e49074732`, including the
original step and cumulative diff.

Step 4 committed `1cba9a48c3f615ef15128fccdd9c132e61a7ba0b`, task baseline
`f97a605510271cb1f7cc0d0a3033837e49074732`. It adds GPU DAG selection, native
vertex pulling, compatible-bin indirect commands, single-view temporal HZB and
current-depth recovery, and explicit scenario context. Surface, shadow/DP,
collider, material, UV2 and SDF consumers are wired. Multiview conservatively
retains geometry without frustum/HZB rejection. Imported format/build 2 adds
parent adjacency, coarse count and original vertex IDs; importer versions are 3.
Twelve scoped C++ objects and 164 native plus four compute shader compilation/
SPIR-V validation invocations passed, according to retained receipts in
`C:/Users/lukas/AppData/Local/Temp/godot-micro-native-step4-20260909/`.
Round 1 source review rejected geometric HZB occlusion for fragment DEPTH writers
and coarse streaming fallback during one-shot UV2 baking. Proof audit rejected
the pinned-compiler claim (receipts used SDK CLI, not the exact pinned DLL) and
the compiler driver returning success despite failed/missing jobs. Existing
per-case successful SDK compilation and ABI observations remain bounded evidence;
the object build was eight compiled plus four up-to-date targets. Fix commit
`ec399801bfb9bfeab4bd7e1867ebbc13a06b36d1` disables geometric HZB rejection for
DEPTH-writing materials and uses an exact temporary native mesh for one-shot
UV2 baking, independent of page residency. General microgeometry UV2 remains
wired; the one-shot bake is an explicit native exception. The corrected compiler
driver reports 164 raster and four compute successes with strict cardinality and
the verified pinned compiler. Final source and proof reviews both returned PASS
at `ec399801bfb9bfeab4bd7e1867ebbc13a06b36d1`, including original and cumulative
history. Fresh pinned disassembly and exact incremental object-build receipts
are retained.
At the Step 4 gate, actual import/rendering had not run. Frozen ABI: GPUAsset 120, GPUGroup 48,
SelectedCluster 64, Task 72,
Parameters 432, native InstanceData 192/224 bytes. At this gate, full static buffers remained for unmigrated RT consumers. Cuts are transient; frozen inspection needs
group pins before retaining them across storage updates.

Step 5 committed `ec9a584644c14c889b91ac1c939d2eb98feedaab`, baseline
`ec399801bfb9bfeab4bd7e1867ebbc13a06b36d1`. Shared resident CLAS and GPU-selected
per-instance BLAS/TLAS replace the old static cluster assembly. Selected payload
hit decoding preserves native material consumers and original emitter identities.
Environment controls default to 4 output-image pixels and a 2x offscreen
multiplier. Static exact GPU buffers are recreated for native consumers and
retired after completed use; public RD RID exposure pins them.
Full ordinary Windows editor/console build 04 passed in 42.17 seconds with
`accesskit=no d3d12=no`; source hashes remained unchanged. Host receipts:
`C:/Users/lukas/AppData/Local/Temp/godot-micro-step5-host-20260909/`.
The shader closure reports 56 current pinned compiler/SPIR-V variants in
`C:/Users/lukas/AppData/Local/Temp/godot-micro-selected-step5-20260909/final-current-source-closure.json`.
Round 1 source review rejected cached request-counter exhaustion, publication of
new cuts before history invalidation, missing TIME material tracking, and incorrect
behind-camera RT error/offscreen classification. Fixes are in progress. Proof
review independently confirmed the 56 distinct artifacts, hashes, pinned tools
and ABI, but rejected the final verifier: it did not enforce unique expected
variant identities or exit nonzero on a failed report. That correction is also
in progress; no final Step 5 review PASS is claimed.
Cut membership history feedback is asynchronous and can lag the changed GPU cut
by in-flight frames. Dense cluster membership storage and real GPU behavior
remain unvalidated.

## Actual dragon import

Manual fixture committed `b9bd0e7f5f3b11288e8e388dab2212fcdf73254d`. Initial
headless native import passed in 82.661853 seconds; an explicit native reimport
passed in 67.038237 seconds. Builder times were 30.502 and 24.674 seconds.
Resource loading and instantiation observed one surface with external
MicroGeometry, 163762 clusters, 81649 leaf clusters, 10057 groups, 17 levels,
one terminal group and 9737 pages. The 14422982 triangles are summed across
all DAG levels, not the source or selected cut.
The 296370993-byte `.mgdata` was reused byte-identically on reimport; SHA256
`7ab3d860fcfa6fc6803924a457b9133cf919d618e81f1c321d083fe23ebc4066`.
The regenerated `.scn` changed hash; whole-scene determinism is not claimed.
Input bytes and timestamp were preserved.

Evidence: `C:/Users/lukas/AppData/Local/Temp/godot-dragon-import-5c0c6ff0291d42adb35826fef8a50cc1/`.
Execution used an immutable intermediate Step 5 build 03 snapshot, executable
SHA256 `a1232f8b34de247b6e829c4190ca267a5d938065f3124831f373a7c4b3fc03e0`;
importer/format sources were stable, but this is not final Step 5 GPU evidence.
Four glTF packed-byte-stride warnings occurred. Diagnostic reimport using
`--editor --script` produced shutdown leaks/errors; that command is not in the
shipped fixture instructions. The shipped stats-only inspector subsequently
passed in 3.399673 seconds with empty stderr. An editor documentation parser
error caused by the self-closing tutorials element was corrected separately in
`22089c9781` and included in build 04. Fixture source and proof round 1 confirmed the import evidence but rejected
README artifact-location wording. Documentation fix `b03b5710aa` explicitly
keeps both `.godot/` and the adjacent `.import` sidecar untracked. Fresh final
source and proof reviews of that correction are running. No automated test suite was authored or run.

Step 6 remains pending: selected raster/RT cluster debug views and frozen
selection, counters, real Vulkan rendering, export/PCK, native save/load and OBJ,
and required double/template build coverage. Continue review closure and this
remaining implementation under the existing owner authorization.

Existing unrelated dirty scenes/documents and the source dragon are preserved.
