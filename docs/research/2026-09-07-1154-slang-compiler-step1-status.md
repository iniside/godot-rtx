# Slang compiler integration, step 1

2026-09-07 UTC. Implements step 1 of the
[approved plan](../plans/2026-09-07-1126-shader-unification-plan.md).
Task baseline: `b2d04129cb8f1ad0b0d81d6b112cbbc2be6a4272`; overall baseline:
`a89cd8a3b064427e29a89af0f0b6823bfac21da7`. The accompanying implementation
commit contains the source described below. Independent parent documentation
commits occurred during the task. Existing owner changes were preserved.

## Implemented authority

- `modules/slang`: in-process Slang API, pinned executable-adjacent DLL loading,
  global-session synchronization and per-compile session ownership. Core teardown
  releases SDK objects before unloading the DLL. No compiler process or PATH lookup.
- `misc/scripts/update_slang.py`, imported `thirdparty/slang` headers/license/manifest:
  controlled release import. Windows x86_64 archive SHA-256
  `fa1c9bcab2cdcd3626f7a1e250dd35d606c1b84745b64627f1dd63fca3746a70`
  matched the release API digest and downloaded archive. The importer normalizes
  text line endings before recording file hashes, matching Git's LF policy.
  Compiler DLL SHA-256:
  `6cd4e6bee3ee8a1f8349f77c838ac711a3d217465771b114f461075b77b71b40`.
  The DLL is intentionally untracked; run the documented importer before building
  a fresh checkout. SCons checks imported hashes and installs runtime files beside
  editor/template executables. Windows export copies DLL/license from the template
  directory. No hand edits to vendor/generated files.
- `RenderingShaderCompileRequest`, internal RD methods, `ShaderRD` and baker WorkItem:
  one request carries language, compiler identity, explicit Vulkan 1.3/SPIR-V 1.6,
  entrypoints, layout/optimization/debug options and embedded include contents.
  Existing ShaderRD cache identity includes that request; the res-only cache path
  now computes group hashes too. Existing public GLSL binding/defaults are unchanged.
- `RD_SLANG` in SConstruct/glsl_builders and the root/effects/forward_clustered/
  raytracing shader SCsub files: stage embedding, recursive include dependencies,
  virtual include paths and source locations. Includes retain their own preprocessor
  scope through an in-memory filesystem. No shader consumer is migrated in this step.
- The existing raytracing base-stage caller now forwards its owning ShaderRD request.
  SPIRV-Reflect, shader containers, SMOL-V and Vulkan creation remain the downstream
  authorities; no second reflection layout or cache was introduced.

The new private C++ seam is `RenderingDevice::shader_get_compile_request`,
`shader_compile_spirv_from_internal_source`, and the three-argument
`ShaderRD::compile_stages`. A generated `.slang` ShaderRD constructor calls existing
`setup`, then `setup_slang` and `setup_slang_include`. All stage entrypoints default
explicitly to `main`; the request can name another entry, which Slang emits as
SPIR-V `main` to satisfy the existing Vulkan pipeline contract.

## Validation performed

No automated tests, assertion harnesses, formatter runs, cache deletion or scene
edits. Temporary compiler diagnostics contain printed observations and early error
returns, and do not add production hooks.

Commands from the repository root:

```powershell
python misc/scripts/update_slang.py --archive $env:TEMP/slang-2026.13.1-windows-x86_64.zip
scons platform=windows target=editor accesskit=no d3d12=no -j16
scons platform=windows target=template_debug accesskit=no d3d12=no -j16
scons platform=windows target=editor accesskit=no d3d12=no compiledb=yes compiledb_gen_only=yes
```

Editor and template builds passed, including final bounded rebuilds after import
line-ending normalization. A final editor build regenerated shader headers
after the embedding dependency adjustment and passed. The unchanged generated GLSL
consumers remained compilable. Editor SHA-256:
`566EB6333395F1F5E2D13AEC3ACCD8743177B7A5C3B56D7AFBF37BD401D2EB47`.
Template SHA-256:
`44052406971AE6FF9E8A668CD35530EF4065A20C61F5314B013882C51183BC33`.
Binaries embed parent documentation revision `8956f08ec`; they were built with this
task's uncommitted production diff, before its source commit.

Temporary artifacts are in `%TEMP%/godot-shader-unification-20260907`:

- `build_step1_diag.py` derives the compiler flags from root compile_commands and
  the linker arguments from `step1-editor-linked-build.log`. It substitutes only a
  temporary Windows main object; production engine/module objects are linked
  unchanged. `step1-diag-compile-command.txt` and `step1-diag-link.rsp` preserve the
  actual commands. No SDK DLLs beyond the imported compiler are supplied there.
  These objects correspond to the editor build before text-only imported-header
  normalization, whose EXE hash was
  `835703EDBD99C8E4B5381ACC1756805AAAA3B381DA1C87008AF684471CCB1C9D`.
  The normalized import preserves the same SDK tokens and compiler DLL.
- `step1_diagnostic.cpp` SHA-256:
  `0DCDF63260142E0962BA22149721FF03FDF9A32FC6E847806AE09180CC4D97EC`.
  Final diagnostic EXE SHA-256:
  `E21E852C831CD89E39108674D9E603C733C7D00C07B18AD136086C332BE307F2`.
  It runs Main::setup, invokes the production ShaderRD/RD compilation path, writes
  SPIR-V, creates the existing container, creates Vulkan shader/compute pipeline
  RIDs, and frees them before Main::cleanup. It does not dispatch a kernel.
- Runtime arguments:
  `--path G:/Projects/godot-rtx/demos/rtxdi_manual --rendering-driver vulkan --rendering-method forward_plus --audio-driver Dummy`.
  Environment `SLANG_DIAG_SOURCE` selects `cluster.hlsl` or `abi.slang`;
  the ABI run also sets `SLANG_DIAG_INCLUDE=.../abi-include.slang` and
  `SLANG_DIAG_ENTRY=abi_entry`. Observed device: RTX 4090, Vulkan 1.4.351.
- `step1-diag-cluster.{log,err}`: candidate/committed NV CLAS query source produced
  1704-byte SPIR-V and a 732-byte container; shader and compute pipeline creation
  both succeeded. SPIR-V hash:
  `03E315E5E5E87DD8AC557F9F8DC8C669F9308D9ABF086459CAEDC9B3BDDEEB64`.
- `step1-diag-abi.{log,err}` and `abi.api.spvasm`: 2660-byte SPIR-V, 836-byte
  container, shader/pipeline creation succeeded. Includes, named entrypoint,
  unbounded nonuniform texture/sampler arrays, push constants, specialization ID 0,
  physical buffer addresses and Int64 reached this path. Bindings were set 0/binding
  0 and set 1/bindings 0,1; Push offsets 0/8/12; Payload offsets 0/12/16/80 and
  stride 96. Slang column-major source convention emitted a RowMajor matrix
  decoration with stride 16; step 2 must account for Slang's transposed matrix
  representation when preserving Godot indexing/multiplication. ABI SPIR-V hash:
  `D990F102C42ADA9A9F9A876B407B955E3FE9D10B644DEA8EE94D8764C146EE6B`.
- Both API-produced SPIR-V files passed
  `C:/VulkanSDK/1.4.357.0/Bin/spirv-val.exe --target-env vulkan1.3 <file>`.
  The validator is a diagnostic tool only, not a runtime compiler dependency.
- `inspect_embedding.py`, `embedding.slang`, `embedding_inc.slang` and the generated
  temporary header exercise the production header builder. The diagnostic then
  instantiates that ShaderRD and invokes its actual worker/cache path.
  `step1-diag-embedding-cold.log` records a miss and valid shader for group hash
  `1330fcedf215ef224b33c8e1705deda20b52f3319a43689df60080a2ee128c00`;
  `step1-diag-embedding-warm.log` reuses the same key without a miss.
  Changing only include constant 7 to 9 and regenerating the diagnostic header
  produced a miss and valid shader for
  `77adf9ddd2fdf941172d131910c10446135417d45ec7131979605a77e34f48f4`.
  No prior cache directory was removed.
- `step1-diag-missing.err`: temporarily moving only the diagnostic-directory DLL
  yielded the explicit missing/mismatched pinned compiler error and empty compile
  result. The DLL was restored afterward.
- `step1-editor-final-build.log`, `step1-template-final-build.log`, source readback and
  scoped `git diff --check` provide compilation/static closure.

The first compile found the SDK filesystem's required ISlangCastable method; the
implementation was corrected. Runtime diagnostics exposed a bool-to-vformat cache
identity formatting error, corrected and rebuilt before the passing observations.

## Evidence boundaries and navigation

The current Vulkan driver tries re-spirv optimization and already skips it when
its parser cannot handle a shader. Diagnostics observed unsupported
`OpTypeRayQueryKHR`, `OpConvertUToPtr`, and existing renderer `OpTypeForwardPointer`
messages; final Vulkan creation succeeded using that existing behavior. No new
optimizer fallback was added. This is acceptance through reflection/container and
Vulkan shader/pipeline creation, not proof of GPU dispatch results or matrix math.

Complete exported-project bake/run, migrated material hot reload, renderer visuals,
double precision, and real editor/template scene coverage remain steps 2–5.
Windows DLL export code compiled; an exported project was not run in this step.
D3D12/Metal were not built or executed. Their public compiler/container interfaces
and defaults are unchanged; Slang is enabled only for Windows x86_64 Vulkan builds.
GLES3/dummy and retained GLSL consumers were included in the passing Windows builds.

Navigation used clang-nav against root compile_commands for ShaderRD compile_stages,
RD shader compilation, the affected raytracing caller, OS dynamic-library API, and
pipeline creation. One-shot references were TU-local; bounded textual consumer
inventory plus the build exposed and closed the raytracing caller. Actual source,
declarations, RenderingDevice ClassDB binding and XML were read. Scoped git history
identified existing public shader infrastructure versus fork-local raytracing code.
Native SCons/Python reads established embedding/export dependencies. Slang API
declarations came from the checksum-verified imported release, alongside the
[official compilation/session documentation](https://shader-slang.org/slang/user-guide/compiling.html)
and [pinned release](https://github.com/shader-slang/slang/releases/tag/v2026.13.1).

Applicable failure classes: 1/4 public API preservation; 2/7 SDK/session/thread
lifetime; 3 downstream shader ABI; 5 editor/template/build distribution; 6 evidence
limits; 8/9 scope and commit ownership. Comments: default NONE.

## Round 1 build-graph correction, 2026-09-07 12:08 UTC

Fresh review of `3a04df35e6` rejected one class-5 defect: Slang SDK includes were
opened relative to the process working directory during the SCons emitter. SCons
enters the nested SCsub directory while constructing the graph, so `Rtxdi/...` and
explicit `thirdparty/...` includes could fail before the build action ran. The
original standalone header-generation diagnostic did not exercise this context.

The correction is confined to `glsl_builders.py`: physical include reads and SCons
dependency nodes are anchored to the repository containing the builder. Source
paths inside that repository are normalized to the same repository-relative
virtual identity for both the emitter and build action. External diagnostic sources
retain their absolute virtual identities; SDK contents retain
`/godot/thirdparty/...` keys. Local includes still resolve beside their including
source. No SDK, generated, compiler or rendering source was edited.

Actual SCons verification ran from
`%TEMP%/godot-shader-unification-20260907/scons-nested-sdk` using
`scons -C <directory> --tree=prune`. Its `nested/shaders/SCsub` invoked the production
`rd_slang_dependencies` and `build_rd_headers` through RD_SLANG. The log
`step1-fix-nested-scons.log` records the nested working directory, successful graph
construction/header generation, and dependencies on `Rtxdi/DI/Reservoir.hlsli`,
its four transitive SDK headers, explicit `thirdparty/nrd/Shaders/NRDConfig.hlsli`,
and `local_inc.slang`. Generated header SHA-256:
`0016FD181E9FCB12E726EA2379BA059BECFFA30CE7FD052A125C0C8835EC3DC0`.

`build_step1_fix_diag.py` compiled this actual SCons-generated ProbeShaderRD into
the existing temporary engine diagnostic. `step1-fix-diag-sdk.log` records a fresh
ShaderRD cache miss followed by a valid Vulkan shader for group hash
`a5c3b2bf354b11950e9f0e24295b70ad74be0093a27edb08ab5205a644c3b401`.
No compile errors occurred. The separate existing CLAS diagnostic also created its
Vulkan pipeline successfully. Diagnostic source SHA-256:
`213D4E8B12F927CDCF9C9A81F0E6CF596836502EADE5A16CE44E8418C285857A`;
EXE SHA-256:
`523A3F959A50D5C580014D7727C1B7A94B9648E0A773416DB082231D05C96965`.
Commands and linker response are retained as `step1-fix-diag-compile-command.txt`
and `step1-fix-diag-link.rsp`; arguments/device match the initial diagnostic above.

The bounded `scons platform=windows target=editor accesskit=no d3d12=no -j16` build
passed in `step1-fix-editor-build.log`; scoped diff checking passed. No automated
tests or assertions were introduced or run. Template/runtime coverage remains as
recorded above; this correction specifically verifies the previously missing
nested SCons topology. Comments: default NONE.
