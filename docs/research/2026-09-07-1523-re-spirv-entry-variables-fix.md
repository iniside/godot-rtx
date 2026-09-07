# re-spirv caller entry variable reservation fix

Bounded integration fix for the [approved shader unification plan](../plans/2026-09-07-1126-shader-unification-plan.md), downstream optimizer and final export closure.
Task start: `12924470328a540ee9b5822212bc688f21e42cdd`, 2026-09-07 UTC.
Executing agent: core-implementer, gpt-6-astra, high, selected for optimizer correctness and GPU compiler impact. Comments: default NONE.

## Authority and minimal closure

The valid captured export shader has entry function `%93` with zero Function
variables and a call to `%104`, which owns eleven Function variables. The old
`respv::Shader::inlineData` reserves the callee variable span only when it
encounters the caller's first `OpVariable`. Its missing destination remains
`UINT32_MAX` and crashes at `copyInstruction` when copying `%107` from word 1845.
The captured input is 12096 bytes, SHA-256
`b15e97709376a1a8d2ba5c3fd0f3bf73201112b033a1fd3ddbcd30f3c6f312cf`.

[Patch 0002](../../thirdparty/re-spirv/patches/0002-reserve-inlined-variables-at-entry-label.patch)
moves the existing reservation to immediately after copying the first label of
each non-inlined function. `OpVariable` only selects the existing destination.
The existing total-size calculation, per-function sentinel reset, end-of-function
fill check, result/decoration remapping, call expansion and Phi fixup remain.
Zero-sized reservations still consume the sentinel once; subsequent blocks do
not reserve again. Globals use the ordinary stream, and each retained function
gets its own span. Callee locals precede the caller's original locals, before
executable instructions in its entry block.

The patch was authored first, checked and applied with `git apply` from the
repository root; vendor source was not hand-edited. README records patch order.
No compiler-specific branch, dummy shader variable, optimizer bypass, new API,
ClassDB/XML change, resource owner, SCons input or direct-consumer edit is needed.

## Navigation and provenance

Root compile database plus clangd `find` resolved `inlineData` at line 814;
`refs` resolved its `parse` caller at line 2243. Targeted reads of the actual
header, inliner sizing/copy passes and Vulkan container path confirmed the
contract. This private SDK has no script-bound surface. The Vulkan driver at
`shader_create_from_container` constructs `respv::Shader(..., inline_data)`
after decompression/SMOL-V decode, with specialization shaders enabling inlining.
No alternate request/cache path was introduced.

`git show 076f30c19d` identifies the faulty reservation in the upstream import;
patch 0001 remains unchanged. Both recorded patches apply cleanly to that pinned
import. The reconstruction differs from the checkout solely by an older
three-line missing-result decoration guard from `4732933359`, absent from the
recorded patch set. This preexisting provenance gap is retained, not repaired
within this fix. Applying patch 0002 to exact task-start source reconstructs the
working source byte-for-byte after LF normalization (SHA-256
`7510a818fd85969cfd33996b32204b50fa38f4682f9b579df3382d112a6ffadb`).

## Validation and boundaries

The captured shader body was unchanged. A temporary compiler diagnostic uses the
production `respv::Shader(..., true)` and `respv::Optimizer::run(..., nullptr, 0)`;
it emits intermediate/final modules without assertions or synthetic fixtures.
It exits zero: 12096 input bytes, 12124 inlined bytes, 12108 optimized bytes.
`spirv-val --target-env vulkan1.3` exits zero with empty diagnostics for all three.
Topology inspection shows one retained function, eleven locals and zero calls
after inlining; the input had two functions and one call. The 37 global variables
survive inlining; existing dead-code elimination removes one during optimization.

Windows ordinary editor and template_debug builds with `accesskit=no d3d12=no
-j16` passed. Owned `git diff --check` passed. No automated tests or formatter
were authored or run. Captured-module validation proves structural correctness
of this concrete optimizer input, not visual equivalence or every legal SPIR-V
module. Multiple retained functions and zero-sized spans are source-checked;
there are no newly authored shader cases.

Applicable failure classes: 3 (shader/backend contract), 4 (vendor provenance),
5 (editor/template/double build axes), 6 (captured input, binary provenance and
real export evidence), 7 (destination bounds), 8 (scoped dispatch/commit/review),
9 (no bypass or added mechanism). Existing non-inlining behavior is unchanged.
Vulkan production export is the affected runtime topology; D3D12/Metal RT support
and generic optimizer opcode support are outside this fix. Final package rendering
and double-runtime observations belong to Step 5 and are reported there.

Retained bounded evidence is in [re-spirv-entry-variables-evidence](re-spirv-entry-variables-evidence/).
Full temporary modules, diagnostic binary and raw export logs remain under
`%TEMP%/godot-shader-unification-20260907/step5`; the multi-gigabyte crash dump is
not committed. Parent owns canonical migration status and fresh source/proof reviews.

## Final export closure

The ordinary non-headless Vulkan export of the existing isolated Step 5 project,
with shader baking enabled and the rebuilt custom template, exited zero at
2026-09-07 15:22:56 UTC. The command, exact startup project inputs and executable
hash are retained in `optimizer-fix-export.receipt.json`. Full stdout SHA-256 and
package hashes are retained in `artifact-manifest.json`, alongside a numbered
stdout excerpt and complete stderr. Export emits re-spirv unsupported-opcode
diagnostics for `OpDemoteToHelperInvocation`, `OpTypeAccelerationStructureKHR`
and `OpTypeForwardPointer`, Vulkan overlay loader warnings, the intended
unsupported-material warnings, and twelve unclaimed StringNames at exit.
These observations remain outside the caller-variable crash fix. No suppression
or optimizer-disable setting was introduced.

The separate `precision=double` editor build also passed. All three binaries
contain this uncommitted source patch and embed the task-start revision; exact
binary/build-log hashes and commands are in `optimizer-fix-build-provenance.json`.
The package and all binaries were released to Step 5 after the export process
exited. This report claims successful real-device bake/export, not yet a visual
package comparison or final migration-wide PASS.
