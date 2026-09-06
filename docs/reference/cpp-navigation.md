# C++ Code Navigation

Detail for **C++ Code Navigation - RULE** in
`.agents/shared/research-navigation.md`.

## Fallback Chain

Do not start with a broad text sweep for a named C++ symbol:

1. Use the runtime's LSP/clangd definition, references, hover, or workspace
   symbol operation. `compile_commands.json` belongs at the repository root;
   refresh it with `scons platform=windows target=editor compiledb=yes
   compiledb_gen_only=yes` when stale and when a build is authorized.
2. Read the declaration and implementation in this checkout. It is Godot
   4.8-dev with NVIDIA patches; a remembered release signature is not evidence.
3. For script-visible behavior, inspect `_bind_methods()` and
   `doc/classes/<Class>.xml`, including bound names, properties, signals, enum
   constants, defaults, and compatibility methods.
4. Use `git log -p`, `git blame`, and the relevant upstream comparison to
   identify fork-local code and stable upstream API obligations.
5. Use `rg` only when the earlier methods fail or for a bounded text/config
   sweep. Label it a lower bound. When it finds an anchor, return to clangd and
   source reads for definitions, types, and callers.

## Non-C++ Surfaces

Clangd does not cover SCons/Python, GDScript, XML semantics, GLSL include/build
graphs, ProjectSettings names, or `.tscn`/`.tres` references. Inspect those
files and their native build/parser/registration paths directly. A complete
consumer map may require all of C++ references, ClassDB/XML, `SCsub`, shader
includes, settings, and serialized resources.

Exclude `.git`, `bin/`, `.godot/`, `.sconsign*.dblite`, generated headers,
build output, and `thirdparty/` from project API sweeps.

## Handoff

The chain does not transfer automatically into a delegated context. Paste it
into every code-touching prompt and require the report to name methods used and
source anchors. Do not delegate one symbol lookup merely to run `rg`; use
separate research contexts for independent breadth questions.
