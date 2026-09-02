# RTX Mega Geometry (CLAS) + camera-relative rendering — research

Data: 2026-09-02 19:04 UTC. Branch `nvidia-pt-dlss`.
Sześć równoległych researchów: nagłówki Vulkan, meshoptimizer, ścieżka RT forka,
audyt transformów world-space, D3D12/NVAPI, stan zewnętrzny (dokumentacja NVIDII,
Khronos, upstream Godot).

Dokument jest **raportem ustaleń**, nie planem. Plan powstaje osobno w `docs/plans/`.

### Skróty ścieżek

Kotwice `plik:linia` są w treści skracane do samej nazwy pliku. Pełne ścieżki:

| Skrót | Ścieżka |
|---|---|
| `render_raytracing.*`, `scene_shader_raytracing.*`, `render_forward_clustered.*` | `servers/rendering/renderer_rd/forward_clustered/` |
| `render_forward_mobile.*` | `servers/rendering/renderer_rd/forward_mobile/` |
| `render_scene_data_rd.*`, `material_storage.*`, `light_storage.cpp`, `texture_storage.cpp`, `particles_storage.cpp`, `mesh_storage.*` | `servers/rendering/renderer_rd/storage_rd/` |
| `gi.cpp`, `fog.cpp`, `sky.cpp` | `servers/rendering/renderer_rd/environment/` |
| `cluster_builder_rd.h` | `servers/rendering/renderer_rd/` |
| `*.glsl` z prefiksem `raytracing_` lub `scene_raytracing_` | `servers/rendering/renderer_rd/shaders/raytracing/` |
| `scene_forward_clustered*.glsl` | `servers/rendering/renderer_rd/shaders/forward_clustered/` |
| `scene_forward_mobile*.glsl` | `servers/rendering/renderer_rd/shaders/forward_mobile/` |
| `rendering_device.*`, `rendering_device_driver.h`, `rendering_device_commons.h` | `servers/rendering/` |
| `rendering_device_driver_d3d12.cpp`, `rendering_shader_container_d3d12.cpp` | `drivers/d3d12/` |
| `renderer_scene_cull.h` | `servers/rendering/` |

---

## 1. Co jest już w drzewie (nic do zdobycia z zewnątrz)

### Vulkan

`thirdparty/vulkan/include/vulkan/vulkan_core.h` ma `VK_HEADER_VERSION 335` i zawiera
komplet obu rozszerzeń, bez gatingu `VK_ENABLE_BETA_EXTENSIONS`:

- `VK_NV_cluster_acceleration_structure`, spec v4 — `vulkan_core.h:23127-23375`
- `VK_NV_partitioned_acceleration_structure`, spec v1 — `vulkan_core.h:23378-23491`
- bonus: `VK_NV_ray_tracing_linear_swept_spheres` — `vulkan_core.h:20675-20726`

Oba są `_NV`-only, nie promowane do core.

Punkty wejścia (dwa na rozszerzenie):

```
vkGetClusterAccelerationStructureBuildSizesNV        vulkan_core.h:23359
vkCmdBuildClusterAccelerationStructureIndirectNV     vulkan_core.h:23360
vkGetPartitionedAccelerationStructuresBuildSizesNV   vulkan_core.h:23475
vkCmdBuildPartitionedAccelerationStructuresNV        vulkan_core.h:23476
```

`thirdparty/volk` ładuje wszystkie cztery w `volkGenLoadDevice`
(`volk.c:1226-1229`, `volk.c:1304-1307`) i w `volkGenLoadDeviceTable`
(`volk.c:2464-2467`, `volk.c:2542-2545`). Ładowanie jest opakowane w
`#if defined(VK_NV_...)` — token definiuje sam nagłówek (`vulkan_core.h:23128`),
więc kompiluje się zawsze. **Ale `load()` zwraca NULL, gdy sterownik nie ma danego
punktu wejścia** — sprawdzenie na NULL w runtime jest po naszej stronie.
Żadnej zmiany w thirdparty nie trzeba.

Limity są **właściwościami urządzenia**, nie stałymi —
`VkPhysicalDeviceClusterAccelerationStructurePropertiesNV` (`vulkan_core.h:23195`)
niesie `maxVerticesPerCluster`, `maxTrianglesPerCluster`, `maxClusterGeometryIndex`
i pięć osobnych wyrównań (`clusterScratchByteAlignment`, `clusterByteAlignment`,
`clusterTemplateByteAlignment`, `clusterBottomLevelByteAlignment`,
`clusterTemplateBoundsByteAlignment`).
Trzeba je odpytać w runtime, nie zaszywać.

Rozszerzenie wymaga też opt-inu na pipelinie:
`VkRayTracingPipelineClusterAccelerationStructureCreateInfoNV`
z `allowClusterAccelerationStructure` (`vulkan_core.h:23353`).

W `drivers/`, `servers/` i `modules/` **nie ma ani jednego odwołania** do tych symboli.

### meshoptimizer

`thirdparty/meshoptimizer` w wersji 1.1. `modules/meshoptimizer/SCsub:14-31` **już
kompiluje** `clusterizer.cpp` i `partition.cpp`, więc dostępne bez zmian w SCons:

- `meshopt_buildMeshletsSpatial` — `meshoptimizer.h:734`, w dokumentacji wprost
  „optimized for raytracing"; to bezpośredni kandydat na generator klastrów
- `meshopt_optimizeMeshlet` — `:743`
- `meshopt_computeMeshletBounds` — `:795`
- `meshopt_partitionClusters` — `:827`, grupowanie klastrów w partycje (mechanizm
  pod hierarchię LOD w stylu Nanite)
- `meshopt_simplifyWithAttributes` + flagi `LockBorder`/`Sparse`/`ErrorAbsolute`/`Prune`

Na dysku, ale **poza buildem**: `meshletcodec.cpp`, `meshletutils.cpp`,
`opacitymap.cpp`. Ich symbole nie linkują się dziś do silnika.

`modules/meshoptimizer/register_types.cpp:42-49` podpina do `SurfaceTool` wyłącznie
funkcje uproszczania/remapu — API meshletów nie jest wołane nigdzie w silniku.

W całym forku (poza `thirdparty/`) nie ma żadnego kodu meshlet/cluster/Nanite.

---

## 2. Ścieżka raytracingu forka

### Kluczowe ustalenie: `gl_PrimitiveID` i brak `gl_GeometryIndexEXT`

Shadery RT mapują trafienie na dane w dwóch krokach:

1. `gl_InstanceCustomIndexEXT` indeksuje **płasko** oba SSBO, `geometries[]`
   i `materials[]` — ten sam indeks do obu. Oba naraz widać w
   `scene_raytracing_raygen.glsl:470-471`+`:509` oraz `raytracing_lights_inc.glsl:171`+`:176`;
   samo `geometries[]` w `raytracing_closest_hit_common_inc.glsl:31-32`
   i `scene_raytracing_raygen.glsl:619-624`.
2. `gl_PrimitiveID` wybiera trójkąt w obrębie tej geometrii —
   `get_triangle_indices_ex()` w `raytracing_hit_inc.glsl:33-61`, wołane
   z `gl_PrimitiveID` w `:65`.

**`gl_GeometryIndexEXT` nie występuje w drzewie shaderów RT ani razu.** Fork zakłada
odwzorowanie 1:1 instancja↔geometria, zakodowane w `instanceCustomIndex`.

`get_triangle_indices_ex` traktuje `primitive_id` jako **ciągły indeks trójkąta
od zera w jednym liniowym buforze indeksów** — dla `index_format==2` (brak indeksów)
liczy `primitive_id*3`, dla UINT16 liczy offset bajtowy `primitive_id*6` z ręcznym
rozpakowaniem dwóch indeksów na słowo, dla UINT32 `primitive_id*3`.

**To jest dokładnie założenie, które łamie CLAS.** W cluster BLAS `primitiveID` jest
względny wobec klastra i wymaga dodatkowej indyrekcji przez cluster ID.

Konsumenci `primitiveID` — lista obejmuje zarówno odczyty `gl_PrimitiveID` wprost,
jak i wywołania `get_triangle_indices*` oraz ray-query produkujące primitive index —
każde z nich trzeba będzie ruszyć osobno.
Sweep po `servers/rendering/renderer_rd/shaders/raytracing/`, z pominięciem `.gen.h`
(wygenerowane kopie tych samych źródeł):

| Miejsce | Znaczenie |
|---|---|
| `raytracing_hit_inc.glsl:65` | jedyny odczyt `gl_PrimitiveID` w ścieżce trójkątowej — wrapper `get_triangle_indices()` |
| `raytracing_hit_inc.glsl:250` | wywołanie wrappera w `fetch_vertex_attributes()` |
| `scene_raytracing_raygen.glsl:474` | wywołanie wrappera w stage `#[any_hit]` (blok od `:409`) |
| `raytracing_closest_hit_common_inc.glsl:182` | wywołanie wrappera dla motion vectorów przy `FLAG_DEFORMED` |
| `raytracing_lights_inc.glsl:173` | `get_triangle_indices_ex()` w `ray_query_alpha_test()` — konsument ID z wiersza niżej |
| `raytracing_closest_hit_common_inc.glsl:508` | `rayQueryGetIntersectionPrimitiveIndexEXT` (inline ray query, DLSS-RR) — producent ID dla wiersza wyżej |
| `scene_raytracing_raygen.glsl:629` | `gl_PrimitiveID` jako indeks AABB w shaderze intersekcji (`*6` floatów), nie trójkąt |

Ścieżka proceduralna (`RT_GEOM_FLAG_PROCEDURAL`) **nie** przechodzi przez
`get_triangle_indices` — czyta atrybuty z `hitAttributeEXT`
(`raytracing_closest_hit_common_inc.glsl:51-65`), a `gl_PrimitiveID` wybiera tam
tylko slot AABB.

### RT_GeometryData

`render_raytracing.h:61-89`, 128 bajtów, `static_assert` na rozmiar. Niesie adresy
buforów (BDA), stride'y, offsety atrybutów, `primitive_count`, `flags`
(`COMPRESSED=1`, `PROCEDURAL=2`, `DEFORMED=4` — `render_raytracing.h:171-176`),
AABB i adres poprzedniej klatki dla motion vectorów.
Lustro GLSL: `raytracing_data_inc.glsl:17-52`, pole w pole.

### Pipeline i SBT

Budowane asynchronicznie per wariant flag RT w `scene_shader_raytracing.cpp`:
ścieżka synchroniczna `:818-892`, pełna asynchroniczna `:1178-1299`.
`HIT_SBT_CAPACITY = 4096` — zdefiniowane **dwa razy**, jako osobne
`static constexpr` lokalne dla funkcji (`:866` i `:1264`).
Stary SBT jest zwalniany dopiero po zbudowaniu nowego (`:1291-1292`, swap `:1299`)
— zgodne z regułą frames-in-flight.

Wszystkie pliki `shaders/raytracing/*.glsl`, `scene_shader_raytracing.*`
i `render_raytracing.*` są **fork-local** (nie istniały w upstream w merge base
`6b1004bbeb`). `rendering_device.*` i sterownik Vulkan są natomiast **patchowanym
upstreamem** — każda linia tam to trwała powierzchnia konfliktów przy merge'u.

### Luka w researchu — DOMKNIĘTA, patrz §8

Pierwszy przebieg nie dostarczył przebiegu sterowania `render_raytracing.cpp`
(tworzenie/przebudowa BLAS per powierzchnia, dirty tracking, skinning/blendshape,
merge multimesh, cząsteczki) ani szczegółów implementacji sterownika Vulkan
(flagi budowania, strategia scratcha, bariery). **Uzupełnione w §8.**

---

## 3. Camera-relative — audyt

### Zakres jest mniejszy, niż zakładałem

Większość podsystemów rasteryzacji **już jest** camera-relative:

- `light_storage.cpp:718` liczy `inverse_transform` i premnaża przez nie **każdą**
  pozycję światła i macierz cienia przed zapisem: mnożenia w `:1063`, `:1184`, `:1200`,
  zapisy w `:836`, `:1064-1066`, `:1186`, `:1202`, `:1221`.
  `:2037` to nie światło, tylko `local_matrix` reflection probe — też view-relative
- `cluster_builder_rd.h` — dwa mnożenia `view_xform * p_transform` (`:249` w `add_light`
  oraz `:382`) zasilają cztery zapisy `store_transform_transposed_3x4` (`:289`, `:332`, `:365`, `:412`)
- dekale: `texture_storage.cpp:4088` liczy `camera_inverse_xform * xform * ...`, zapis w `:4089`
- SDFGI robi to wprost: `gi.cpp:1886` — `pos -= cam_origin; //make pos local to camera,
  to reduce numerical error`

### Ale nie ma jednego mechanizmu, tylko kilka niespójnych

Miejsca, które **nie** są rebasowane:

| Miejsce | Co zapisuje |
|---|---|
| `render_raytracing.cpp:2860-2862` | `RT_LightData::position = xform.origin` — surowo; rasteryzator w analogicznym miejscu zawsze stosuje `inverse_transform` |
| `gi.cpp:1988-1992`, `:2480-2484` | pozycje dynamicznych świateł SDFGI — dwie linijki obok kaskady, która jest rebasowana |
| `gi.cpp:1653-1655` | `push_constant.cam_origin` (debug SDFGI) |
| `fog.cpp:618-620`, `:699`, `:764-767`, `:775` | transform kamery i pozycje fog volume — cała ścieżka mgły nie ma rebasingu w ogóle |
| `particles_storage.cpp:623,871,873,976,1024,1556` | transformy emisji/kolizji cząsteczek |
| `render_raytracing.cpp:2296,2510,2663` | `motion.prev_object_to_world` |

### Gdzie faktycznie następuje truncation w RT

`RendererSceneCull::Instance::transform` (`renderer_scene_cull.h:412`) jest `real_t`,
czyli double przy `precision=double`. Ten **sam obiekt** wędruje przez
`blas_transforms` i `RD::AccelerationStructureInstance::transform`
(`rendering_device.h:1411`, też `Transform3D`) aż do samego dna sterownika:

```
drivers/vulkan/rendering_device_driver_vulkan.cpp:6530-6543  _store_transform_transposed_3x4()
drivers/vulkan/rendering_device_driver_vulkan.cpp:6579       wywołanie z acceleration_structure_instance_write()
```

RT dostaje dokładnie te same dane co rasteryzator i psuje je **później** — dla
transformów instancji TLAS dopiero na granicy sterownika. Dla nich jest to jedno
wąskie gardło.

**Ale to nie jest jedyne miejsce truncation w ścieżce RT** i nie wolno tego uogólniać.
Dwa dalsze siedzą w kodzie fork-local, po stronie CPU, i wymagają osobnej obsługi:

- `render_raytracing.cpp:2296`, `:2510`, `:2663` — `store_transform_transposed_3x4()`
  do `float prev_object_to_world[12]` (`render_raytracing.h:93`, w `RT_InstanceMotionData`); helper ma sygnaturę
  `(const Transform3D &, float *)` — `material_storage.h:388`
- `render_raytracing.cpp:2860-2862` — do `float position[3]` (`render_raytracing.h:127`)

### Istniejąca emulacja split-double

Jedyna odpowiedź upstreamu na double-vs-float. Pisarze CPU:
`render_scene_data_rd.cpp:95-99` i `:284-288` (origin kamery),
`render_forward_clustered.cpp:934-939` (blok `#ifdef` od `:931`)
i `render_forward_mobile.cpp:2115-2120` (origin instancji). Define wchodzi do shaderów w dwóch miejscach:
`render_forward_clustered.cpp:5671-5675`, `render_forward_mobile.cpp:3600`.

Matematyka (`quick_two_sum`/`two_sum`/`double_add_vec3`) jest **zduplikowana**
między clustered a mobile:

```
shaders/scene_data_inc.glsl:22-24
shaders/forward_clustered/scene_forward_clustered.glsl:190-215, 233, 252, 339-346, 422-439, 814, 858
shaders/forward_clustered/scene_forward_clustered_inc.glsl:363
shaders/forward_mobile/scene_forward_mobile.glsl:175-200, 258, 378-385, 446-462, 732, 794
shaders/forward_mobile/scene_forward_mobile_inc.glsl:362
```

Dla 2D ta ścieżka nie istnieje. `precision=double` → `REAL_T_IS_DOUBLE`
w `SConstruct:605-606`, typedef w `core/math/math_defs.h:138-147`.

W całym drzewie nie ma nic o nazwie `camera_relative`/`floating_origin`/`world_rebas`.

---

## 4. D3D12 — pytanie odpada

Raytracing na D3D12 w tym forku **nie istnieje**. Wszystkie wirtuale AS
w `rendering_device_driver_d3d12.cpp:5547-5605` to jednolinijkowe `ERR_FAIL_V_MSG`
(zwracające wartość) albo `ERR_FAIL_MSG` (void), z tym samym komunikatem
"Ray tracing is not currently supported by the D3D12 driver.".
`has_feature` (`:5915-5934`) nie ma `case` dla `SUPPORTS_RAYTRACING_PIPELINE`
ani `SUPPORTS_RAY_QUERY` — leci w `default: return false`, więc warstwa RD odrzuca
wywołania RT jeszcze przed stubami.

Shadery: D3D12 nie używa DXC (nie ma go w drzewie w ogóle), tylko cross-kompiluje
SPIR-V→DXIL przez Mesa NIR. Tablica `SPIRV_TO_MESA_STAGES`
(`rendering_shader_container_d3d12.cpp:356-362`) ma `SHADER_STAGE_MAX` = 10 elementów,
ale tylko 5 inicjalizatorów — stage'e RT (indeksy 5-9) są zero-inicjalizowane
na `MESA_SHADER_VERTEX`. Shader RT zostałby **cicho przetłumaczony jako vertex
shader**, gdyby tam trafił. Dziś nie trafia tylko dzięki bramce `has_feature`.

NVAPI w drzewie to wyłącznie `thirdparty/misc/nvapi_minimal.h` — ręcznie pisany
minimalny nagłówek do ustawień profilu sterownika (DRS), używany przez
`platform/windows/gl_manager_windows_native.cpp` do przełączania threaded
optimization w OpenGL. Zero związku z D3D12 i RT. Prawdziwe SDK NVAPI nie jest
vendorowane (jest na licencji MIT, więc redystrybucja nie byłaby blokerem prawnym —
ale to i tak nie zmienia skali pracy).

Agility SDK 1.618.5 i `directx_headers` z 2025 są wystarczające dla DXR 1.1 —
typy są, tylko nikt ich nie używa.

**Wniosek: ścieżka D3D12 dla Mega Geometry to nie „rozszerzenie", tylko zbudowanie
całego backendu RT od zera plus osobna ścieżka shaderów DXIL. Vulkan-only.**

Dla porządku: Microsoft opublikował spec DXR 2.0 z `D3D12_RAYTRACING_TIER_2_0`,
klastrami i PTLAS jako API vendor-neutralne — ale wg własnej specyfikacji preview
dopiero ~późne lato 2026.

---

## 5. Stan zewnętrzny — rzeczy, które zmieniają plan

### CLAS nie ma refitu

„CLAS can be built, but cannot be updated". Nie ma odpowiednika BLAS refit.
Ścieżka dla geometrii deformowanej to **template + instantiate**:
`BUILD_TRIANGLE_CLUSTER_TEMPLATE_NV` raz (sama topologia), potem co klatkę
`INSTANTIATE_TRIANGLE_CLUSTER_NV` z aktualnym buforem pozycji, a na końcu
`BUILD_CLUSTERS_BOTTOM_LEVEL_NV` składa CLAS-y w cluster BLAS.
CLAS **nie może** trafić bezpośrednio do TLAS.

### CLAS nie przyspiesza trace'owania

`vk_animated_clusters`, RTX 6000 Ada, 2560×1440, 8.43M animowanych trójkątów
(liczby z README sampla nvpro, nie mierzone przez nas):

| Konfiguracja | build/refit | render | total |
|---|---|---|---|
| klasyczny BLAS, 10% rebuild/klatkę | 5.22 ms | 1.39 ms | 6.61 ms |
| klasyczny BLAS, sam refit | 2.78 ms | 1.45 ms | 4.23 ms |
| klastry + templates | **0.80 ms** | 1.52 ms | **2.32 ms** |

Pamięć AS: 564 MB (klasyczny) → 22 MB (cluster BLAS), plus 232 MB danych roboczych
instancjacji.

Render jest *minimalnie gorszy* — dodatkowa indyrekcja kosztuje przepustowość
intersekcji. Cały zysk siedzi w czasie budowy i pamięci. Framing NVIDII:
„the speed of a refit, while retaining most of the flexibility and trace performance
of a full rebuild". Metryka sukcesu to czas budowy AS i VRAM, nie ms/klatkę trace'u.

### Tryby alokacji

`IMPLICIT_DESTINATIONS` (sterownik subalokuje w jednym buforze, limit ~4 GB,
fragmentuje się), `EXPLICIT_DESTINATIONS` (adresy od nas), `COMPUTE_SIZES`
(sam sizing). Konserwatywne estymaty hosta bywają **3-5× większe** niż realny
rozmiar liczony na GPU — `vk_lod_clusters` napisał z tego powodu własny
GPU-driven sparse allocator zamiast ufać sizingowi hosta.
`MOVE_OBJECTS_NV` defragmentuje po fragmentacji trybu implicit.

### PTLAS to natywny mechanizm rebasingu

Partycje niosą osobny wektor translacji, a cytowane zdanie brzmi:
„when the user wishes to re-center the world space coordinates of all objects,
the position translations can be updated efficiently without triggering a rebuild
of the entire Partitioned TLAS".

**Uwaga o źródle:** cytat pochodzi z dokumentu proposala w repo Vulkan-Docs
(`KhronosGroup/Vulkan-Docs/proposals/VK_NV_partitioned_acceleration_structure.adoc`),
nie z pełnej specyfikacji — ta zwracała 403 (§6.4). Twierdzenie „to samo zdanie jest
w specyfikacji DXR" **nie zostało zweryfikowane u źródła**. Ponieważ na tym cytacie
stoi przesunięcie PTLAS w §7, trzeba go potwierdzić przed pisaniem planu.

Czyli camera-relative i PTLAS **nie są niezależnymi etapami** — PTLAS jest
API-owym rozwiązaniem dokładnie tego problemu. NVIDIA używa go tak w NvRTX UE5.6.
Rekomendacja z GDC 2026: 100-1000 instancji na partycję. Jest też „global partition"
dla obiektów zmieniających się co klatkę, nieliczona do `maxPartitionCount`.

### Shadery

`SPV_NV_cluster_acceleration_structure`, capability
`RayTracingClusterAccelerationStructureNV`, builtin **`ClusterIDNV`** (dekoracja 5436)
w `ClosestHitKHR`/`AnyHitKHR`, zwraca −1 dla obiektu nieklastrowego.
Dla inline ray query: `OpRayQueryGetIntersectionClusterIdNV`.

**Niezweryfikowane:** agent twierdzi, że GLSL nie ma natywnego `gl_ClusterIDNV`
i sample sięgają po `GL_EXT_spirv_intrinsics`. Do sprawdzenia w źródle
`vk_lod_clusters/render_raytrace_clusters.rchit.glsl` przed wpisaniem do planu.

### Wymagania

Driver 572.16 dla surowego rozszerzenia (sample nvpro), SDK RTXMG mówi 570+
i ≥10 GB VRAM, Alan Wake 2 w materiałach marketingowych mówi „wszystkie RTX".
Źródła się rozjeżdżają i żadne ich nie godzi. Działa od Turinga; Blackwell ma
dedykowany sprzętowy silnik intersekcji i kompresji klastrów (4. gen RT Core).

### Upstream Godot — zero

PR #66178 (emulacja double, merged 2022) jest **wyłącznie rasteryzacyjny**,
nie wspomina o strukturach akceleracji ani słowem; nie obsługuje też
`skip_vertex_transform`, cząsteczek ani multimesh. Tracker #98655 (precyzja
na dużych odległościach) nie ma pod-issue o RT. Dyskusja #5162 odsyła do #99119,
czyli samego plumbingu RD. **Camera-relative TLAS nie jest w upstreamie ani
zrobiony, ani zgłoszony.**

---

## 6. Otwarte ryzyka (nie do rozstrzygnięcia z dokumentacji)

1. **DLSS Ray Reconstruction a rebasing co klatkę.** Żadne źródło NVIDII tego nie
   opisuje. Streamline ma `sl::Constants.reset` na nieciągłość historii i ASCD
   wnioskujące ją z bazy ortonormalnej kamery, ale nikt nie pisze, czy ciche
   przesunięcie origin ją wymaga. Rozumowanie (nasze, nie z dokumentacji): jeśli
   rebasing stosuje się **spójnie** do danych bieżącej i poprzedniej klatki
   w obrębie jednego przebiegu, motion vectory zostają ciągłe i reset nie jest
   potrzebny. Do przetestowania empirycznie.
2. **Próg kwantyzacji i histereza origin.** Nigdzie nieudokumentowane. Nasza decyzja.
3. **Dostęp do `ClusterIDNV` z GLSL** — patrz wyżej.
4. **Realne wartości limitów/wyrównań CLAS** — pełna specyfikacja Khronosa zwracała
   403; wartości i tak są per-device, do odpytania w runtime.
5. **Luka w mapie lifecycle'u `render_raytracing.cpp`** — patrz §2.

---

## 7. Co z tego wynika dla kolejności prac

Trzy rzeczy przestawiają wcześniejszy szkic etapów:

- **PTLAS awansuje.** Był etapem 5 „dla dużej liczby instancji". Jest natywnym
  mechanizmem rebasingu, więc należy do rozmowy o camera-relative, nie po niej.
- **Skinning nie jest „per-cluster rebuild".** Jest template + instantiate,
  co znaczy inny kształt cache'u i inny moment budowy.
- **D3D12 wypada z zakresu** — nie ma czego rozszerzać.

Plan implementacyjny powstaje osobno, zgodnie z Plan Writing Workflow.

---

## 8. Domknięcie luki — lifecycle RT i warstwy RD/Vulkan

Uzupełnienie luki zgłoszonej w §2. Dwa osobne przebiegi read-only, każdy z regułą
„cytowana linia musi być otwarta i przeczytana".

### 8.1 Przebieg per-frame

Wołane z `render_forward_clustered.cpp`: `build_tlas` (`:2170`) →
`update_uniform_set` (`:2172`), później `register_raytracing_buffer_dependencies`
(`:2562`) wewnątrz `raytracing_list_begin/end`, na końcu `copy_output_texture` (`:2584`).

`build_tlas` (`render_raytracing.cpp:2179-2714`) w kolejności:

1. `prepare_frame()` (`:2189`, def. `:433-531`) — czyści tablice scratch,
   TTL-eviction cache'ów deformowanych i merged-MM, drenaż asynchronicznych kompilacji
   hit-group, `bindless_block` begin-frame.
2. `ensure_pipeline_bundle()` (`:2193`).
3. **Faza 1** (`:2210-2572`) — iteracja `rt_instances`. Proceduralne (`:2245-2313`)
   i zwykłe powierzchnie (`:2420-2571`) trafiają od razu do tablic scratch;
   MultiMesh (`:2318-2418`) tylko rozgrzewa cache i odkłada pracę do
   `pending_mm_surfaces`.
4. **Faza 2** (`:2574-2688`) — `compute_list_begin()` (`:2577`),
   `_build_merged_mm_blas` per odłożona powierzchnia; przy niepowodzeniu fallback
   na „expanded" (jedna instancja TLAS na instancję MM, wspólny BLAS) (`:2623-2671`).
   `finalize_custom_shaders()` (`:2705`), `compute_list_end()` (`:2708`).
5. `build_acceleration_structures()` (`:2710`, def. `:1689-1730`) — buduje/aktualizuje
   zabrudzone BLAS-y, potem TLAS.
6. `finalize_buffers()` (`:2711`, def. `:1732-1760`).

**Barier w tym pliku nie ma.** `register_raytracing_buffer_dependencies`
(`:3219-3264`) deklaruje bufory adresowane przez BDA do render grafu, a ten wstawia
bariery przed `raytracing_list_trace_rays`. Statyczne VB/AB/IB są z tego wyłączone
(komentarz `:3226-3227`) — jednorazowy upload przez transfer worker.

### 8.2 Cache'e BLAS

| Rodzaj | Klucz / miejsce | Trigger przebudowy | Zwolnienie |
|---|---|---|---|
| statyczna powierzchnia | `(mesh_rid.local_index<<8) \| (surface & 0xFF)` (`:559`), `surface_chunks` po 256 wpisów (`:356-373`) | `!ptr \|\| cached_rid_version != mesh_version \|\| cached_counter != invalidation_counter` (`:566-568`) | brak w `cleanup_caches()` — liczy na kaskadowe free RD przy zwolnieniu VB (komentarz `:239-240`) |
| deformowana (`FLAG_DEFORMED`) | `RID_Owner deformed_pool`, uchwyt na powierzchni (`:407-417`) | pełna przebudowa przy zmianie layoutu/wersji/countera (`:658-660`); sam `data_changed` → refit (`:732-734`) | TTL w `prepare_frame` (`:456-484`) + `cleanup_caches` (`:257-282`) |
| MultiMesh merged | `RID_Owner merged_mm_pool` + `mm_handles` (`:419-427`) | `structure_changed` → free+rebuild (`:1857-1871`); `transforms_changed` (z `multimesh_get_last_change`) → re-bake (`:1966-2120`) | TTL `prepare_frame` (`:486-516`), detekcja recyklingu RID (`:1801-1835`), `cleanup_caches` (`:284-310`) |
| proceduralna (AABB) | stan w `RTProceduralState` na instancji (`render_raytracing.h:179-189`) | flaga `dirty`, ustawiana poza tym plikiem | **ścieżka free nieznaleziona w tym pliku** |
| cząsteczki | **nie istnieje** — plik przeczytany w całości, zero kodu | — | — |

`rt_invalidation_counter` (`mesh_storage.h:144`) dostaje świeżą wartość przy tworzeniu
powierzchni (`mesh_storage.cpp:373-375`) i jest inkrementowany w
`mesh_surface_update_*_region` (`mesh_storage.cpp:592`, `608`, `624`, `640`).

**Co leci bezwarunkowo co klatkę:** `tlas_build` (`:1729`), upload czterech SSBO
w `finalize_buffers` (bez sprawdzania zmian), oraz `params_buffer`/`light_buffer`
w `update_uniform_set` (`:3053`, `:3060`). Zawartość BLAS-ów — nie.

### 8.3 Bufor instancji TLAS

`build_acceleration_structures:1719-1726`:

```cpp
inst.id = i;
inst.transform = blas_transforms[i];
inst.blas = blass[i];
inst.flags = BitField<...>(instance_flags[i]);
inst.mask = (i < instance_masks.size()) ? instance_masks[i] : 0xFF;
uint32_t sbt_off = (i < sbt_offsets.size()) ? sbt_offsets[i] : 0;
inst.hit_sbt_range = RD::HitShaderBindingTableRange((1ULL << 32) | uint64_t(sbt_off));
```

`inst.id = i` — czyli `gl_InstanceCustomIndexEXT` to numer w pętli. Wszystkie tablice
(`blass`, `geometry_data`, `material_data`, `sbt_offsets`, `motion_indices`) są
pushowane w tych samych iteracjach, więc `i` zgrywa się 1:1 przez `geometries[]`,
`materials[]` i SBT. **To jest cały mechanizm adresowania — i to on musi się zmienić
przy CLAS.**

`inst.mask` to zawsze `0xFF` (`:2311`, `:2568`, `:2598`, `:2670`) — maskowanie
promieni nie jest dziś używane. TLAS rośnie z podwajaniem, nigdy się nie kurczy
(`:1706-1714`).

### 8.4 Warstwa RD

`blas_create` / `tlas_create` (`rendering_device.cpp:308`, `:474`) **nie idą przez
render graf** — alokują od razu i zwracają RID. `blas_build` / `blas_update` /
`tlas_build` (`:496`, `:517`, `:538`) **idą** — `draw_graph.add_blas_build` (`:509`),
`add_blas_update` (`:530`), `add_tlas_build` (`:646`), z guardami
`ERR_RENDER_THREAD_GUARD_V` i zakazem wywołania przy aktywnej liście draw/compute/RT.

Bufor instancji jest zarządzany na poziomie RD, nie sterownika (`:538-651`):
per-TLAS `LocalVector<InstanceBuffer>`, ring-buforowany per klatka, trwale
zmapowany write-combined (`MEMORY_ALLOCATION_TYPE_CPU`). Instancje są składane
w cieniu CPU i wrzucane jednym `memcpy` — komentarz w kodzie tłumaczy dlaczego:
rozproszone zapisy do WC są o rzędy wielkości wolniejsze.

Deferred free jest oparty na klatkach: `free(RID)` (`:7849-7858`) wrzuca strukturę
na `frames[frame].acceleration_structures_to_dispose_of`, realne zwolnienie
w `:8065-8092`.

### 8.5 Sterownik Vulkan

- **Flagi budowania idą surowo od wołającego** — `build_info.flags = p_flags`
  (`rendering_device_driver_vulkan.cpp:6515`, `:6557`), bity RDD są statycznie
  asertowane jako identyczne z `VkBuildAccelerationStructureFlagBitsKHR` (`:6441`).
  Rozmiary z `vkGetAccelerationStructureBuildSizesKHR` (`:6520`).
- **Scratch jest per-AS, własność RD**, nie pula i nie per-frame
  (`_acceleration_structure_create:6605-6613`): `MAX(buildScratchSize,
  updateScratchSize)` gdy `ALLOW_UPDATE`, plus zapas na wyrównanie. RD tworzy go
  leniwie (`rendering_device.cpp:256-281`), przy za małym wrzuca stary na listę
  utylizacji klatki.
- **Buildy nie są batchowane** — `command_build_blas` (`:6662-6678`),
  `command_update_blas` (`:6680-6696`), `command_build_tlas` (`:6698-6714`) każdy
  robi dokładnie jedno `vkCmdBuildAccelerationStructuresKHR(cmd, 1, ...)`.
- **Bariery liczy render graf**, nie sterownik. Użycia tagowane jako
  `RESOURCE_USAGE_ACCELERATION_STRUCTURE_READ/_READ_WRITE`
  (`rendering_device_graph.cpp:1790`, `:1817`, `:1843`, `:1864`), mapowanie na
  access maski w `:178-204`. Sterownik dostaje je osobnym parametrem i wystawia
  **drugie, oddzielne** `vkCmdPipelineBarrier` na buforze podkładowym AS
  (`rendering_device_driver_vulkan.cpp:3067-3076`, `:3110-3119`).
- **Kompakcji nie ma.** Bit `ALLOW_COMPACTION` przechodzi do sterownika, ale nie ma
  ani zapytania o rozmiar, ani `vkCmdCopyAccelerationStructureKHR` z trybem
  kompaktującym — nigdzie w pliku.
- **`VkRayTracingPipelineCreateInfoKHR.pNext` jest puste** (`:6821-6828`, struktura
  zero-inicjalizowana i nigdy nieprzypisana). Tam wchodzi
  `VkRayTracingPipelineClusterAccelerationStructureCreateInfoNV`.

### 8.6 Gdzie wpiąć CLAS — wzorzec do skopiowania

Rejestracja rozszerzenia: `_initialize_device_extensions` (`:568-635`), wszystkie RT
rejestrowane jako opcjonalne (`false`), np. `VK_KHR_ACCELERATION_STRUCTURE` na `:590`.

Feature działa w dwóch przebiegach:
1. **Query** (`:997-1009`, wynik do pól capabilities w `:1118-1127`) — struktura
   feature jest doczepiana do `next_features` **tylko jeśli rozszerzenie zostało
   włączone**, potem jedno `GetPhysicalDeviceFeatures2`.
2. **Enable** (`:1462-1478`) — lustrzane odbicie, ale bramkowane już na
   *capability*, nie na fladze rozszerzenia; łańcuch ląduje w `VkDeviceCreateInfo.pNext`.

`has_feature` (`:7611-7635`): `SUPPORTS_RAY_QUERY` i `SUPPORTS_RAYTRACING_PIPELINE`
oba wymagają `acceleration_structure_support` jako warunku wstępnego.

BDA: `buffer_get_device_address()` (`:2252-2259`) to jedyna droga do adresu; VMA
dostaje `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` (`:1639-1641`). CLAS jest
w całości sterowany adresami, więc to jest już gotowe.

### 8.7 Co zostało niezweryfikowane w tym przebiegu

- ścieżka zwalniania `RTProceduralState` i jego RID-ów (poza plikiem)
- łańcuch ustawiania `RTProceduralState::dirty` (potwierdzone tylko do
  `renderer_scene_cull.cpp:1122-1141`)
- wnętrza rodziny `hit_sbt_*` (sygnatury i struktury tak, ciała guardów nie)
- które punkty wejścia RT poza `CreateAccelerationStructureKHR` i
  `CreateRaytracingPipelinesKHR` idą przez `device_functions`, a które przez volk
- prowenancja fork vs upstream dla `rendering_device_driver.h` — wnioskowana
  z kształtu API, `git blame` nie uruchomiony
