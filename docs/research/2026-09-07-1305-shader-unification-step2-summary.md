# Spatial Slang frontend and surface contract, step 2

2026-09-07 UTC. Implements only step 2 of the
[approved plan](../plans/2026-09-07-1126-shader-unification-plan.md).
Source baseline: `836f8c7e7735ce82732400366d791acb26784646`.
Parent documentation commit `6f62140386d50bd120e133dd5ff2fa2f0ac619a7`
arrived during implementation. Owner changes and parent status files are excluded.

## Changed authority and closure

`ShaderCompiler` retains the existing gdshader parser and metadata generation,
with a native Slang AST target selected only by spatial Forward+. Native types,
constructors, operators, resource methods, sampler selection, function arguments,
arrays, structs and stage varyings are emitted from typed AST nodes. GLSL remains
the default for existing distinct consumers. Shader/VisualShader source, public
properties, uniform names/hints and serialization are unchanged.

Godot matrix columns are represented as native Slang rows. Native matrix products
reverse `mul` operands; scalar multiplication and componentwise multiplication
retain their own semantics. Constructors, indexing, inverse and compound assignment
follow that representation. Material offsets use the existing std140 calculation;
resource bindings and varying locations are explicit. Native matrix buffer fields
and spatial request defaults are row-major, including compiler-generated std140
matrix-array wrappers. Global and instance uniforms retain their existing buffers.

`SceneShaderForwardClustered::ShaderData::set_code` uses this target for all retained
spatial variants. `set_code_rt` retains the same frontend's uniform/texture layout
metadata alongside its existing routing classification. Existing unsupported RT
material diagnostics remain; no custom RT feature support is added. ShaderRD
retains the existing version/hot-reload/cache/baker path.

The former spatial `scene_forward_clustered.glsl` and its private include are
removed. Seven native sources provide the template, scene/decal data, octahedral
encoding, AA and common surface contract. Existing RD_SLANG registration and
recursive include dependency generation cover the closure. Distinct upstream
GLSL includes remain for their actual mobile, sky, fog and effect consumers;
raytracing/DI sources remain assigned to steps 3 and 4.

`surface_data_inc.slang` owns `SurfaceMaterial`, `SurfaceData`, `SurfaceBufferData`
and `encode_surface_buffers`. Material, world position/normals, view direction
and layers are separate from raster motion, jitter, depth and history. Raster
performs interpolation/derivatives/decals and supplies the common packer. The six
live render-target formats remain unchanged.

Private dead COLOR variants, flags, shader-version mappings and prewarm are removed
from the spatial owner and Forward+ caller. Prewarm now uses the actual single-view,
non-MSAA six-target RTXDI surface format, with motion vertex inputs. Depth, shadow,
material, SDF, skinning, particle-trail and editor paths remain in the same native
template. Parent clangd references and actual `_render_scene` calls established
that live scene draws use RTXDI_SURFACE; material/UV2 use DEPTH_MATERIAL, collider
uses shadow and SDF retains its own pass. History `a661887676` removed COLOR draws
and reflection captures; the old color prewarm was a stale private compile consumer.

## Compiler ABI corrections discovered by the real consumer

The compile request now identifies parameter preservation, precise floating point
and invariant vertex position. Slang requests preserve parameters and use explicit
optimization level 0. Spatial requests additionally select row-major defaults and
invariant position; unrelated GLSL requests and fragment/compute position handling
are unchanged. The complete request still reaches both live workers and baker.
Grouped ShaderRD initialization also computes resource-only cache hashes, needed
by this migrated consumer's export cache path.

The original O2 API output retained only 3 descriptors from the native double/
multiview vertex module, producing real draw-time shared-set mismatches. O0 retains
31 declarations (sets 0/1/2: 6/24/1). SPIRV-Reflect enumerates module declarations,
without filtering by entry-point access. Pinned Slang's downstream optimizer drops
unused resource declarations even with PreserveParameters, and offers no preserved
binding option on that path. O0 selects native emission without depending on the
unshipped downstream plugin. Vulkan still performs final driver code generation;
this change makes no performance improvement claim.

Pinned sources: [session option inheritance](https://github.com/shader-slang/slang/blob/v2026.13.1/source/slang/slang-session.cpp),
[native/downstream emission](https://github.com/shader-slang/slang/blob/v2026.13.1/source/slang/slang-emit.cpp),
[downstream optimization](https://github.com/shader-slang/slang/blob/v2026.13.1/source/slang-glslang/slang-glslang.cpp).
TargetDesc's default floating-point field overrides session options, so the module
sets `TargetDesc.floatingPointMode` explicitly from the same request.

The pinned native emitter has no working invariant-position attribute. The existing
final-SPIR-V compiler seam therefore inserts one narrowly scoped Invariant decoration
on the spatial vertex BuiltIn Position. Header/instruction lengths are checked,
an existing decoration is retained, and missing direct Position fails explicitly.
No new reflection authority or generic rewriting framework is introduced.

## Actual verification

All temporary artifacts referenced below reside in
`C:/Users/lukas/AppData/Local/Temp/godot-shader-unification-20260907/`.
The accompanying `step2-evidence.txt` retains the relevant transcripts and hashes.
No automated tests or assertions were written or run.

- Editor builds: `scons platform=windows target=editor accesskit=no d3d12=no -j16`.
  Final build log: `step2-editor-final-build.log`. Earlier compile iterations caught
  and corrected one invalid AST accessor, discarded-result warnings and projected
  texture-coordinate helper emission before the final runtime diagnostic.
- Release template build: `scons platform=windows target=template_release accesskit=no d3d12=no -j16` completed successfully in 4m30s.
  `step2-template-build.log` records the no-tools build; template runtime/export
  scene validation remains in step 5. Template executable SHA-256:
  `fd9436a0b761f3cb2e114dbb8668ce30301567feabee862240a89305704e06b0`.
- Native template closure: 28 vertex/fragment variants compile and validate with
  Vulkan 1.3 spirv-val. Coverage includes surface, depth/material, shadow,
  normal/voxel export, SDF with/without atomics, point, trails, multiview, double,
  world-coordinate, skip-transform and custom-depth definitions. These are compiler
  diagnostics, not real-device execution of every topology.
- ABI disassembly: relevant original/native SceneData, InstanceData, DrawCall,
  ImplementationData and DecalData offsets/strides match. Matrix-array wrappers
  require the spatial row-major request. Logs: `native-spatial-abi-final.log` and
  `native-spatial-final-diagnostics.log`.
- Final production-linked vertex diagnostic: one replacement TEMP winmain links
  the production editor objects and calls real ShaderRD/RD compilation. The final
  double/multiview output is 38,400 bytes, retains 31 descriptors, one Invariant on
  BuiltIn Position and 60 NoContraction decorations; Vulkan 1.3 spirv-val exits 0.
  `step2-native-production-api-final.log` and `.spvasm` record the output. The binary
  is diagnostic-only; its native source is an expanded production template.
- RTX 4090/Vulkan numeric diagnostic: two hand-authored GLSL/Slang kernels exercise
  the agreed representation through production ShaderRD/RD, dispatch and readback.
  All 25 float4 rows (100 scalars) match exactly; maximum printed absolute difference
  is 0.000000000. Coverage includes nonsymmetric/nonuniform model/view/projection,
  both vector orders, composition, inverse, normal matrix, constructors, uniform
  matrix arrays and packed 3x4 affine reconstruction. Both 400-byte readbacks have
  SHA-256 `9f8563e6f8274997b8678ed20699cb83365944663127a03f0fab14b34f215951`.
  This proves executed conventions; it does not claim the kernels came from the
  gdshader AST. `step2-matrix-*` retains sources, linker/launch provenance and output.
- Real editor gallery and shadow captures: `step2-gallery-o0` and `step2-shadows`
  complete with capture result 0, no shader/pipeline/uniform ERROR diagnostics.
  Gallery image was inspected and compared with the retained baseline: visible
  geometry, material and shadow arrangement match, without pixel-equality claims.
- Real gdshader AST/material diagnostic: `step2-emitter-final` compiles matrices,
  inverse/compound products, arrays/structs, ordinary/flat/array varyings, projected,
  gradient, gather, fetch and query texture methods, packing and boolean helpers.
  A live shader reload adds a vec4 uniform and changes the UBO shape; the log prints
  STEP2_MATERIAL_RELOADED and capture result 0 at frame 439, without shader/pipeline
  errors. The first draft diagnostic itself used invalid vec3(int); that input was
  corrected to an explicit float conversion before the successful runs.

Existing unsupported-material warnings and legacy OpDemoteToHelperInvocation /
OpTypeForwardPointer diagnostic prints remain visible. They are recorded separately
from successful spatial shader compilation and draw validation.

## Navigation and limits

C++ declarations, definitions and callers were located with clangd from the root
compile_commands database, then read in this checkout. Shader `_bind_methods()`
and doc/classes/Shader.xml confirmed the public surface is unchanged. Git history
and parent semantic caller research distinguished removed fork-local COLOR code
from retained upstream/distinct consumers. Bounded text scans supplied only lower
bounds for shader includes, registration and stale identifiers. No vendor or
generated source was edited.

Applicable failure-taxonomy classes: 1/4 for preserved public/frontend contracts;
2/3 for data/layout/lifetime ownership; 5/6/7/8/9 for render topology, GPU ABI,
precision, shader variants and artifact/build boundaries. Complete final float and
double editor/template scenes, packaged shader-bake/cache, multiview device/editor
interaction and later DI/NRD migration validation belong to step 5. No claim is made
that all gdshader builtins or every retained rendering topology were executed here.

Final source patch (20 owned source paths, staged against the source baseline)
SHA-256: `d90019c4afa2d935792330a27c52f7b4659e63af235bf5bb04ad501b7b260dfd`. The final editor and template builds use this source
closure. Their embedded Git version remains the pre-commit parent documentation
SHA; runtime evidence is mapped to the actual source/build state above, not that
embedded label. No production source changed after the final editor build began.
