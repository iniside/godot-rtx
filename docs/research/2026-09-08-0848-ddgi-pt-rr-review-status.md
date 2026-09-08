# DDGI/PT/RR post-commit review

Current closure source (2026-09-08): `968d4e9e8ef7884552b82c70c3886e9fc5f4efae`.
Fresh review of that exact commit and corrective range `82c5e7a0a5..968d4e9e8e`
returns PASS. The material-cache RID refresh, whole-graph VisualShader reset,
resource self-copy and Windows post-main-loop timer corrections are reviewed.
Current builds, both-mode external reload, saved-resource restart and exported
SR/RR rendering are recorded in the [closure status](2026-09-08-0935-ddgi-pt-rr-closure-status.md).
Independent proof audit returns PASS for bounded amended Step 8 claims.
Final exported SR/RR runs render and exit 0. The earlier intermittent exit
crash remains recorded with its unrecovered-stack limit; no deterministic
reproduction or universal reliability claim is made. The historical broad
missing-proof list below describes earlier reviews and is superseded.

Owner scope correction (2026-09-08): VR/XR work and multiview validation are
excluded from closure. The closure-specific early multiview guard and
XR-to-mono recovery change are removed. Historical multiview requirements
below no longer apply. DDGI/path-tracing visual parity is not an acceptance
criterion; active work covers DLSS/RR, path tracing and camera-following DDGI.

Frozen target: `52c8563dc828036904f7f6c727994dd5b60bbeae`.
Cumulative baseline: `381c36eec7d7e0d5c9111be366a3a0ae78cdadd3`.

The final implementation was handed off before its mandatory fresh post-commit
review. Working implementation checks were not a substitute. The owner called
out this omission; a fresh hostile-reviewer and separate proof-auditor then
examined the frozen commit and cumulative task diff. Both used Astra/high.

## Round 1 — REJECT

The code reviewer attacked failure classes 1–9 and found:

1. Unsupported material callbacks leave native emission zero. Raw PT/PT RR
   used that value instead of the existing packed magenta diagnostic, rendering
   unsupported surfaces black (`pathtracing.slang:39–44`, camera composition
   override in `rtxdi_frame.slang:218–220`).
2. Environment, RenderingServer, ProjectSettings and Viewport XML descriptions
   still said the now-integrated DDGI/PT/RR paths were not implemented.

The proof-auditor independently confirmed the three executable hashes, the
terminal-newline-only correspondence to final source, all 20 retained native
shader stages, 17 exported manifest entries, actual SR M/RR evaluation,
stationary motion-buffer/world replacement failure-path coverage, same-size
RR/SR transitions and historical DDGI raw readbacks. These bounded results pass.

The final-plan proof nevertheless remains rejected:

- The temporary compiler driver derived its expected stage count from the same
  regex discovery as its outputs; absent markers could silently remove required
  stages, including the empty-set success case. The existing 20 results are
  valid; the reusable gate is not.
- Required Inspector/save-reload, material/VisualShader/texture hot reload,
  initial partial-setup failure and unsupported multiview observations are not
  closed by runtime property assignments or successful startup.
- Three sparse motion images do not establish contiguous temporal stability.
- Individual viewport GPU times and RD allocations do not establish per-pass
  timing, probe-update counts and process/device VRAM for the chosen settings.
- Final GPU/export logs and images do not independently retain command,
  process exit, launch binary hash and sanitized environment. Those appeared
  in tool responses, not a durable per-run receipt. Bundled DLL execution is
  proved; the stronger environment/exit claims need retained provenance.

## Correction status

The PT emission read now uses the existing surface-buffer diagnostic authority.
The four affected class XML files describe implemented behavior and current
availability. All four XML files parse successfully. Editor, double editor and
debug-template builds succeed with stable inputs (`review-editor`,
`review-double`, `review-template` receipts under the existing evidence root).

The new temporary `compile_lighting_review.py` preserves the historical driver
and requires the explicit per-file inventory and complete set of 20 stage
identities. `review-native-final/receipts.json` records all 20 successful
compiler/validator outputs. This is a native shader diagnostic, not an automated
test suite. The source/XML correction is committed as
`31e3a94fbce898f06681dc0d5550380b891bba64`.

## Round 2 — bounded corrections PASS; full-plan proof REJECT

A fresh hostile-reviewer and a fresh proof-auditor independently returned PASS
for the named corrections at `31e3a94fbce898f06681dc0d5550380b891bba64`.
Both used Astra/high; neither round-one reviewer was reused.

The source review confirms shared diagnostic emission and binding/XML closure,
without changed signatures, enum values, shader layouts or a second diagnostic
authority. It inspected the exact fix, original task history and relevant
cumulative changes, applying classes 1, 3, 4, 5, 6, 8 and 9.

The proof audit independently verified the new explicit 20-stage inventory and
all compiled artifact/dependency hashes, three rebuilt executable hashes, and
the representative unsupported-material GPU case. The temporary room fixture
sets actual StandardMaterial3D meshes to SHADING_MODE_UNSHADED. The five
`review-unsupported-{raw,rr,nrd,hybrid-rr,hybrid-nrd}` PNGs all show magenta
geometry, as independently viewed by the auditor. The associated launch
receipts retain argv, cwd, timestamps, binary hashes, selected SDK environment
and actual exit 0. The two RR logs explicitly record feature 1001 evaluation
with `sl::eOk`. Hybrid NRD used DDGI off; hybrid RR retained DDGI.

`run_review_capture.py` records execution, not image acceptance: its
classification remains null and all image claims require inspection. The new
five receipts cover these editor runs only. They do not retroactively prove
the environment or process status of historical exports/motion captures.

Full Step 8 proof remains REJECT for the editor/serialization/hot-reload,
partial-setup/multiview, contiguous-motion, per-pass/probe/VRAM and historical
export-provenance obligations listed above. The task is not represented as
fully verified or complete. No automated tests were added or run.
