# Remove the fixed microgeometry metadata admission ceiling

Owner approved fixing the editor's `Microgeometry metadata budget exhausted`
failure on 2026-09-10. Baseline: `348d2111b0`. Scope is this admission failure;
the separately deferred performance TODO remains deferred.

`MicroGeometryStorage::acquire` rejects aggregate live plus retired metadata
above64MiB before allocating buffers. The dense scene already uses59.49MiB.
Existing source-identity sharing, buffer allocation/rollback, accounting and
submission-based retirement own the required behavior. Extend those paths.

## Step1 — Correct admission [independent]

Own `servers/rendering/renderer_rd/storage_rd/micro_geometry_storage.{cpp,h}`.
Remove the arbitrary aggregate ceiling; allocate metadata according to actual
asset requirements and existing RenderingDevice limits. Check buffer-size
arithmetic before narrowing and preserve cleanup on allocation failure. Never
publish a partially allocated asset or query invalid buffer RIDs. Preserve
source sharing, statistics, pending-admission behavior and deferred GPU lifetime.
No replacement fixed cap, new setting, import/shader format, page-pool budget,
generic memory manager or editor-specific rendering path is requested.

Compile the ordinary Windows target, then the double target used by the owner
when its running executable can be replaced. Do not close the owner's editor
or lose unsaved work. Use existing native editor/project assets to exercise
more than64MiB of metadata and confirm clean admission and shutdown; no
automated tests, unsupported view modes or performance campaign. At most one
brief code check, no proof/audit workflow. Update the canonical status with
the actual result and limits. This fix changes no public bindings or SCons
source ownership; other backends retain the existing RD allocation contract.
