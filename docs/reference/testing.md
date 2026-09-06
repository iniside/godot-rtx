# Testing

Writing, updating, or running automated tests requires an explicit owner
request. Reading tests, test infrastructure, or prior results for research or a
proof audit does not authorize a run or edit.

## Build Precondition

`SConstruct` defaults `tests=no`; the executable must be built with `tests=yes`
before `--test` can provide current proof. A representative Windows build is:

```powershell
scons platform=windows target=editor tests=yes -j16
```

Run the resulting test-enabled binary from this checkout:

```powershell
bin/godot.windows.editor.x86_64.exe --test
```

Add filters or fixtures only from the current test runner's supported options.
Confirm the build succeeded and the expected tests were discovered; a green run
of an old binary or zero selected cases proves nothing.

## Reporting And Proof Boundary

When the owner requests a run, report pass/fail counts, failing test names, and
the first useful error lines, then stop. Do not start a test-fix loop without a
new request.

Headless unit tests do not prove real-device rendering behavior. Ray tracing,
DLSS, shader compilation/permutations, resource barriers, descriptors, and
backend-specific output require a real rendering device on each relevant path.
State precisely which backend, project/scene, configuration, and observable
result were checked.

Tests and fixtures are written only by the `[test-author]` lane and remain a
separate task after the implementation they cover is built. Changes to tests,
fixtures, CI, or executable proof also receive a `proof-auditor` review routed
through `godot-failure-taxonomy.md`.
