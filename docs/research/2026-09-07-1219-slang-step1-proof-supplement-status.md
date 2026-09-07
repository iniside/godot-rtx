# Slang step 1 proof supplement status

Date: 2026-09-07 12:19 UTC

Production revision: `836f8c7e7735ce82732400366d791acb26784646`. This supplement corrects diagnostic process-exit evidence only. Production source and the frozen step 1 report were not edited. No automated tests or assertions were authored or run.

## Diagnostic correction and provenance

The existing temporary CLAS/ABI and nested-include diagnostic entry points now return `EXIT_FAILURE` for missing RenderingDevice, invalid embedded shader/version, empty compiler output, output-file errors, empty container, invalid Vulkan shader, or invalid compute pipeline. Their return value reaches `os.set_exit_code()` before normal engine cleanup. Missing-DLL compilation remains an intended failure; it is no longer hidden by a successful process exit.

All temporary evidence is rooted at `C:\Users\lukas\AppData\Local\Temp\godot-shader-unification-20260907` (abbreviated `$D` below). These files remain outside version control.

The diagnostic builds use the root compilation database Windows entry point command and the captured production editor link command, substituting only their temporary Windows main object. They link the existing production engine objects/libraries; no production hooks were added. MSVC environment selection is `MSVC_VERSION=14.5`, `TARGET_ARCH=amd64`. Builds completed before step 2 was released to rebuild engine artifacts.

Build invocations from `G:/Projects/godot-rtx`:

```powershell
python "$D/build_step1_exit_diag.py"
python "$D/build_step1_fix_exit_diag.py"
```

Both builds completed successfully. The exact expanded compiler commands are `step1-exit-diag-compile-command.txt` and `step1-fix-exit-diag-compile-command.txt`; complete linker arguments are `step1-exit-diag-link.rsp` and `step1-fix-exit-diag-link.rsp`. Build output is retained in `step1-exit-diag-build.log` and `step1-fix-exit-diag-build.log`. The scripts derive their link arguments from `step1-editor-build.log`.

## Recorded invocations and exit codes

All invocations use the repository root working directory. `capture_step1_proof.py` clears `SLANG_DIAG_SOURCE`, `SLANG_DIAG_INCLUDE`, and `SLANG_DIAG_ENTRY` before applying the environment overrides shown below, saves stdout/stderr and a JSON receipt, and exits with the actual child-process exit code. It contains no result assertions. Receipts preserve UTC timestamps, exact argv, command line, source/executable hashes, DLL presence/hash, and output paths.

### cluster: exit 0

UTC start: `2026-09-07T12:15:30.843930+00:00`.

```powershell
$env:SLANG_DIAG_SOURCE = "C:\Users\lukas\AppData\Local\Temp\godot-shader-unification-20260907\cluster.hlsl"
& C:\Users\lukas\AppData\Local\Temp\godot-shader-unification-20260907\step1_exit_diagnostic.exe --path G:/Projects/godot-rtx/demos/rtxdi_manual --rendering-driver vulkan --rendering-method forward_plus --audio-driver Dummy --verbose
```

Receipt: `step1-proof-cluster.receipt.json`; console receipt: `step1-proof-cluster.command-and-exit.log`; raw output: `step1-proof-cluster.stdout.log` and `step1-proof-cluster.stderr.log`.

### abi: exit 0

UTC start: `2026-09-07T12:15:34.768155+00:00`.

```powershell
$env:SLANG_DIAG_SOURCE = "C:\Users\lukas\AppData\Local\Temp\godot-shader-unification-20260907\abi.slang"
$env:SLANG_DIAG_INCLUDE = "C:\Users\lukas\AppData\Local\Temp\godot-shader-unification-20260907\abi-include.slang"
$env:SLANG_DIAG_ENTRY = "abi_entry"
& C:\Users\lukas\AppData\Local\Temp\godot-shader-unification-20260907\step1_exit_diagnostic.exe --path G:/Projects/godot-rtx/demos/rtxdi_manual --rendering-driver vulkan --rendering-method forward_plus --audio-driver Dummy --verbose
```

Receipt: `step1-proof-abi.receipt.json`; console receipt: `step1-proof-abi.command-and-exit.log`; raw output: `step1-proof-abi.stdout.log` and `step1-proof-abi.stderr.log`.

### nested: exit 0

UTC start: `2026-09-07T12:15:37.327401+00:00`.

```powershell
$env:SLANG_DIAG_SOURCE = "C:\Users\lukas\AppData\Local\Temp\godot-shader-unification-20260907\cluster.hlsl"
& C:\Users\lukas\AppData\Local\Temp\godot-shader-unification-20260907\step1_fix_exit_diagnostic.exe --path G:/Projects/godot-rtx/demos/rtxdi_manual --rendering-driver vulkan --rendering-method forward_plus --audio-driver Dummy --verbose
```

Receipt: `step1-proof-nested.receipt.json`; console receipt: `step1-proof-nested.command-and-exit.log`; raw output: `step1-proof-nested.stdout.log` and `step1-proof-nested.stderr.log`.

### missing: exit 1

UTC start: `2026-09-07T12:15:55.866031+00:00`.

```powershell
$env:SLANG_DIAG_SOURCE = "C:\Users\lukas\AppData\Local\Temp\godot-shader-unification-20260907\cluster.hlsl"
& C:\Users\lukas\AppData\Local\Temp\godot-shader-unification-20260907\step1_exit_diagnostic.exe --path G:/Projects/godot-rtx/demos/rtxdi_manual --rendering-driver vulkan --rendering-method forward_plus --audio-driver Dummy --verbose
```

Receipt: `step1-proof-missing.receipt.json`; console receipt: `step1-proof-missing.command-and-exit.log`; raw output: `step1-proof-missing.stdout.log` and `step1-proof-missing.stderr.log`.

Before this launch, only the owned temporary `slang-compiler.2026.13.1.x86_64.dll` was moved to `.proof-disabled`; a `finally` block restored it afterward. The receipt records DLL absent before launch. Output reports `DIAG stages 0` and `Missing or mismatched pinned Slang compiler`; the actual process exit is 1. The embedded shader was already cached and valid; the direct CLAS compilation failed. Any existing `.api.spv` recorded by this failure receipt is an earlier successful artifact, not output from the failed compile.

### validate-cluster: exit 0

UTC start: `2026-09-07T12:15:55.510593+00:00`.

```powershell
& C:\VulkanSDK\1.4.357.0\Bin\spirv-val.exe --target-env vulkan1.3 C:\Users\lukas\AppData\Local\Temp\godot-shader-unification-20260907\cluster.hlsl.api.spv
```

Receipt: `step1-proof-validate-cluster.receipt.json`; console receipt: `step1-proof-validate-cluster.command-and-exit.log`; raw output: `step1-proof-validate-cluster.stdout.log` and `step1-proof-validate-cluster.stderr.log`.

### validate-abi: exit 0

UTC start: `2026-09-07T12:15:55.675840+00:00`.

```powershell
& C:\VulkanSDK\1.4.357.0\Bin\spirv-val.exe --target-env vulkan1.3 C:\Users\lukas\AppData\Local\Temp\godot-shader-unification-20260907\abi.slang.api.spv
```

Receipt: `step1-proof-validate-abi.receipt.json`; console receipt: `step1-proof-validate-abi.command-and-exit.log`; raw output: `step1-proof-validate-abi.stdout.log` and `step1-proof-validate-abi.stderr.log`.

## Observed results and boundary

| Invocation | Observed result |
| --- | --- |
| CLAS | Embedded shader valid; one compiled stage; 1704-byte SPIR-V, 732-byte container; Vulkan shader and compute pipeline valid; exit 0. |
| ABI | Embedded shader valid; one compiled stage; 2660-byte SPIR-V, 836-byte container; Vulkan shader and compute pipeline valid; exit 0. |
| Nested include | `ProbeShaderRD` cache miss followed by valid embedded shader; SDK and local includes are in the generated header. The existing additional CLAS path creates its shader and compute pipeline; exit 0. |
| Missing DLL | Compiler produces no stages; missing pinned compiler diagnostic; exit 1. |
| Both spirv-val invocations | `--target-env vulkan1.3`; exit 0 and empty stdout/stderr for both artifacts. |

Device: Vulkan 1.4.351, Forward+, NVIDIA GeForce RTX 4090. Logs retain overlay-layer warnings, the pre-existing RTXDI material diagnostic, and re-spirv unsupported-op messages. Successful exit here means the explicit diagnostic compile/container/shader/pipeline operations succeeded; it does not certify that all engine warnings disappeared.

Failure-taxonomy class 5 covers the build/include and artifact linkage evidence. This adds reliable exits to the earlier step 1 CLAS/ABI and nested include evidence. The nested shader is created through production ShaderRD after a cache miss; its pipeline is not created separately. CLAS/ABI compute pipelines are created but never dispatched. No image/readback, export execution, other backend/platform, concurrency stress, or migrated-consumer validation is claimed. The existing SCons graph evidence remains `step1-fix-nested-scons.log`; graph construction was not rerun in this exit-evidence supplement.

## SHA-256 artifact identities

| Artifact under `$D` unless absolute | SHA-256 |
| --- | --- |
| `step1_exit_diagnostic.exe` | `c2adb28e76fa198599960074452d44ee0b2d2b35caef87c5599d803368b972af` |
| `cluster.hlsl` | `4b9b2671e09a1957186c9e622e516953c297080c648be70484f1f47c7f84ee0f` |
| `step1_exit_diagnostic.cpp` | `10549f40313497b1850d8cca984b7a51819a8f1f224ec4a39cb2f39f0ab81140` |
| `embedding.slang.gen.h` | `0735adc42c2898bf39ad6137824b0722c34a1211741c696b9c55feb7b70215a7` |
| `slang-compiler.2026.13.1.x86_64.dll` | `6cd4e6bee3ee8a1f8349f77c838ac711a3d217465771b114f461075b77b71b40` |
| `cluster.hlsl.api.spv` | `03e315e5e5e87dd8ac557f9f8dc8c669f9308d9abf086459caedc9b3bddeeb64` |
| `abi.slang` | `314914d961503d9fd2a4e9bb75e4c295280b8cbfb69f492c2d7e095ba39c0e8f` |
| `abi-include.slang` | `d802fcde839781c6a96a1904c3e8ab62a526b9b76026016ffbf3ff1689452aef` |
| `abi.slang.api.spv` | `d990f102c42ada9a9f9a876b407b955e3fe9d10b644dea8ee94d8764c146ee6b` |
| `step1_fix_exit_diagnostic.exe` | `8f5f572faabf361ec60b8fc8c694374b44c9b5bf9dc471f20dddd8ec3c5f3dc3` |
| `step1_fix_exit_diagnostic.cpp` | `827cd7919d637ffbfb8f20697a1ba22677151e9d5e3efe9972a7ebd7e7bc8d1f` |
| `scons-nested-sdk\nested\shaders\probe.slang.gen.h` | `0016fd181e9fcb12e726ea2379ba059becffa30ce7fd052a125c0c8835ec3dc0` |
| `C:\VulkanSDK\1.4.357.0\Bin\spirv-val.exe` | `55b0bde93c20d427a95d6deb95181ab6621d9bb18e6f1519d30ceba8d3f1de17` |
| `capture_step1_proof.py` | `4e3478e096aa04b0be12bf90bdf91c623d3bd02281bd4e1c6b5e5e292648f4d2` |
| `propagate_diagnostic_exit.py` | `e4e8fdbe8af4dc1ef0bd8d19947c104f1858dac46f6a5e8046b532da4414df82` |
| `build_step1_exit_diag.py` | `0023eb3481dcbcb379ddf796f5c971840c257edd479652a879562becbfd850da` |
| `build_step1_fix_exit_diag.py` | `969cc6eb2b5f018353380a1fb3379d296e987dcab048a66d3fb74975a7b7a333` |
| `step1-exit-diag-compile-command.txt` | `d3791c87462255c838c229965cbe91c9397f3bf3fbd2b8be928f6a51a94579e9` |
| `step1-fix-exit-diag-compile-command.txt` | `60a89921215452fa6a04e70aae4be09bf1e9f16f9d2681f0d192cef41d2004e4` |
| `step1-exit-diag-link.rsp` | `a062fb28308855535440a02a4549b750111d95728f1b3ca9b3663bc2536be387` |
| `step1-fix-exit-diag-link.rsp` | `338c1d5bbd17a0b88a92cd1eb44f4788df88fbe67bdd591ff8945e8c8aaca730` |

The parent owns canonical status maintenance and persistence/commit coordination. This supplement is provided separately so the frozen source review can remain unchanged.
