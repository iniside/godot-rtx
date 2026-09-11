# Shadow cascade scheduling status

2026-09-11; source baseline `3bcab4a12b`.
[Authorized plan](../plans/2026-09-11-0946-shadow-cascade-scheduling-plan.md).

Owner requests bounded near/far shadow update cadence and explicitly permits
reducing shadow geometry detail. Camera/RT quality and streaming remain separate.
The 1 GiB geometry pool is a temporary workaround, not evidence of successful
streaming under constrained memory. No computer-use automation, automated tests
or proof auditors are part of this work.

Native Vulkan before capture `shadow-schedule-before-profile` uses the same
saved 10000-instance scene, T mode, 2689x1602 and current 1 GiB pool. It exits 0
without ERROR lines and reaches 12158 resident pages with zero pending pages,
202464 resident CLAS and 120 shared cuts. Last-ten profile medians:

| GPU stage | Before ms | After ms |
| --- | ---: | ---: |
| Whole frame | 7.953 | 6.461 |
| Shadow selection reset | 0.2595 | 0.1111 |
| Shadow DAG traversal | 1.5779 | 0.6415 |
| Shadow cluster emission | 0.1770 | 0.0798 |
| Shadow raster | 1.3397 | 0.3768 |
| Primary surface trace | 1.4505 | 1.3013 |

These replace the older 8.6195 ms capture as the direct baseline for this task;
the source and pool match. GPU medians do not establish end-to-end FPS or visual
equivalence. Matched before unprofiled capture `shadow-schedule-before-noprofile`
exits 0 without ERROR lines; last-ten FPS median is 62. Plan is committed at
`df2e5edf1b`; fresh brief plan review passes.

Implementation `235449ef33` changes six renderer source files. It uses cascade
periods 1/2/4/8 with staggered phases, a populated 10% guard band for reusable
far cascades, retained transforms, updated-rectangle clears and shadow texel
errors 1/2/4/8. Atlas generation, light layout/parameters, camera ownership and
coverage, and accumulated 0.5-degree sun movement can force refresh. Sampled
`ShadowCadence` counters aggregate all frames rather than sampling a single
phase. Ordinary Windows editor build exits 0 in 36.43 seconds; fresh brief
review of exact `235449ef33` and cumulative `3bcab4a12b..235449ef33` passes.

After capture `shadow-schedule-after-profile` exits 0 without ERROR lines.
Per 120 settled frames, cascade refresh counts are exactly 120/60/30/15 and
reuse counts 0/60/90/105, with maximum ages 1/2/4/8 and zero forced refreshes.
The pool reaches 12158 resident pages, zero pending and 202464 resident CLAS.
Whole GPU falls approximately 18.8%; shadow generation stages account for the
work reduction. Independently computed stage medians do not sum to the median
whole frame. The change combines cadence, coarser shadow geometry and expanded
coverage; no isolated attribution between those mechanisms is claimed.

Native scene SHA256 remains
`F276E9A2B9A18B8DE4AE672BF339D7D7925D589E29131EA99A1F61B939A8B19A`;
environment SHA256 remains
`BED94F4E1AD8ECDCDD6F48C0B9811B5F64C4DE79B8FA2C97A2D85C64B4BDCCD5`.

Live sun/camera playback is not available through an existing native file/CLI
route: `main/main.cpp:4431` rejects script main loops; EntitySceneRuntime has no
script bindings, and the existing converter produces a static authored frame.
No new fixture or motion subsystem is added. Moving-sun/camera appearance and
invalidation remain unverified by runtime until exercised interactively by the
owner or via a separately available native playback mechanism.

Artifacts: `C:/Users/lukas/AppData/Local/Temp/godot-render-repair-20260909/`.
Preserved before executable: `bin/godot.shadow_schedule_before.exe`, SHA256
`890B8941DD8F52DD2DC4F1FF79E4626B3C8CE54E121B335A30BAC2E2F4EFE39E`.
After unprofiled capture `shadow-schedule-after-noprofile` exits 0 without errors,
reaches zero pending pages, and reports last-ten median 66 FPS versus 62 before.
All four temporary editor timer/display settings are restored from
`shadow-schedule-editor-settings-original.json` after all Godot processes exit.
