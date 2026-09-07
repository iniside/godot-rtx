# Internal Slang compiler

The approved renderer migration uses Slang 2026.13.1 direct SPIR-V on Windows
x86_64 Vulkan. Other platforms and backends retain their existing GLSL compiler.
This module does not add a public shader language or change RDShaderSource.

Before building a checkout, import the pinned release:

```powershell
python misc/scripts/update_slang.py
```

An offline download can be supplied with `--archive <path>`. The importer verifies
the archive SHA-256 before extracting headers, the upstream license, and the
compiler DLL. The imported manifest records each file hash. Headers and metadata
are tracked; the distributable DLL is retrieved by the importer and is not Git
build output. SCons verifies imported files and copies the DLL plus
`slang.LICENSE.txt` beside editor/template executables. Keep these files beside
installed Windows export templates. Windows export copies them to the game.

The compiler loads only that executable-adjacent DLL and verifies its pinned hash.
Missing or changed binaries produce a compilation error. There is no PATH or SDK
lookup and no per-shader compiler process.

`RenderingShaderCompileRequest` is internal C++ metadata shared by ShaderRD's live
compilation, existing cache, and export baker. Slang requests explicitly select
Vulkan 1.3/SPIR-V 1.6, direct emission, column-major Slang matrices, GL buffer
layout, optimization, debug information, stage entrypoints, and compiler identity.
The emitted SPIR-V entrypoint is `main`, matching the existing Vulkan container.
Slang's matrix representation differs from GLSL: migrated emitters must preserve
logical indexing and multiplication as well as storage, rather than inferring
CPU layout from the SPIR-V RowMajor/ColMajor decoration alone.

`RD_SLANG` embeds stage sources and recursive include contents. Its SCons emitter
records each include dependency. Includes remain separate virtual files, retaining
conditional preprocessing, include guards, pragma-once behavior and source
locations. Only embedded include files are visible to compilation. Their sorted
paths and contents participate in the existing ShaderRD cache identity alongside
the request, stage templates, variant/custom defines and material code.

One mutex serializes access to the global SDK session. Each compile owns a new
session and releases its program, diagnostics and blobs before returning; core
module shutdown releases the global session before unloading the library. Final
SPIR-V continues through Godot's SPIRV-Reflect and platform shader container.
