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

### Luka w researchu

Agent od lifecycle'u nie dostarczył pełnego przebiegu sterowania
`render_raytracing.cpp` (tworzenie/przebudowa BLAS per powierzchnia, dirty tracking,
skinning/blendshape, merge multimesh, cząsteczki) ani szczegółów implementacji
sterownika Vulkan (flagi budowania, strategia scratcha, bariery). **To trzeba
domknąć przed pisaniem planu Etapu 2/4.**

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
