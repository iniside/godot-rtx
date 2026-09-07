# STEP2 round 1: native AST intrinsic semantics

2026-09-07 UTC. Named fix against frozen
`675d3cbe7f27c5337b84f84d4c8c9b9681e015bf`, within approved STEP2.
Only production file changed: `servers/rendering/shader_compiler.cpp`.

## Defects and correction

- Native `firstbithigh`, `firstbitlow` and `countbits` return unsigned values.
  The emitter now casts their result to the actual gdshader AST result type
  before enclosing arithmetic or conversions. Signed zero-input find operations
  therefore produce float -1, and signed bitCount(0)-1 remains signed -1.
  Scalar and vector overloads use the same typed emission; unsigned AST results
  retain their declared unsigned type.
- `roundEven` now emits native `rint`, whose SPIR-V target uses RoundEven.
  Native `round` uses the distinct Round operation with unspecified halfway ties.
- `textureQueryLod` now passes the texture and coordinates once, in their original
  argument order, to a generated typed helper. The selected sampler follows the
  same existing uniform/function-argument policy. Both native queries consume
  those captured values. Multiview captures the original coordinate and applies
  the existing view-coordinate function to that value inside the helper.

Clangd located `_dump_slang_call` and `compile` in the current root compilation
context; declarations/implementation were read. Actual shader_language.cpp
bitCount/findLSB/findMSB overload entries define the signed/unsigned return types.
Pinned [Slang intrinsic source](https://github.com/shader-slang/slang/blob/v2026.13.1/source/slang/hlsl.meta.slang)
was read at countbits/firstbithigh/firstbitlow and scalar/vector rint definitions
(downloaded file lines 9403,10921,11032,14482,14514). Shader binding/XML inspection
from STEP2 remains applicable: no public API or serialization change. History
identifies the defective target mapping as the preceding fork-local STEP2 commit.
Applicable failure classes: 3/4/7. No comments, tests or assertions were added.

## Actual generated-code and GPU evidence

Temporary artifacts: `%TEMP%/godot-shader-unification-20260907/step2-round1-*`.
The accompanying `step2-round1-evidence.txt` retains commands, outputs and hashes.

The TEMP diagnostic replaces only winmain and links the newly built production
editor objects. It calls actual `ShaderCompiler::compile` on gdshader source for
both GLSL and Slang targets. The returned body/helpers and uniform declarations
are placed in a small compute wrapper, then passed through production ShaderRD,
RD, container creation, Vulkan dispatch and buffer readback. The wrapper does not
reimplement the operations under review. A separate actual emitted fragment module
contains the texture-query side effects; it is compiled and disassembled, not
claimed as a executed query-result readback.

RTX 4090/Vulkan readbacks (each backend, 5 float4 rows) printed identically:

```
-1 -1 -1 -0
 0  2  2  0
-1  0 -1  0
-1  3  0  0
-1 31  1  0
```

These cover scalar and vector signed find operations, signed population-count
arithmetic, and scalar/vector halfway values 0.5,1.5,2.5,-0.5. Actual native SPIR-V
contains signed bitcasts before ConvertSToF, plus scalar and vector RoundEven.
Both numeric outputs and the query fragment pass `spirv-val --target-env vulkan1.3`
(exit 0 for each). Diagnostic process exits 0.

The query gdshader indexes a texture array with `next_index(inout ticks)` and
obtains coordinates with `next_uv(inout ticks)`. Generated source calls each once.
SPIR-V main calls next_index, then next_uv, then the query helper; inside the helper
both OpImageQueryLod operations use the same coordinate parameter. This proves the
reviewed evaluation-count/order defect is removed without relying on a duplicate
hand-authored implementation.

`scons platform=windows target=editor accesskit=no d3d12=no -j16` completes in
35.22 seconds. A gallery run using this final fix source completes capture result 0
at frame 211; the image was inspected and no shader/pipeline ERROR is logged.
Existing unsupported-material warnings and legacy OpDemote/OpTypeForwardPointer
prints remain. This is not a claim of warning-free execution or pixel equality.

The previous rich material diagnostic established AST compilation and reload layout
handling. Its dormant hg1/hg2 warnings mean it is not evidence of visible supported
uniform semantics. This fix does not broaden that claim. The prior template build
belongs to frozen675d3cbe7f; this named fix rebuilt the editor. Final STEP5 still owns
packaged/baker/cache, float/double and comprehensive rendering-topology validation.
