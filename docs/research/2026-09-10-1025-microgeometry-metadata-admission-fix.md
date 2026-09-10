# Microgeometry metadata admission correction

Owner reported `Microgeometry metadata budget exhausted` when opening the
stress scene in the editor. Corrected at `193ce66ab8acf4c06e855cc7c239ff53d699c266`
under the [approved scope](../plans/2026-09-10-1014-microgeometry-metadata-admission-plan.md).

The fixed64MiB aggregate ceiling is removed. Metadata allocation follows actual
asset requirements and the existing RD allocation contract. Buffer sizes use
checked64-bit arithmetic before narrowing to RD's32-bit size; invalid page/group
RIDs are not queried for device addresses. Existing partial-allocation rollback,
source sharing, accounting, admission generations and GPU retirement remain.
Page streaming budgets and shader/import formats are unchanged. Verbose logging
reports successful asset/live/retired metadata bytes once per admission.

Ordinary and double Windows editor builds pass36.17/66.36s. Native ordinary
editor opens `microgeometry_stress/scene.tscn` and admits62,375,288bytes. Native
double editor restores that scene and opens the existing dragon scene, admitting
74,196,976bytes (70.76MiB), above the former64MiB ceiling. Both exit0 without
Godot ERROR or metadata/allocation failures. Existing inline editor shader-parser
warnings remain; this is not a warning-free editor or appearance claim.
Logs: `%TEMP%/godot-render-repair-20260909/metadata-admission-editor-{ordinary,double}.log`.
Both normal and double executables are updated. No automated tests, unsupported
view modes, performance comparisons or memory-exhaustion injection were run.

The single brief source check found no concrete blocker in the checked size,
RID-validity and rollback paths. No additional review or audit is scheduled.
