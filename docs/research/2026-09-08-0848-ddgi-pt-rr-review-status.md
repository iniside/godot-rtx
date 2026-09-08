# DDGI/PT/RR post-commit review

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
test suite. Real-device diagnostic parity and the fresh second review remain
pending. No final PASS or complete Step 8 verification is claimed; neither
original reviewer is reused for round 2.
