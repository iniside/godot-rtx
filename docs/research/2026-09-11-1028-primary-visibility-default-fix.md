# Primary visibility default correction

Owner-authorized correction, 2026-09-11; task-start `53e816d65f`.

Normal editor launches still selected raster, while previous Mega Geometry
measurements explicitly set `GODOT_PRIMARY_VISIBILITY=T`. The owner rejected
this setup: the intended renderer must work on an ordinary launch. Double
precision is the primary build, as already recorded in the project authorities.

The named fix removes the private experimental environment selector and
unimplemented hybrid choices. Existing primary surface tracing becomes the
normal RTXDI/DDGI camera path, selected from the actual rendering configuration
and supported capability. No new setting, launcher, environment requirement or
second material renderer is introduced. Full PT remains separately selected;
non-RT rendering keeps its existing contract. Existing material eligibility,
editor composition, canonical surfaces, shadow cadence and geometry quality
remain owned by their current renderer paths.

Implementation owns Forward+ source; parent owns double build and native
launch with no primary-visibility environment variable and no temporary editor
timer/display overrides. Validate actual trace/camera counters, scene loading
and absence of errors through files/logs; no computer use or automated tests.
One brief source review checks exact commit and cumulative task diff. Source commit `eba40da2e8` changes two existing renderer files (+3/-44).
The HYBRID mode is selected after `update_viewport_settings`; existing compositor
preflight requires Vulkan/native RT/CLAS. Double editor build exits 0; native
launch `primary-default-double` runs with no selector and unchanged editor
settings. Fresh brief source review passes exact `eba40da2e8` and cumulative
`53e816d65f..eba40da2e8`; double build exits 0 in38.69 seconds. Native launch
exits 0 after1800 frames. Default mode traces4307778 primary rays at2689x1602,
with4035146 hits and zero unsupported-hit classifications; camera microgeometry
bins are zero. Last-ten GPU median is7.9065ms, profiled FPS95. This is a new
default-settings double run, not a matched comparison with older single builds.

The run is NOT error-free: atframe168, sparse selection requests571872420 active
bytes against536870912 limit (flags51,10000tasks), reported at
`micro_geometry_selection.cpp:278`. Later residency reaches1580pages, zero pending,
25781CLAS and530cuts, but that does not prove the rejected selection reached its
requested detail. Selection-budget correctness remains unresolved; no limit was
raised or error suppressed in this fix. Owner was informed. Scene hash unchanged;
no editor timer/display settings were changed.
