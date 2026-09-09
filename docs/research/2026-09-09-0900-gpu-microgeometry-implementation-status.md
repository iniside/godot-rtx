# GPU microgeometry implementation status

Started 2026-09-09 from `43c02e86622aa45e7fce2b2a53a1b180f2035f3c`.
Owner authorized implementation and supplied `rawcontent/xyzrgb_dragon.glb`.
[Execution plan](../plans/2026-09-09-0900-gpu-microgeometry-plan.md) recorded in
`414a19fdef1c3bbd799d6fd13cccf8f36ae3c6cf`; fresh plan review returned PASS,
examining the exact commit and cumulative baseline-to-plan diff. This is a plan
gate, not implementation/runtime evidence.

## Input evidence

GLB header/JSON inspected directly, no importer or rendering success claimed:

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
All 16 owned C++ object targets compiled successfully. Full editor
link remains blocked by the two old RT call signatures at render_raytracing.cpp
1418/1421, awaiting planned runtime migration. Import execution and GPU validation
have not run.
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
original step and cumulative diff. Step 4 implementation has started from that
baseline, including native shader variants, GPU selection and all static native
pass consumers. Step 5's read-only consumer map is complete; it awaits stable
selected-cut contracts. Step 6 is pending. No implementation or validation
success is claimed for Steps 4–6.

Continue Step 4 and its fresh review; Step 5 needs stable shared storage and
selected-cut shader contracts. All prior owner
authorization remains in effect. The final linked editor, dragon import/reimport,
export/PCK, double/template and real GPU checks remain outstanding.

Existing unrelated dirty scenes/documents and the source dragon are preserved.
