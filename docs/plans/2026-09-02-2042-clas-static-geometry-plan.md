# CLAS na statycznej geometrii — pierwszy pionowy przekrój Mega Geometry

## Context

**Cel:** Sponza renderuje się path-tracingiem przez **cluster BLAS**
(`VK_NV_cluster_acceleration_structure`) zamiast monolitycznego, plus debug view
kolorujący klastry.

**Ograniczenia właściciela:** kompatybilność wsteczna ze starym rendererem, wsparcie
long-term dla meshy innych niż meshlet/microgeometry, oświetlenie inne niż raytracing
i platformy bez raytracingu — nie interesują. „Jak coś ma wylecieć żeby było łatwiej
to wylatuje."

**Poza zakresem:** geometria deformowana, MultiMesh, cząsteczki, AABB proceduralne,
PTLAS, LOD-DAG, camera-relative, D3D12. Zostają na monolitycznym BLAS-ie.

**Klastry pieczone przy imporcie** (decyzja właściciela). `MeshStorage` nie trzyma
kopii CPU — `mesh_get_surface()` robi readback (`mesh_storage.cpp:661-707`).

### Dwa ograniczenia API, które kształtują cały plan

**1. Wierzchołki klastra muszą leżeć ciągle.**
`VkClusterAccelerationStructureBuildTriangleClusterInfoNV` ma `vertexCount:9`
i `triangleCount:9` (`vulkan_core.h:23294-23295`), a indeksy w `indexBuffer`
są **lokalne** — muszą być `< vertexCount`. `meshopt_buildMeshletsSpatial`
(`meshoptimizer.h:734`) zwraca `meshlet_vertices`, tablicę **pośrednią** rozproszonych
indeksów globalnych — nie da się jej wyrazić jako offsetu we współdzielonym buforze.
Indeksy 32-bitowe nie ratują: wymagałyby `vertexCount` całego mesha, a to nie mieści
się w 9 bitach.
→ **Trzeba upiec osobny bufor pozycji z duplikacją per klaster.** Koszt: kilkanaście
procent nadmiarowych wierzchołków na granicach klastrów.

**2. Adres BLAS-a musi znać host.**
`acceleration_structure_instance_write` czyta `blas_info->cached_device_address`
(`rendering_device_driver_vulkan.cpp:6586-6587`), ustawiane z
`buffer_get_device_address()` bufora alokowanego przez hosta (`:6623`), a instancje
TLAS są pisane po stronie hosta (`rendering_device.cpp:641`).
→ **Budowa dolnego poziomu (`BUILD_CLUSTERS_BOTTOM_LEVEL`) musi iść
`EXPLICIT_DESTINATIONS_NV`**, z buforem docelowym alokowanym przez nas.
`IMPLICIT_DESTINATIONS` zostaje tylko dla samych CLAS-ów, gdzie adresy wracają
przez `dstAddressesArray` i konsumuje je GPU, nie host.

### Dlaczego nie rozszerzyć czegoś, co już jest

- **`_populate_surface_blas()`** (`render_raytracing.cpp:914-990`) — to właśnie
  rozszerzamy; gałąź trójkątowa dla statycznych powierzchni zostaje **zastąpiona**.
- **`_build_merged_mm_blas()`** (`:1766-2167`) — inny problem (liczba instancji),
  inne dane (pieczone compute shaderem w runtime). Nie ruszamy.
- **`generate_lods()`** (`importer_mesh.cpp:570-848`) — dokładamy równoległy
  `generate_clusters()`, nie rozszerzamy tamtego.
- **`ARRAY_CUSTOM0-3`** — per-wierzchołek, a dane klastrów są per-powierzchnia.
- **`Object::set_meta`** — per-`Resource`, nie per-surface.
- **`SurfaceData`** — **tego używamy.** Jest już jedyną drogą do `MeshStorage`
  zarówno przy wczytaniu zasobu (`mesh.cpp:1685`, `:1690`), jak i przy
  `mesh_add_surface`. Nie dokładamy równoległego settera.

### Ryzyko, które znam

`ClusterIDNV` (dekoracja SPIR-V 5436) jest udokumentowany dla `ClosestHitKHR`/`AnyHitKHR`,
ale **nie jest potwierdzone, czy GLSL ma natywny `gl_ClusterIDNV`**. Fallback:
`GL_EXT_spirv_intrinsics`. Rozwidlenie wewnątrz Kroku 5, nie brama planu.

---

> Research stojacy za tym planem:
> [docs/research/2026-09-02-1904-megageometry-camera-relative-research.md](../research/2026-09-02-1904-megageometry-camera-relative-research.md)

---

## Kroki

### Krok 1 — Generowanie klastrów przy imporcie `[sonnet]`

**(a)** `modules/meshoptimizer/register_types.cpp` (wskaźniki funkcji, wzorzec `:42-49`),
`scene/resources/surface_tool.h` (`:87-105`), `scene/resources/3d/importer_mesh.{h,cpp}`
(`Surface` + `Vector<uint8_t> cluster_data`, `:50-71`; `generate_clusters()`;
klucz `"clusters"` w `_get_data`/`_set_data`, `:1052-1129`),
`editor/import/3d/resource_importer_scene.cpp` (wywołanie w `:2811-2820`).

`ImporterMesh::add_surface()` (`:99`) **nie dostaje parametru** — jest bindowane
(`importer_mesh.cpp:1560`), a klastry powstają w `generate_clusters()` na już
dodanych powierzchniach.

**Bez opcji importu** — generacja bezwarunkowa dla `PRIMITIVE_TRIANGLES`. Opcja
byłaby przełącznikiem między nową ścieżką a niewidocznością (Krok 4), czyli drugą szyną.

**Nic tu nie umiera.**

**(b)** Wszystko dalej potrzebuje tych danych.

**(c)**
- `meshopt_buildMeshletsSpatial` (`meshoptimizer.h:734`) + `meshopt_optimizeMeshlet`
  (`:743`). `clusterizer.cpp` jest już w buildzie (`modules/meshoptimizer/SCsub:16`),
  **ale `meshopt_optimizeMeshlet` mieszka w `meshletutils.cpp`, którego w liście
  nie ma** — plan pierwotnie twierdził „zero zmian w SCons" i było to błędne;
  wyszło dopiero linkerem (`LNK2019: unresolved external symbol
  meshopt_optimizeMeshlet`). **`meshletutils.cpp` dochodzi do
  `modules/meshoptimizer/SCsub`.**
- `max_vertices = 128`, `min_triangles = 96`, `max_triangles = 128`, `fill_weight = 0.5`.
  Realne limity urządzenia odpytuje Krok 3.
- **Kolejność krytyczna:** po `optimize_indices()` (`resource_importer_scene.cpp:2820`).
  Ono robi cache-optimize (`importer_mesh.cpp:514`), remap LOD-ów (`:517-521`, `:540-544`),
  `optimize_vertex_fetch_remap` (`:534`) i `_remap_arrays` (`:546`) — **zmienia też
  liczbę wierzchołków**.

  **Korekta po review Kroku 1 (commit `a643a15e48`):** pierwotnie napisałem tu, że
  „nic dalej nie przestawia trójkątów". To było błędne — przestawia je **sam
  klasteryzator**. `meshopt_buildMeshletsSpatial` konsumuje ścianki w kolejności
  posortowanej przestrzennie przez BVH (`thirdparty/meshoptimizer/clusterizer.cpp:1339`:
  `unsigned int index = axes[i];`, gdzie `axes` pochodzi z `bvhSplit` w `:1313`).
  Czyli lokalny trójkąt *t* klastra *j* to oryginalna ścianka `axes[prefix + t]` —
  dowolna permutacja, a nie `base_triangle + t`.

  **Dlatego `generate_clusters()` przepisuje `s.arrays[ARRAY_INDEX]` w kolejność
  klastrów**, po czym suma prefiksowa staje się poprawnym globalnym indeksem
  z konstrukcji. Tablice `Surface::LOD::indices` są niezależne i nie są ruszane;
  shadow mesh to osobny `ImporterMesh` i klastrów nie dostaje. Kosztem jest
  utrata uporządkowania pod cache wierzchołków z `optimize_indices()` — akceptowalne,
  bo kolejność klastrowa jest przestrzennie spójna, a rasteryzacja nie jest tu
  priorytetem.
- **Blob (little-endian), dwie sekcje o jawnych bazach:**
  - nagłówek 32 B: `magic` u32, `version` u32, `cluster_count` u32, `total_triangles` u32,
    `index_section_offset` u32, `position_section_offset` u32, `position_vertex_total` u32,
    `_pad` u32
  - `cluster_count` × rekord 16 B: `position_offset` u32, `index_offset` u32,
    `vertex_count` u8, `triangle_count` u8, `_pad` u16, `base_triangle` u32
  - **`position_offset` i `index_offset` są liczone od początku swojej sekcji**, nie od
    początku bloba. Krok 2 rozcina blob na dwa bufory GPU, więc offset bloba-absolutny
    dawałby przesunięcie o rozmiar nagłówka — bez żadnego błędu, tylko zła geometria.
  - sekcja indeksów: u8 × 3 na trójkąt (z `meshlet_triangles`)
  - sekcja pozycji: `float[3]` × `position_vertex_total`, zgromadzone przez
    `meshlet_vertices` z `ARRAY_VERTEX`
- **Pozycje zawsze `R32G32B32_SFLOAT`**, niezależnie od kompresji powierzchni:
  `vertexFormat` w `VkClusterAccelerationStructureTriangleClusterInputNV`
  (`vulkan_core.h:23215-23226`) jest **jeden na całe wejście budowy**, a
  `render_raytracing.cpp:953-960` pokazuje, że mesh bywa `R16G16B16A16_UNORM`.
  Konsekwencja dla transformu instancji jest w Kroku 4 — **to nie jest neutralne**.
- Blob tylko dla `PRIMITIVE_TRIANGLES`; dla reszty pusty.

**(d)** `[sonnet]`.

---

### Krok 2 — Transport bloba na GPU `[sonnet]`

**(a)** `servers/rendering/rendering_server_types.h` (`SurfaceData` +
`Vector<uint8_t> cluster_data`, obok `:83-111`),
`scene/resources/mesh.{h,cpp}` (**niebindowane**
`ArrayMesh::add_surface` `:348` dostaje jeden parametr; `_get_surfaces` `:1512-1571`
i `_set_surfaces` `:1587-1719` — klucz `"cluster_data"`),
`scene/resources/3d/importer_mesh.cpp` (`get_mesh()` `:859-909`),
`servers/rendering/renderer_rd/storage_rd/mesh_storage.{h,cpp}`
(`Mesh::Surface` + `cluster_buffer`, `cluster_position_buffer`, `cluster_count`,
obok `:78-156`; tworzenie w `mesh_add_surface` `:264-523`; **readback
w `mesh_get_surface` `:661-707`**; zwolnienie w `_mesh_surface_clear` `:525-557`).

**Jedne drzwi: `SurfaceData`.** Żadnego osobnego settera na `RenderingServer`, więc
zero nowych wirtuali, zero stubów w `dummy/storage/mesh_storage.h`, zero `FUNC*`
w `rendering_server_default.h`, zero XML-a.

**Bindowanych metod nie ruszamy:** `ArrayMesh::add_surface_from_arrays`
(`mesh.cpp:2304`) i `ImporterMesh::add_surface` (`importer_mesh.cpp:1560`) zostają
z niezmienioną sygnaturą. `ArrayMesh::add_surface` (`mesh.h:348`) jest niebindowane
— dokładamy mu parametry bez kosztu zgodności.

**Co umiera:** nic — kasuje Krok 4.

**(b)** Krok 3 buduje API sterownika; dane muszą już być w buforach GPU.

**(c)**
- Dwa bufory per powierzchnia: `cluster_buffer` (rekordy + indeksy lokalne)
  i `cluster_position_buffer` (pozycje). Blob rozcinany wg
  `index_section_offset`/`position_section_offset` z nagłówka — **rozcina
  `mesh_add_surface`**, jedyny konsument bloba.
- Oba bufory z `buffer_flags` z `mesh_storage.cpp:381-386`
  (`ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT` + `DEVICE_ADDRESS_BIT`
  + `AS_STORAGE_BIT`). **`index_buffer` już te flagi ma** (`:426`, LOD-y `:436`) —
  nic tam nie zmieniamy.
- **Trasa danych, kompletna:**
  1. import → `ImporterMesh::Surface::cluster_data` (Krok 1)
  2. `ImporterMesh::get_mesh()` (`:859-909`) buduje `SurfaceData` przez
     `RS::mesh_create_surface_data_from_arrays`, dopisuje `cluster_data`/`cluster_count`
     i woła `ArrayMesh::add_surface(...)` (`mesh.h:348`) zamiast
     `add_surface_from_arrays` (`:891`). `add_surface_from_arrays` normalnie rozbija
     `SurfaceData` na płaskie argumenty (`mesh.cpp:1839`) i wszystko spoza listy gubi —
     dlatego omijamy je tylko tutaj, nie zmieniając go dla nikogo innego.
  3. zapis: `_get_surfaces` (`:1512`) czyta przez `RS::mesh_get_surface`, więc
     **`MeshStorage::mesh_get_surface` musi robić `buffer_get_data`** na obu buforach
     i skleić blob z powrotem; klucze `"cluster_data"`/`"cluster_count"` do Dictionary
  4. wczytanie: `_set_surfaces` (`:1587`) odczytuje te klucze do `SurfaceData`
     i idzie istniejącą drogą — `mesh_add_surface` (`mesh.cpp:1685`) na gałęzi
     aktualizacji albo `mesh_create_from_surfaces` (`:1690`) na pierwszym wczytaniu.
     **Obie gałęzie działają bez zmian**, bo obie biorą `SurfaceData`.
- `rt_invalidation_counter` (`mesh_storage.h:144`) — nowa powierzchnia i tak dostaje
  świeżą wartość w `mesh_add_surface` (`:373-375`); osobny bump nie jest potrzebny,
  bo nie ma ścieżki aktualizacji samych klastrów.
- **Bez flagi formatu.** Pierwotnie plan rezerwował `ARRAY_FLAG_HAS_CLUSTER_DATA`
  na bicie 34. Review Kroku 2 wykazał, że nikt jej nie ustawia ani nie czyta —
  obecność klastrów sygnalizuje `cluster_data.size()` we wszystkich trzech
  konsumentach. Bit zaparkowany „na później" to dokładnie ten kształt, który
  odrzuca *No Backward Compatibility*. **Flaga usunięta.**
- **Blob jest jedynym źródłem prawdy o liczbie klastrów.** Pierwotnie plan kładł
  `cluster_count` również w `SurfaceData`, w kluczu Dictionary i w parametrze
  `add_surface` — przyjmowane na słowo, bez cross-checka. Zapisany zasób z poprawnym
  blobem i `"cluster_count" = 0x7FFFFFFF` dałby iterację po dwóch miliardach
  16-bajtowych rekordów nad 200-bajtowym buforem. **Pole usunięte**;
  `mesh_add_surface` dekoduje offset 8 zwalidowanego nagłówka.
- **Walidacja nagłówka przed `memnew(Mesh::Surface)`**, nie po — inaczej każde
  `ERR_FAIL` na złym blobie przecieka `s` i sześć RID-ów utworzonych wcześniej.
  Walidacja obejmuje `index_section_offset == 32 + cluster_count * 16`, bez czego
  spreparowany nagłówek wskazuje rekordy poza buforem.

**(d)** `[sonnet]`.

---

### Krok 3 — API klastrów w RD/RDD + Vulkan `[opus]`

**(a)** `servers/rendering/rendering_device_driver.h` (`:793-845`),
`servers/rendering/rendering_device.{h,cpp}` (obok `blas_create` `:308`, `blas_build` `:496`),
`drivers/vulkan/rendering_device_driver_vulkan.{h,cpp}` (rejestracja `:568-635`,
query `:997-1009` + `:1118-1127`, enable `:1462-1478`, pipeline `:6821-6828`,
`_acceleration_structure_create` `:6600-6625`),
`drivers/d3d12/rendering_device_driver_d3d12.{h,cpp}` (stuby w stylu `:5547-5605`),
`servers/rendering/rendering_device_graph.{h,cpp}` (`:178-204`, `:1790`/`:1817`/`:1843`).

**(b)** Krok 4 nie ma czego wołać.

**(c)**
- **Dwie różne operacje, dwa różne tryby** (patrz Context 2):
  - CLAS-y: `opType = BUILD_TRIANGLE_CLUSTER_NV`, `opMode = IMPLICIT_DESTINATIONS_NV`,
    adresy wracają przez `dstAddressesArray` i konsumuje je GPU.
  - BLAS: `opType = BUILD_CLUSTERS_BOTTOM_LEVEL_NV`, **`opMode = EXPLICIT_DESTINATIONS_NV`**,
    bufor docelowy alokujemy my, a jego `buffer_get_device_address()` trafia do
    `cached_device_address` — dokładnie tak, jak robi to `_acceleration_structure_create`
    (`:6623`).
- `ClusterBuildInput` odwzorowuje **`VkClusterAccelerationStructureInputInfoNV`**
  (`vulkan_core.h:23241-23249`), nie tylko unię: `maxAccelerationStructureCount`
  (liczba CLAS-ów — obowiązkowe, bez domyślnej), `flags`, `opType`, `opMode`,
  plus `opInput` = pełne `VkClusterAccelerationStructureTriangleClusterInputNV`
  (`:23215-23226`): `vertexFormat`, `maxGeometryIndexValue`,
  `maxClusterUniqueGeometryCount`, `maxClusterTriangleCount`, `maxClusterVertexCount`,
  `maxTotalTriangleCount`, `maxTotalVertexCount`, `minPositionTruncateBitCount`.
- Wirtuale RDD — **regiony przekazujemy jako adres+stride+rozmiar**, bo
  `VkClusterAccelerationStructureCommandsInfoNV` (`:23258-23271`) używa
  `VkStridedDeviceAddressRegionKHR`; samo `BufferID` zmuszałoby sterownik do
  zgadywania stride'u:
  ```cpp
  struct ClusterAddressRegion { BufferID buffer; uint64_t offset, stride, size; };

  virtual bool clas_is_supported() = 0;
  virtual ClusterAccelerationStructureLimits clas_get_limits() = 0;
  virtual void clas_get_build_sizes(const ClusterBuildInput &, ClusterBuildSizes &r) = 0;
  virtual void command_build_clas(CommandBufferID, const ClusterBuildInput &,
          BufferID p_dst_implicit,
          const ClusterAddressRegion &p_dst_addresses,   // WYJŚCIE: adresy CLAS-ów
          const ClusterAddressRegion &p_dst_sizes,
          BufferID p_scratch,
          const ClusterAddressRegion &p_src_infos,
          BufferID p_src_infos_count) = 0;               // VkDeviceAddress, nie uint32
  virtual AccelerationStructureID blas_create_from_clusters(uint32_t p_max_clusters,
          uint32_t p_max_clusters_per_as) = 0;
  virtual void command_build_blas_from_clusters(CommandBufferID, AccelerationStructureID,
          BufferID p_scratch, const ClusterAddressRegion &p_cluster_addresses,
          BufferID p_count) = 0;
  ```
- **Dwa osobne zapytania o rozmiar.** `clas_get_build_sizes` obsługuje wejście
  trójkątowe; `blas_create_from_clusters` woła to samo
  `vkGetClusterAccelerationStructureBuildSizesNV`, ale z
  `VkClusterAccelerationStructureClustersBottomLevelInputNV`
  (`maxTotalClusterCount`, `maxClusterCountPerAccelerationStructure`) — stąd drugi
  parametr w jego sygnaturze.
- Rekord per klaster (wypełnia Krok 4) to pełne
  `VkClusterAccelerationStructureBuildTriangleClusterInfoNV` (`:23292-23310`):
  `clusterID`, `clusterFlags`, `triangleCount`, `vertexCount`,
  `positionTruncateBitCount` (0), `indexType` (8-bit), `opacityMicromapIndexType` (0),
  `baseGeometryIndexAndGeometryFlags`, `indexBufferStride`, `vertexBufferStride` (12),
  `geometryIndexAndFlagsBufferStride` (0), `opacityMicromapIndexBufferStride` (0),
  `indexBuffer`, `vertexBuffer`, reszta adresów zerowa.
- **Limity odpytujemy** —
  `VkPhysicalDeviceClusterAccelerationStructurePropertiesNV` (`:23195`):
  `maxVerticesPerCluster`, `maxTrianglesPerCluster`, `maxClusterGeometryIndex`
  i **pięć** wyrównań, wszystkie do `ClusterAccelerationStructureLimits`.
- Rejestracja wzorcem z `:590`. Feature dwuprzebiegowo: query bramkowany flagą
  rozszerzenia (`:997-1009`), enable bramkowany capability (`:1462-1478`).
  volk ładuje punkty wejścia (`volk.c:1226-1229`), **ale wskaźniki są NULL bez
  rozszerzenia w sterowniku** — `clas_is_supported()` sprawdza capability **i**
  niezerowość wskaźników.
- `VkRayTracingPipelineCreateInfoKHR.pNext` (`:6821-6828`) jest puste; wchodzi tam
  `VkRayTracingPipelineClusterAccelerationStructureCreateInfoNV`
  z `allowClusterAccelerationStructure = VK_TRUE`.
- **Scratch idzie przez istniejące odroczenie.**
  `_acceleration_structure_scratch_buffer_create` (`rendering_device.cpp:256-281`)
  odracza utylizację przez `frames[frame].buffers_to_dispose_of` (`:266`). Scratch
  CLAS-ów jest per-powierzchnia i tworzony w Kroku 4, ale **musi używać tej samej
  listy** — zwolnienie w locie bufora czytanego przez klatkę in-flight to crash.
- **Bariery liczy render graf.** Nowe użycie obok
  `RESOURCE_USAGE_ACCELERATION_STRUCTURE_READ_WRITE`, stage
  `PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT`. Budowa BLAS-a czyta bufor adresów
  zapisany przez budowę CLAS-ów — krawędź zależności, bez której jest wyścig bez komunikatu.
- **Integracja z TLAS-em jest realna, nie deklaratywna.** BLAS z klastrów zwraca zwykłe
  `AccelerationStructureID`, ale RD-owy obiekt `AccelerationStructure` niesie
  `scratch_buffer`, `draw_tracker`, `untracked_buffers`,
  `acceleration_structure_dependencies` i flagę `invalidated`, zarządzane przez
  `blas_build`/`_blas_remove_tlas_dependencies` (`rendering_device.cpp:282-305`,
  `:496-514`). Ścieżka klastrowa wypełnia je równoważnie i ustawia
  `cached_device_address` z bufora `EXPLICIT_DESTINATIONS` — dopiero wtedy
  `acceleration_structure_instance_write` (`:6584-6590`) działa na niej tak samo.
- **D3D12: stuby `ERR_FAIL_*`** w stylu `:5547-5605`.
- API niebindowane — `doc/classes` nie dotyczy.

**(d)** `[opus]`.

---

### Krok 4 — Cluster BLAS w rendererze; **kasacja monolitycznej ścieżki statycznej** `[opus]`

**(a)** `servers/rendering/renderer_rd/forward_clustered/render_raytracing.{h,cpp}`,
`servers/rendering/renderer_rd/shaders/raytracing/raytracing_data_inc.glsl`
(lustro `RT_GeometryData`), `.../raytracing_inc.glsl` (lustro flag, `:144`).

**Co umiera, w tym samym kroku:** gałąź `TYPE_TRIANGLES` w `_populate_surface_blas`
(`:964-980`) i `blas_create()` (`:982`) **dla ścieżki statycznej**. Uwaga dla
implementującego: `_populate_surface_blas` jest **współdzielone** — parametr
`p_vertex_buffer_override` wybiera ścieżkę deformowaną, która zostaje. Nie kasować
całej funkcji. Bez flagi projektowej, bez „usuniemy jak się sprawdzi".

**(b)** Krok 3 dał API; to jedyny konsument. Kasacja tutaj, bo dwa źródła prawdy dla
BLAS-a statycznej powierzchni są gorsze niż zepsuty stan pośredni.

**(c)**
- `RTSurfaceData` + `clas_buffer`, `clas_addresses_buffer`, `cluster_remap_buffer`,
  `clas_count_buffer` (4 bajty, `srcInfosCount` — **tylko dla `clas_build`**;
  budowa dolnego poziomu już go nie przyjmuje, sterownik trzyma tam literalne `1`),
  `cluster_count`.
  **`blas_dst_buffer` nie istnieje** — bufor docelowy dolnego poziomu jest
  własnością sterownika, alokowany w `blas_create_from_clusters` i zwalniany
  kaskadowo przez `acceleration_structure_free` (Krok 3).
- **`aabb_transform` NIE wolno stosować do powierzchni klastrowych.** Dziś dla
  skompresowanej powierzchni BLAS jest budowany z pozycji `R16G16B16A16_UNORM`
  w znormalizowanej przestrzeni AABB, a transform instancji to kompensuje:
  `aabb_transform` liczone w `:780-784`, aplikowane w `:2491-2493`, `:2506`,
  `:2646-2648`, `:2658-2660`. Nasze pozycje klastrowe są float32 w przestrzeni mesha
  (Krok 1), więc kompensacja przestaje pasować. glTF ustawia
  `ARRAY_FLAG_COMPRESS_ATTRIBUTES` domyślnie (`modules/gltf/gltf_document.cpp:1433`),
  czyli dotyczy **całej Sponzy** — nietknięte, wszystko wyląduje w złej skali i miejscu.
  **We wszystkich czterech miejscach transform instancji dla powierzchni klastrowej
  pomija `aabb_transform`.** `is_compressed` zostaje bez zmian, bo shader nadal
  dekoduje **atrybuty** przez `FLAG_COMPRESSED` i `get_aabb_compression_xforms` —
  to dwie różne rzeczy i tu się rozjeżdżają.
- **Zwalnianie — najłatwiejszy wyciek w planie.** Statyczne BLAS-y nie są dziś
  zwalniane w `cleanup_caches()`, bo `blas_create` rejestruje zależność od
  vertex/index buffera (`rendering_device.cpp:434`, `:436`) i RD kaskaduje —
  opisuje to komentarz `render_raytracing.cpp:239-240`.
  **`blas_create_from_clusters` nie rejestruje żadnej zależności**, więc:
  - jawnie zwalniamy **pięć** rzeczy: cztery nowe bufory **plus sam BLAS**;
  - miejsca: `cleanup_caches()` i gałąź re-populacji `:581-588`. **TTL dla statycznych
    powierzchni nie istnieje** — jest tylko dla deformowanych (`:455-464`)
    i merged-MM (`:486-495`);
  - **warunek wersji w `:583` (`if (entry->cached_rid_version == mesh_version)`)
    znika, zwolnienie staje się bezwarunkowe.** Ten warunek istnieje wyłącznie
    dlatego, że przy niezgodności wersji liczono na kaskadę RD. Bez kaskady
    zostawienie go to wyciek na całe życie procesu;
  - komentarze `:239-240` i `:587` („BLAS already cascade-freed by RD") stają się
    nieprawdą i **muszą zostać przepisane w tym samym kroku** — komentarz twierdzący
    coś, czego kod nie robi, to defekt.
- Budowa, w miejscu skasowanej gałęzi:
  1. adresy urządzenia `cluster_buffer` i `cluster_position_buffer` (Krok 2),
  2. wypełnienie `srcInfos` rekordami per klaster;
     `vertexBuffer` = adres `cluster_position_buffer` + `position_offset`,
     `indexBuffer` = adres `cluster_buffer` + **`cluster_index_section_offset`**
     + `index_offset`.
     **Podział na dwa bufory jest niesymetryczny.** `cluster_position_buffer`
     zaczyna się dokładnie na sekcji pozycji, więc `position_offset` działa wprost.
     Sekcja indeksów leży **wewnątrz** `cluster_buffer`, za nagłówkiem i rekordami,
     więc trzeba dodać `cluster_index_section_offset` z `Mesh::Surface` (Krok 2).
     Pominięcie tego daje złą geometrię bez żadnego błędu,
  3. `clas_get_build_sizes` → alokacja `clas_buffer`, `clas_addresses_buffer`, scratch,
  4. `command_build_clas` (`IMPLICIT_DESTINATIONS`),
  5. `blas_create_from_clusters` (sterownik alokuje docelowy bufor sam), potem
     `blas_build_from_clusters` karmione samym `clas_addresses_buffer`.
     **BLAS z klastrów można zbudować tylko raz** — drugie wywołanie to `ERR_FAIL`.
     Gałąź re-populacji musi zwolnić BLAS i utworzyć nowy, co i tak robi.
     `p_dst_sizes` jest opcjonalne — przekazujemy `{}`, bo per-CLAS rozmiary
     nie są nam do niczego potrzebne.
     Każdy region musi spełniać `size % stride == 0`.
- **`cluster_remap_buffer`**: `cluster_count` × u32 = `base_triangle`. Tyle shader
  potrzebuje, by z pary (klaster, lokalny trójkąt) wrócić do globalnego indeksu.
- `RT_GeometryData` (`render_raytracing.h:61-89`) ma `uint32_t _pad[5]` (`:87`).
  Dwa słowa na `cluster_remap_address_lo/hi`, jedno na `cluster_count`, zostają dwa.
  **128 B i `static_assert` (`:89`) bez zmian.** Struktura wgrywana hurtowo
  (`:1752-1753`, bind `:2957`) — nie ma packera do poprawiania. Lustro GLSL
  (`raytracing_data_inc.glsl:17-52`, `_pad[5]` w `:51`) aktualizujemy **w tym samym
  kroku**: rozjazd to cicha korupcja danych, nie błąd kompilacji.
- Nowa flaga `RT_GEOM_FLAG_CLUSTERED` obok `:171-176` **plus jej lustro
  w `raytracing_inc.glsl:144`** (gdzie mieszka `FLAG_DEFORMED`). Krok 5 na niej
  gałęzi — bez lustra shader jej nie zobaczy.
- `cluster_remap_buffer` czytany po BDA, ale tworzony raz z danymi początkowymi,
  więc wg komentarza `:3226-3227` **nie potrzebuje wpisu**
  w `register_raytracing_buffer_dependencies` (`:3219`) — tak jak statyczne VB/AB/IB.
- Powierzchnia bez klastrów **nie renderuje się**, `ERR_PRINT` raz. Cichy fallback
  byłby drugą szyną.

**(d)** `[opus]`.

---

### Krok 5 — Ścieżka klastrowa w shaderach trafień `[opus]`

**(a)** `raytracing_hit_inc.glsl` (`get_triangle_indices_ex` `:33-61`, wrapper `:65`),
`raytracing_data_inc.glsl` (`GeometryData` `:17-52`, nowa deklaracja `buffer_reference`
obok `:7-12`), `raytracing_lights_inc.glsl` (`:173`),
`raytracing_closest_hit_common_inc.glsl` (`:508`).

**Bez nowego bitu `RT_FLAG_*`** — ścieżka klastrowa kompiluje się bezwarunkowo.
Nie interesują nas GPU z RT bez tego rozszerzenia, a wariant nieklastrowy nie miałby
czego rysować.

**(b)** Bez tego Krok 4 renderuje śmieci — `primitiveID` z cluster BLAS-a jest
względny wobec klastra.

**(c)**
- **Wrapper `get_triangle_indices` (`:65`) gałęzi na `geom.flags & FLAG_CLUSTERED`:**
  ```
  clustered:     global_prim = cluster_remap[cluster_id] + gl_PrimitiveID
  nie-clustered: global_prim = gl_PrimitiveID
  ```
  **Gałąź jest obowiązkowa, nie optymalizacja.** Geometria deformowana i AABB zostają
  monolityczne (Krok 4), a `raytracing_closest_hit_common_inc.glsl:182` — wewnątrz
  `if ((geom.flags & FLAG_DEFORMED) != 0u)` otwartego w `:178` — idzie przez ten sam
  wrapper. Bez gałęzi meshe deformowane remapowałyby się po śmieciowym ID klastra
  i trafiały w losowe trójkąty.
- Dalej istniejąca logika `get_triangle_indices_ex` na `global_prim`. Cały łańcuch
  atrybutów (`fetch_uv` `:71-112`, `fetch_color` `:117-131`, `fetch_tbn` `:196-226`)
  pracuje na `(i0,i1,i2)` i **nie wymaga zmian**.
- **Realnych miejsc są dwa.** `raytracing_hit_inc.glsl:250`,
  `scene_raytracing_raygen.glsl:474` i `raytracing_closest_hit_common_inc.glsl:182`
  idą przez ten jeden wrapper — poprawka wrappera załatwia wszystkie trzy. Osobno
  zostaje **para ray-query**: `raytracing_lights_inc.glsl:173` i jej producent
  `raytracing_closest_hit_common_inc.glsl:508`.
- **`scene_raytracing_raygen.glsl:629` — nie dotykać.** Tam `gl_PrimitiveID` to indeks
  AABB w shaderze intersekcji.
- Ray query potrzebuje `OpRayQueryGetIntersectionClusterIdNV`, nie tego samego builtinu
  co closest hit. Jeśli nieosiągalny z GLSL, alpha-test w ray query dla powierzchni
  klastrowych degraduje się do „traktuj jako nieprzezroczyste" — z jawnym komentarzem,
  bo to zmiana zachowania.
- `#extension GL_EXT_buffer_reference : require` już jest
  (`scene_raytracing_raygen.glsl:236-237`, `:417-418`, `:528-529`).
- ID klastra: najpierw natywna składnia; przy błędzie kompilacji fallback na
  `GL_EXT_spirv_intrinsics` ze `spirv_decorate` na builtinie 5436.
- Nowy plik `.glsl` nie wymaga wpisu w `SCsub` (glob) — i tak go nie dodajemy.

**(d)** `[opus]`.

---

### Krok 6 — Debug view „cluster ID" `[sonnet]`

**(a)** `scene/resources/environment.h` (`RT_DEBUG_CLUSTER_ID` przed `RT_DEBUG_MAX`,
`:80-104` — 23 stałe 0-22, nowa dostaje 23), `environment.cpp` (`PROPERTY_HINT_ENUM`,
`:1507`), `doc/classes/Environment.xml` (**bindowane API — XML obowiązkowy**;
dziś udokumentowane do `RT_DEBUG_BRDF_REJECTION value="22"`, `:549`),
`raytracing_closest_hit_common_inc.glsl` (`debug_visualize()` `:270-433`),
`scene_raytracing_raygen.glsl` (oba wywołania, `:348` i `:399`).

**(b)** To cały mechanizm weryfikacji tego punktu.

**(c)** `debug_visualize()` bierze 15 skalarnych parametrów i **nie ma dostępu do ID
klastra** — dlatego oba wywołania są na liście plików. Prościej: gałąź `vis_mode == 23`
czyta builtin klastra **bezpośrednio wewnątrz include'a closest-hit**, gdzie jest
dostępny, zamiast przewlekać szesnasty parametr przez dwa call site'y. Implementujący
wybiera to rozwiązanie, chyba że builtin okaże się tam nieosiągalny.
Kolor: hash rozpraszający bity, **nie gradient** — gradient na sąsiednich indeksach
wygląda jak jednolita powierzchnia i nie odróżni „klastry działają" od „jest jeden klaster".

**(d)** `[sonnet]`.

---

### Krok 7 — Testy generowania klastrów `[test-author]`, `model:"sonnet"`

Pokrywa **Krok 1** (zacommitowany). Reszta jest nieosiągalna z zestawu headless —
`--test` nie ma prawdziwego `RenderingDevice`. Moduły **są** inicjalizowane
w `Main::test_setup()` (`main/main.cpp:840`; `--test` nie przechodzi przez `setup2()`,
routing w `:993-1000`), więc wskaźniki funkcji meshoptimizera będą ustawione.

Test jednostkowy bez `[SceneTree]`. Asercje:

0. `cluster_count > 0` — **bez tego test przechodzi próżno** na pustym blobie
1. suma `triangle_count` == liczba trójkątów powierzchni
2. **dla każdego klastra *j* i lokalnego trójkąta *t*: przepisany bufor indeksów
   pod `3*(base_triangle_j + t)` równa się trójkątowi zrekonstruowanemu z klastra**
   (`meshlet_vertices[vertex_offset + meshlet_triangles[...]]`).
   Sama asercja „`base_triangle` pokrywają zakres bez dziur i nakładek" przechodzi
   **próżno** na sumie prefiksowej i nie wykryje permutacji z `clusterizer.cpp:1339` —
   to musi być porównanie z faktyczną zawartością bufora indeksów
3. `vertex_count`/`triangle_count` w limitach
4. sekcja pozycji ma dokładnie `position_vertex_total × 12` bajtów, każdy indeks
   lokalny `< vertex_count` swojego klastra, a `position_offset`/`index_offset`
   mieszczą się w swojej sekcji — niezmienniki wymuszone przez API (Context 1),
   złamane dają błąd walidacji dopiero na GPU
5. round-trip `_get_data`/`_set_data` zachowuje blob bajt w bajt
6. powierzchnia nietrójkątna → pusty blob

(2) i (4) to gałęzie, które przy cichej pomyłce dają trafienia w losowy trójkąt —
objaw wyglądający jak błąd shadera, diagnozowany godzinami.

---

## Weryfikacja

1. `scons platform=windows target=editor -j16` — zielony.
   **Do uruchomienia testów potrzebny jest `tests=yes`** — zwykły `target=editor`
   po cichu pomija drzewo `tests/` (`SConstruct:1278`), więc `--test` nie zobaczyłby
   zestawu z Kroku 7 w ogóle.
2. `scons platform=windows target=template_debug` — dowodzi, że `#ifdef`-y edytorowe
   nie wyciekły.
3. `scons platform=windows target=editor tests=yes -j16`, potem
   `bin/godot.windows.editor.x86_64.exe --headless --test` — Krok 7.
4. `... --headless --path rt_test_scenes --import` — re-import Sponzy; w logu liczba
   klastrów per powierzchnia.
5. `... -e --path rt_test_scenes res://sponza_pt.tscn` — Sponza w path tracingu.
   **Kryterium: obraz nieodróżnialny od tego sprzed zmiany.** Cała geometria przeszła
   na cluster BLAS, więc identyczny obraz dowodzi poprawności remapu `primitiveID`;
   błąd objawia się pomieszanymi UV, normalnymi albo materiałami, a pomyłka
   w `aabb_transform` (Krok 4) — złą skalą i pozycją całych meshy.
   **Scena jest zdatna:** wyłącznie `Node3D`, `WorldEnvironment`, `DirectionalLight3D`,
   `Camera3D` i instancja glTF; zero `PrimitiveMesh` i zero meshy budowanych w runtime.
6. `F1` cykluje `pathtracing_debug_mode`; tryb 23 pokazuje łaty rzędu 128 trójkątów.
   Jednolity kolor = jeden klaster = generacja nie zadziałała.
7. PIX/Nsight: czas budowy AS i pamięć AS przed i po. Oczekiwane: mniej pamięci,
   czas trace'u bez zmian lub minimalnie gorszy. **To nie jest optymalizacja czasu
   klatki** i nie należy jej tak mierzyć.

## Znane ograniczenie po tym punkcie

Każdy mesh **nie z importu** przestaje być widoczny w path tracingu: `PrimitiveMesh`
(Box/Sphere/Plane), `ImmediateMesh`, wyjście `SurfaceTool`, CSG,
`scene/3d/physics/soft_body_3d.cpp:507`, `mesh_instance_3d.cpp:696`/`:862`.
Scena testowa żadnego nie zawiera, więc nie blokuje — ale pierwszy `BoxMesh` wstawiony
w edytorze zniknie. Domknięcie to osobny punkt.

## Czego ten punkt nie robi

Deformowana geometria, MultiMesh, cząsteczki i AABB zostają na monolitycznym BLAS-ie.
Bez templates i instancjacji (odpowiedź na animację — kolejny punkt). Bez PTLAS,
bez LOD-DAG, bez camera-relative. Bez własnego alokatora CLAS-ów. D3D12 dostaje stuby.
