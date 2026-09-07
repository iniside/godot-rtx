# Project manager Vulkan startup fix

2026-09-07, baseline `10b3db687373c1e4fcef83e71d9814307a7f3d7d`.
The owner reported `bin/godot.windows.editor.double.x86_64.exe -e` from the
repository root failing because the selected driver was `opengl3`.

`Main::setup` selected Compatibility/OpenGL for the project manager when no
renderer arguments were supplied. History identifies this default in upstream
commit `150d3656db7`. The fork's existing `RenderingServerDefault::_init` rejects
non-Vulkan real drivers. Earlier migration validation supplied explicit Vulkan
arguments and did not cover this entry path.

The local fix in the existing setup function changes the project-manager default
to Vulkan/Forward+ and removes its Compatibility mobile-default override. Explicit
driver arguments and project renderer settings retain their existing handling.
No API, setting, fallback, resource owner or new function is introduced. This is
below the repository's delegation/review threshold. Navigation used root clangd
`find`/`decl`, actual setup/server source and scoped blame.

Both builds passed with `scons platform=windows target=editor accesskit=no
d3d12=no -j16`, adding `precision=double` for the double build. Elapsed times were
24.56 seconds ordinary and 23.50 seconds double. No automated tests were run.

At 15:58 UTC the rebuilt double executable ran from the repository root with
`-e --verbose --quit-after 90`, without rendering-driver/method overrides. The
actual log identifies `Vulkan 1.4.351 - Forward+ - Streamline` on RTX 4090, initializes
editor theme/canvas resources, and exits 0. Neither the reported renderer error
nor the ObjectDB exit warning appears. Existing overlay, unsupported-opcode and
unsupported-material diagnostics remain. This proves bounded startup and orderly
exit, not a separate screenshot/UI interaction or every launch configuration.

Double binary SHA-256:
`791a37f8f3292d43669d777f2347287ac9a289895aa05ea519b2e9e724f9f2eb`.
Its banner contains baseline `10b3db687`; the source fix was present at build time.
Raw stdout/stderr and exact invocation/hash receipt remain in
`%TEMP%/godot-project-manager-vulkan-20260907/double.*`.
