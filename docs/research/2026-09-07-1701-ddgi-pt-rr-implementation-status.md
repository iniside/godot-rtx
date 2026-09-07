# DDGI / PT / RR implementation status

Owner approval: "implementuj plan", 2026-09-07 UTC.
Approved [plan](../plans/2026-09-07-1635-ddgi-pt-rr-plan.md) committed separately
as `00f73fb82f52e6244124293b5061c7f36d035487` before implementation.
Whole-task baseline: `381c36eec7d7e0d5c9111be366a3a0ae78cdadd3`.
Research: [current integration summary](2026-09-07-1635-ddgi-pt-rr-integration-summary.md).

## Required outcome

Hybrid RTXDI + automatically created camera-following DDGI from its first
working version; shared native Slang hit materials and RT scene; optional
true camera-ray PT with raw progressive reference; NRD/RR selection and retained
DLSS SR; complete settings, resource/history ownership, real Vulkan validation
and effective DLSS model provenance. The approved exclusions and validation
matrix remain in the plan. Automated tests are not authorized.

## Sequence

| Step | Status | Commit / evidence |
|---|---|---|
| 1. Public settings and per-buffer ownership | Committed; fresh round 1 PASS | `8aa9a77747340a8581c0b655e1d67d7eda1b6f2d`; ordinary editor build passed |
| 2. Common RT coordinates and temporal basis | Delegated implementation running | Starts at `8aa9a77747`; context `ddgi_step2` |
| 3. Shared native RT hit materials | Pending | Depends on step 2 |
| 4. Pinned DDGI import and moving cascades | Pending | Depends on step 3 |
| 5. DDGI lighting and hybrid composition | Pending | Depends on step 4 |
| 6. True camera-ray PT | Pending | Depends on step 5 |
| 7. RR / DLSS lifecycle and composition | Pending | Depends on camera producers |
| 8. Integrated real-device validation and documentation | Pending | Full plan matrix |

Step 1 was assigned to a fresh core-implementer context, Astra high selected for
cross-file API and resource/history ownership. It owned only the step's source
and ClassDB/XML surface; parent owns this status and maintained project index.
An independent read-only context completed the official DLSS 4.5 package
delivery investigation; provenance and the SR/RR model distinction are recorded
in the linked integration research. No driver/App change or unofficial payload
is authorized.

Step 1 commit contains 21 owned files, including explicit scenario-change
render-buffer reconfiguration in `renderer_viewport.cpp`. No RenderDataRD
signature change or shader/algorithm resource was added. XML parsing and
`git diff --check` passed according to the writer; fresh source review passed.
Both ordinary editor builds completed with exit 0 using
`scons platform=windows target=editor accesskit=no d3d12=no -j16`.
Logs: `C:/Users/lukas/AppData/Local/Temp/ddgi-step1-editor-build.log` and
`C:/Users/lukas/AppData/Local/Temp/ddgi-step1-editor-final-build.log`.
The parent inspected the frozen commit and final log ending in successful
target completion (elapsed 00:01:04.21). Fresh read-only reviewer
`ddgi_step1_review_r1` reviewed the exact commit, step baseline `00f73fb82f`
and whole-task baseline `381c36eec7`, returning PASS with no concrete defect.
The reviewer checked all ten properties and enum/XML closure, shared nonvirtual
server propagation, range validation, per-buffer epochs and scenario cleanup.
Applicable classes 1-9 were examined using clangd, source/XML, history and bounded
text inventories. Save/reload, separate rendering thread, template/double axes
and GPU execution remain unverified; build logs do not independently establish
the binary hash-to-commit relationship.

Step 2 is assigned to a fresh core-implementer, Astra high for coordinate
precision and temporal contracts. It owns the complete RT-origin conversion,
current/previous coordinate consumers and scene-mutation generation/invalidation
work specified by the approved plan. Ordinary and double compiler validation
are requested; no automated tests or new demonstration fixtures are authorized.

## Verification boundary

Initial machine inspection on 2026-09-07: `nvidia-smi` reports RTX 4090,
driver 616.64 and 24,564 MiB VRAM. Ordinary/double editors and template binaries
exist from earlier work; they are not evidence of this implementation. No
running Godot process was observed at that inspection. No driver was changed.

The approved plan passed fresh final round 2 review. Step 1 passed its bounded
source review; no new runtime result is claimed. Each completed
step is committed and reviewed against its exact frozen commit plus cumulative
task changes before dependent implementation continues. Final completion also
requires the plan's real-device, export, history and mode validation.

## Worktree preservation

At task start the staged set was empty. Pre-existing owner changes include
`demos/rtxdi_manual/project.godot`, maintained direction/project-state documents,
the earlier DDGI research correction, and untracked game assets/older research.
Only the approved plan was staged for the plan commit. Do not stage unrelated
dirty documents or content wholesale in an implementation commit.
