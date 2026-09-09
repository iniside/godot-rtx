# Microgeometry performance investigation

2026-09-09 UTC. The owner stopped Step 6 correctness/appearance verification and
requested instrumentation and measurements for roughly 34 ms GPU and 60 ms CPU
in `demos/rtxdi_manual/microgeometry/scene.tscn`. Appearance remains the owner's
decision. Step 6 changes are still uncommitted and have not had final review.

## Baseline

Evidence root:
`C:/Users/lukas/AppData/Local/Temp/godot-micro-performance-7dd0711015f641428c05c83a6a93c3c9/`.
The hash-pinned `baseline-bin` snapshot contains the current double editor and
adjacent DLLs; `baseline-binaries.json` and `fixture-inputs.json` record their
identities. The owner closed their editor before measurements. Runs use one Godot
process, RTX 4090/Vulkan, 1280x720, disabled VSync, still camera/objects and normal
rendering. No simultaneous build or screenshot capture ran.

Existing `--gpu-profile` output is flat: stage values are batch means, while the
printed total is the last frame in the batch. The table reports medians of those
reported values, not a mean or percentile of all individual frames. Five startup
batches are discarded, and the frozen run discards five batches after its logged
freeze event. Old console output does not provide CPU phase timings.

| Configuration | Analyzed batches | GPU total, median reported last frame | TLAS Build interval, median batch mean |
| --- | ---: | ---: | ---: |
| Normal, live selection | 35 | 38.210 ms | 31.897 ms |
| Normal, selection frozen after 5 s | 55 | 27.066 ms | 24.817 ms |

Both processes exited 0 without `ERROR:` or `SCRIPT ERROR`. Receipts retain exact
commands, timestamps and executable hashes. `baseline-summary.json` contains
the parsed values and log hashes. Freezing is a diagnostic comparison, not an
optimization proposal. It also changes other intervals, so the difference is
not attributed exclusively to DAG selection.

The broad `TLAS Build` label dominates both configurations. It does not isolate
native TLAS construction from scene gathering, microgeometry selection,
BLAS preparation, feedback or CPU-induced GPU idle time. Finer paired CPU/GPU
markers are being added through the existing native timestamp mechanism before
assigning a cause or changing algorithms.

## Current scope

Instrument streaming, CPU scene preparation, selection, CLAS/BLAS/TLAS,
dependencies, readbacks and waits. Preserve the existing timing mechanism and
render behavior; do not introduce a profiling framework or quality tuning.
Measure the same scene after the instrumentation build, then identify the
dominant GPU and CPU stages. No automated tests are authorized or run.
