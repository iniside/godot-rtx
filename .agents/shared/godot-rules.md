# Godot Rules

## Repository And Upstream - RULE

This is a Godot 4.8-dev C++/SCons fork with NVIDIA ray tracing, DLSS/Streamline,
Aftermath, and bindless work. Read APIs from this checkout. Before changing an
upstream-tracked file, decide whether the change belongs in fork-local code;
when it must touch upstream code, keep the edit narrow so future upstream merges
remain readable.

Never hand-edit `thirdparty/` or generated files such as `*.gen.h`, `*.gen.cpp`,
`modules_enabled.gen.h`, `register_module_types.gen.cpp`, or `*.glsl.gen.h`.
Change the source, build rule, or recorded third-party patch that owns them.

`docs/` holds repository prose. `doc/classes/*.xml` is the upstream Godot class
reference and must remain synchronized with bound APIs.

## C++ And API Contracts - RULE

- Follow `.clang-format`, `.clang-tidy`, and neighboring Godot style. Use
  `snake_case` functions/members, `PascalCase` types, `p_` parameters, `r_`
  output parameters, and `_` prefixes for private/virtual implementations.
- Godot engine code does not use C++ exceptions or raw `new`/`delete`. Use the
  matching Godot ownership type: `memnew`/`memdelete`, `Ref<T>` for
  `RefCounted`, and Godot containers where the surrounding subsystem does.
- Use `ERR_*` macros for fallible runtime paths and `CRASH_*`/`DEV_ASSERT` only
  for genuine invariants. Error exits must leave a state callers can survive.
- Adding or changing a script-facing API requires the complete contract:
  `_bind_methods()`, `doc/classes/*.xml`, class registration when applicable,
  and property/signal/enum/default registration. A `ProjectSettings` key also
  needs registration, default, and property info.
- Editor-only code stays behind `TOOLS_ENABLED`; debug/dev-only behavior stays
  behind its proper define. Runtime/template targets must not depend on editor
  symbols.

## Ownership, Threads, And Rendering - MANDATORY

Name the owner and release path for every `Object`, `Ref<>`, RID, descriptor,
pipeline, acceleration structure, and GPU buffer. Cover early-error and teardown
paths. Do not release a resource while an in-flight frame can still reference it.

Respect main-thread, `RenderingServer` thread, worker-thread, and
`RenderingDevice` ownership. Do not access `Object`/`Node` state off the main
thread or call a rendering API from a thread its contract does not allow.

For shaders and rendering changes, keep C++ bindings, GLSL layouts/defines, and
the SCons include/build graph synchronized. A new `.glsl` needs the appropriate
`SCsub` entry. Check relevant renderer siblings and D3D12/Vulkan/Metal backends,
or record an explicit validated non-applicability.

Headless execution and a successful compile do not prove GPU behavior. Validate
ray tracing, DLSS, synchronization, barriers, descriptors, and shader output on
a real rendering device/backend appropriate to the changed path.

## Scenes, Resources, And Compatibility - MANDATORY

Godot `.tscn`, `.tres`, imported source assets, and project settings are
repository files unless an existing ignore rule says otherwise. Preserve
unrelated content and never stage `.godot/imported`, build output, or caches.

Fork-local and game APIs/data have no compatibility rail: replace them at their
authority and remove the old path in the same step. Stable upstream Godot API
retains upstream compatibility requirements, including binding/XML deprecation
metadata and compatibility methods where current upstream practice requires it.

## Build And Validation - RULE

Representative commands from the repository root:

```powershell
scons platform=windows target=editor -j16
scons platform=windows target=editor dev_build=yes -j16
scons platform=windows target=template_debug -j16
scons platform=windows target=editor compiledb=yes compiledb_gen_only=yes
pre-commit run --files <changed-files>
```

Use only targets/options verified in `SConstruct` and the platform files. Do not
run `scons -c` or delete `.sconsign*.dblite` as a troubleshooting shortcut.
`precision=double` is a separate build axis; validate it only when the change
touches that contract.

Writing, updating, or running automated tests requires explicit owner request.
When requested, follow `docs/reference/testing.md`; test execution requires a
binary built with `tests=yes`. Documentation generation or a real-device render
check is validation, not permission to invent automated tests.
