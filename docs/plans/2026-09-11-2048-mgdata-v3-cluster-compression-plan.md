# Kompresja mikrogeometrii `.mgdata` v3: format dyskowy, format pamięciowy, transkodowanie na GPU

Data: 2026-09-11 UTC (rewizja 6, finalna po dwóch rundach recenzji
rewizji 4/5). Rewizja badań: `af99b2d5d3`, anchory sprawdzone względem
`c6d63b055e`; HEAD poszedł dalej (`341966f946`, zmiany w
`render_raytracing.cpp` i `micro_geometry_rt.slang`), symbole są aktualne,
numery linii w tych dwóch plikach mogą być przesunięte. Pliki
`micro_geometry_storage.{h,cpp}` leżą w `servers/rendering/renderer_rd/storage_rd/`. Autorytety: `AGENTS.md`, `.agents/shared/planning-dispatch.md`,
`docs/reference/plan-writing-workflow.md`, `.agents/shared/godot-rules.md`.

## Kontekst i kształt zamówienia

Właściciel: kompresja meshy i assetów jako pierwszy krok pod open world;
poprzednia wersja planu (620–700 MB dla Lucy) odrzucona jako za słaba.
Zatwierdzone decyzje architektoniczne (2026-09-11):
1. **dwa formaty**: dyskowy (gęsty, transkodowany na GPU przy instalacji
   strony) i pamięciowy (czytelny dla buildera CLAS i shaderów, losowy dostęp);
2. **tangenty niejawne** z gradientów UV trójkąta w cieniowaniu meshy
   mikrogeometrycznych (0 bitów);
3. **globalna siatka pozycji** o kroku będącym potęgą dwójki (opcja importu,
   domyślnie 1/16 cm), bez normalizacji do bounds klastra.
Format fork-lokalny, bez mostków: v2 przestaje się wczytywać, assety są
reimportowane. Brak testów automatycznych. Cache shaderów poza zakresem.

Wykluczenia: bez zmiany DAG-u LOD i grupowania, budżetów selekcji, rozmiaru
puli (1 GiB) i polityki streamingu; bez ProjectSettings; bez zmiany sygnatur
API skryptowego; referencje do stron rodziców (deduplikacja między stronami)
i własny dekoder przecięć w stylu DGF poza zakresem (Krok 9 jako przyszłość).

## Cel liczbowy (z literatury, zmierzony na tych samych assetach)

| Źródło | B/trójkąt | Lucy 28.06 M tri |
| --- | ---: | ---: |
| nasz v2, dysk (zstd) | 41 / tri źródłowy (19.9 / tri DAG) | 1120 MB |
| nasz v2, VRAM | 43 / tri DAG | 2410 MB |
| Nanite "Land of Nanite" dysk (Karis 2021, s. 144) | 11.4 / tri źródłowy, 5.6 / tri Nanite | ≈ 320 MB |
| Nanite VRAM | 8.7 / tri Nanite | ≈ 490 MB |
| DGF Lucy (AMD 2024, tab. 3; pozycje 14-bit + topologia) | 2.87 | 80 MB |

Cel planu: **dysk ≈ 7 B / tri DAG (Lucy ≈ 390–420 MB, ≈ 2.8×) bez
deduplikacji wierzchołków; 6 B / tri jako cel po Kroku 7**; **VRAM ≈ 14 B / tri
DAG (Lucy ≈ 790 MB, 3×)**. VRAM ma podłogę z buildera CLAS: pozycje w
formacie AS (`R16G16B16_SNORM`, 6 B) i indeksy 8-bit (3 B/tri).

## Stan faktyczny (anchory)

Payload v2 per klaster: `modules/meshoptimizer/micro_geometry_builder.cpp:105-143`
(float32 stride 48, indeksy u8, id prymitywów u32, id wierzchołków u32);
walidacja `servers/rendering/micro_geometry_data.cpp:288-290,422-433`; strona
≤ 64 KiB, zstd per strona `:351-371`, SHA-256 per strona. Lucy: 634 477
klastrów, 56.05 M tri DAG (2× źródła), 37 918 stron, 61 wierzchołków / 88
trójkątów na klaster. Konsumenci GPU: mesh shader
`shaders/forward_clustered/scene_forward_clustered.slang:95-196,1055-1129`
(groupshared tylko `micro_geometry_surface`; TANGENT konsumowany :470-471,517-520,579-595,605-607),
dekoder RT `shaders/raytracing/geometry_decode_inc.slang:49-353`
(`geometry_fetch_position` :177-196 mnożone przez `ObjectToWorld3x4()` w
`rt_hit_context_inc.slang:209-211,479,538`; `geometry_fetch_tbn` :335-353;
`model_normal_matrix` `rt_hit_context_inc.slang:451→375`), światła emisyjne
`light_sampling_inc.slang:23-35,68-71,280-283` z transformacją z CPU
(`render_raytracing.cpp` `store_transform_transposed_3x4`, emisyjne :3536-3556),
`surface_shading_inc.slang:144,187,204`, shader strony
`micro_geometry_page.slang:20-35` (dispatch w `_build_page_clas`,
`micro_geometry_storage.cpp:160-190`, `primitive_lookup` przez uniform set →
RDG śledzi zapis), builder CLAS z puli (`micro_geometry_storage.cpp:48-69,136-142`,
`vertexFormat` domyślnie float32, honorowany `rendering_device_driver_vulkan.cpp:6826`).
Instalacja strony: `buffer_update(pool, slot*65536, …)` `micro_geometry_storage.cpp:649`
→ `_build_page_clas` :655. Odczyt strony na workerze `:40-70,694`
(`MAX_IO_TASKS = 16`). TLAS instancje na GPU: `micro_rt_transform`
(`micro_geometry_rt.slang`), bez składania na CPU. Import:
`MicroGeometryImport::prepare_geometry` (`editor/import/3d/micro_geometry_import.cpp:154-174`)
robi `ImporterMesh::from_mesh(p_mesh)` i odrzuca kopię; opcje importu sceny w
`resource_importer_scene.cpp` (`get_import_options`), wersja formatu :290-292 = 3,
OBJ :635-637 = 3. Precedens remapu `lods`: `importer_mesh.cpp:544-552`.
`native_assets/microgeometry_stress/*.{res,mgdata}` produkuje
`editor/entity_scene_energy_converter.cpp`. Krok 0 (bramka sprzętowa) wykonany:
RTX 4090 / 616.64 wspiera `R16G16B16_SNORM` i `R16G16B16A16_SNORM` jako
wierzchołki AS; VU 10439 wymaga tylko tego bitu; RD ma
`DATA_FORMAT_R16G16B16_SNORM` (`rendering_device_commons.h:168`).

## Mapa nakładania się

- zstd per strona: zostaje jako LZ dla formatu dyskowego (Kraken niedostępny).
- `micro_geometry_page.slang`: już jest dispatchem „raz przy instalacji strony”
  z zapisem przez uniform set; staje się transkoderem.
- `micro_geometry_custom` (`micro_geometry_inc.slang:179-206`): precedens
  dekodowania nie-float po bitach formatu.
- Kodeki meshoptimizer: niepotrzebne, format dyskowy jest własny (bity per
  klaster + stripy), a LZ robi zstd.
- `positionTruncateBitCount` CLAS: nie zmniejsza pamięci; nieużywane.

## Decyzje projektowe

**D1. Format dyskowy strony (transkodowany na GPU).** Sekcje pogrupowane
typami, wyrównane do bajtów, potem zstd:
- nagłówek klastra: min pozycji na siatce (3 × i32), bity na oś (3 × u8),
  zakres UV per kanał z wykluczeniem największej luki (2 przedziały × u16
  min/max + bity), liczby wierzchołków/trójkątów, `first_primitive`;
- pozycje: bitstream `ceil(log2(zakres))` bitów na oś, na globalnej siatce
  (krok = opcja importu, potęga dwójki, domyślnie 1/16 cm, środek w origin
  obiektu), bez normalizacji do bounds;
- normalne: oktaedr 2 × 8 bitów (opcja 2 × 10 w Tierze jakościowym);
- UV: bity z zakresu klastra; color: RGBA8 gdy obecny; custom: bez zmian;
- topologia: uogólnione stripy (IsReset/IsLeft/IsRef + 5-bit delta
  referencji), cel ~5 bitów/tri; wierzchołki w kolejności pierwszego użycia;
- bez referencji do innych klastrów (ani w stronie, ani do rodziców): każdy
  klaster jest samowystarczalny i dekodowalny niezależnie, bo transkoder
  przetwarza klastry równolegle bez porządku między grupami roboczymi;
  deduplikacja wierzchołków to Krok 7 (przyszłość);
- id wierzchołków źródłowych: po renumeracji (D3) delta względem poprzedniego
  (bitstream), w pamięci rozwinięte.
Builder pilnuje dwóch limitów strony jednocześnie: rozmiar pamięciowy
≤ 64 KiB (slot puli) i rozmiar dyskowy po dekompresji ≤ 64 KiB (slot
stagingu); przy grubych klastrach dużych assetów bity pozycji mogą
przekroczyć 16 na oś, więc drugi limit nie wynika z pierwszego.

**D2. Format pamięciowy (pula, czytany przez CLAS i shadery).** Per klaster:
- pozycje `R16G16B16_SNORM` (6 B) względem izotropowej ramki assetu
  (`center`, `scale = max(half_extent)`), ramka w nagłówku `.mgdata` i w
  każdym `GPUSurface` (dekoder rastra widzi tylko powierzchnię); snorm jest
  drobniejszy niż siatka 1/16 cm dla meshy < ~10 m, więc nie dodaje
  widocznego błędu; dla większych obiektów ograniczenie do zapisania;
- normalna oktaedr 2 × u8 (2 B), UV `u16 × 2` względem zakresu powierzchni
  (4 B) lub `f16 × 2` gdy zakres > 8, color RGBA8; **bez tangentów: slot
  atrybutu 2 znika z buildera (`builder:165,192-196`), manifestu
  (`attribute_offsets[2]`) i obu układów; gałęzie slotu 2 w
  `micro_geometry_inc.slang`, `geometry_decode_inc.slang:343-346` i
  `scene_forward_clustered.slang:131-142,172-174` usunięte w tym samym kroku**;
  stride 12 B (pos+n+uv) + custom;
- indeksy u8 × 3 (3 B/tri) wprost dla CLAS; `first_primitive` w
  `MicroGeometryCluster.pad` (64 B utrzymane);
- id wierzchołków u32 (4 B) dla `VERTEX_INDEX`;
- każdy blok (wierzchołki, indeksy, id) zaczyna się na granicy 4 B (jak dziś
  `builder:108`), także adres wierzchołków dla CLAS.
Rachunek: (12 + 4) × 0.693 + 3 = 14.1 B / tri DAG → Lucy ≈ 790 MB VRAM.
Jeden autorytet układu pamięciowego: funkcja C++ w `MicroGeometryData`
liczy offsety bloków per klaster z manifestu (`vertex_count`,
`triangle_count`, stride powierzchni); CPU używa jej do `TriangleInfo` i
wysyła je do transkodera przez istniejący per-stronowy `info_buffer`
(`TriangleInfo::vertices/indices`, `micro_geometry_storage.cpp:60-63,162`,
lifetime w `page.clas_resources`), związany z transkoderem jako drugie
wejście; transkoder tylko zapisuje pod podane offsety, a shadery czytają te
same pola przez `payload_offset` i stride powierzchni (bez własnego liczenia
układu). Brak dodatkowego bufora na tabelę offsetów.
Konwencja przestrzeni: payload dekoduje się do przestrzeni ramki; ramka
wchodzi do transformacji TLAS (`micro_rt_transform`, także `previous`);
`geometry_fetch_position` nie stosuje ramki; raster stosuje ją w
`micro_geometry_pull_vertex`. Światła emisyjne: `light_decode_light_transform`
(`light_sampling_inc.slang:22-38`) składa dziś `cpu × multimesh`
(`geometry_multimesh_transform`, `geometry_decode_inc.slang:231-240`); dla
`FLAG_CLUSTERED` dokłada ramkę jako **prawy** czynnik (`cpu × multimesh ×
ramka`, ramka z `geometry_micro_surface`), więc transformacja z CPU
(`render_raytracing.cpp:3568-3569,4944-4953`) pozostaje gołą transformacją
instancji i zgadza się z TLAS.

**D3. Renumeracja źródła.** `prepare_geometry` po zbudowaniu `.mgdata`
odbudowuje powierzchnie `p_mesh` (`clear_surfaces` + `add_surface_from_arrays`,
`lods` remapowane jak `importer_mesh.cpp:544-552`, shadow mesh
regenerowany) w kolejności pierwszego wystąpienia w klastrach liściowych.
`primitive_lookup` liczony z `first_primitive`; `geometry_resolve_primitive`
zwraca `first_primitive + local`.

**D4. Tangenty niejawne, per wierzchołek w mesh shaderze.** Raster: bez
atrybutów per-prymityw (budżet `maxMeshOutputMemorySize` jest już w całości
przydzielony 128 wierzchołkom, `scene_shader_forward_clustered.cpp:809-811`,
`static_assert(sizeof(VertexOutput) <= MICRO_GEOMETRY_OUTPUT_STRIDE_LIMIT)`).
Mesh shader po `pull_vertex` liczy w groupshared tangent/bitangent per
trójkąt z delt pozycji i UV (Schlüler 2013), akumuluje je per wierzchołek
klastra (pętla po ≤ 128 trójkątach, bez atomików) i normalizuje; wynik trafia
do istniejącego wejścia ścieżki mikrogeometrii `_unpack_vertex_attributes`
(`scene_forward_clustered.slang:706-714`, statyczne `micro_geometry_tangent/binormal`
:84-85), skąd przechodzi przez ten sam łańcuch `model_normal_matrix`/view co
dziś (:517-520, :579-595, konsumenci :470-471, :605-607) i przez blok
`VERTEX` materiału. Kształt przebiegu: `pull_vertex` + zapis pozycji/UV do
groupshared → bariera w zakresie jednolitym (poza gałęzią
`lane < vertex_count`) → akumulacja per trójkąt i normalizacja per
wierzchołek → bariera → `evaluate_vertex`. Przypadek zdegenerowany: gdy
powierzchnia nie ma UV (`attribute_offsets[4] == INVALID`) lub wyznacznik UV
trójkąta jest poniżej epsilonu, tangent liczony jak dziś z normalnej i
dowolnej osi (`scene_forward_clustered.slang:172-174`,
`geometry_decode_inc.slang:347-351`); bez NaN dla assetów skanowanych bez UV
(Lucy, dragon, Thai). `evaluate_vertex` i układ potoku bez zmian. RT:
`geometry_fetch_tbn` (`geometry_decode_inc.slang:335`) zmienia sygnaturę na
trójkątową (trzy pozycje + UV, normalna per wierzchołek zostaje), oba
wywołania `rt_hit_context_inc.slang:347,368` aktualizowane (ten sam
przypadek zdegenerowany); ścieżka konwencjonalna liczy jak dziś z jawnych
tangentów i ignoruje argumenty trójkątowe. Ograniczenie: tangent
per wierzchołek uśredniony w obrębie klastra różni się na szwach klastrów o
kąt rzędu błędu geometrycznego klastra; ocena wizualna w Kroku 4.

**D5. Transkodowanie na GPU przy instalacji strony.** Nowy przebieg w
`micro_geometry_page.slang`: staging (strona dyskowa po zstd, uniform set,
read) → pula slot (uniform set, write) + `primitive_lookup`. Jeden dispatch
per strona, jedna grupa robocza per klaster (dekodowanie
bitstreamu i stripów jest sekwencyjne w klastrze, ≤ 128 wierzchołków/trójkątów;
klastry są niezależne, więc brak porządku między grupami nie ma znaczenia),
a `clas_build` po nim jak dziś. Transkoder przetwarza **wszystkie** klastry
strony; dzisiejszy wczesny return dla `refined_group != INVALID`
(`micro_geometry_page.slang:25`) zostaje tylko dla zapisu `primitive_lookup`.
Walidacja indeksów, którą dziś robi `read_page` na workerze
(`micro_geometry_data.cpp:419-434`), przenosi się do transkodera: każdy
zdekodowany indeks ≥ `vertex_count` jest klamrowany do trójkąta
zdegenerowanego (0,0,0) przed zapisem bloku, więc `clas_build` w tym samym
`_build_page_clas` nigdy nie dostaje indeksu poza zakresem; zliczenie takich
trójkątów trafia do statystyk strony. Staging: pierścień `MAX_IO_TASKS × 64 KiB`, jeden bufor RD,
właściciel `MicroGeometryStorage`, zwalniany z pulą. Zapis do puli przez
uniform set daje RDG zależność RW przed buildem CLAS (jak `primitive_lookup`
dziś), bez ręcznych barier.

**D6. Krok 6 (bez tablic źródłowych w `.scn`) bez zmian względem rewizji 3**,
osobna zgoda; anchory w `mesh_storage.cpp:266-354,596-681,684-708,733-745,1031-1073`,
`mesh.cpp:1515,1590`.

## Trzy kąty dowodowe

- **API/kontrakty:** format `.mgdata` v3 (nagłówek: ramka, krok siatki;
  manifest: `Cluster.first_primitive`, `Surface` z zakresem/trybem UV i ramką,
  bity formatu; strona dyskowa D1), `FORMAT_VERSION=3`, `BUILD_VERSION=3`,
  `BUILDER_COMMIT`; struktury GPU (`GPUSurface` rośnie: `static_assert` w
  `micro_geometry_storage.h:313`; `GPUCluster` 64 B :311; `GPUAsset` :315
  bez zmian), `build_input.vertex_format`, `RT_LightData::transform` dla
  klastrowych; opcja importu `meshes/micro_geometry_position_step` w
  `ResourceImporterScene`/`ResourceImporterOBJ` (`get_import_options`, XML
  klasy importera bez zmian, bo opcje importu nie są w `doc/classes`);
  `get_format_version` importerów; nowy przebieg shadera w istniejącym pliku
  (bez nowych plików w grafie SCons).
- **Konsumenci:** builder, `MicroGeometryData`, storage (`_read_page`,
  `update`, `_build_page_clas`), `micro_geometry_page.slang`,
  `micro_geometry_inc.slang`, `scene_forward_clustered.slang`,
  `geometry_decode_inc.slang`, `rt_hit_context_inc.slang` (TBN),
  `micro_geometry_rt.slang`, `render_raytracing.cpp` (światła),
  `micro_geometry_import.cpp`, `importer_mesh.cpp`, importery, konwerter.
- **Własność/wątki:** kodowanie w builderze (import); zstd na workerze;
  upload stagingu, transkodowanie i CLAS na wątku renderu; jeden nowy bufor
  RD (staging), zwalniany z pulą; brak nowych ścieżek barier.

## Sekwencja kroków

**Krok 0** → wykonany (bramka sprzętowa).

**Krok 1 → Builder i format v3 (dysk + manifest), renumeracja, opcja importu** `[independent]`
- what: `micro_geometry_builder.cpp` (`Builder::output`, `build_surface`;
  koder D1: siatka, bity per klaster, oktaedry, UV z luką, stripy,
  referencje w stronie, delta id), `micro_geometry_data.h/.cpp` (nagłówek,
  manifest, `validate`, `read_page` = dekompresja zstd + walidacja nagłówków
  klastrów; walidacja indeksów przenosi się na koniec transkodowania jako
  test zasięgów w nagłówku), `importer_mesh.cpp:565-591` i
  `micro_geometry_import.cpp:154-174` (D3), `resource_importer_scene.cpp`/
  `resource_importer_obj.cpp` (opcja kroku siatki `meshes/micro_geometry_position_step`
  w `get_import_options`, przekazywana przez nowy parametr
  `import_scene_micro_geometry(...)`/`import_micro_geometry(...)`
  (`micro_geometry_import.cpp:196,208`) do `ImporterMesh::generate_micro_geometry`
  i buildera; `get_format_version` 3 → 4). Usunąć zapis float32 i tablicę id prymitywów.
- how: cięcie stron po obu limitach; kod bez symboli edytora; do statusu:
  B/tri dysk, max błąd pozycji. `import_scene_micro_geometry` jest w
  `micro_geometry_import.cpp:207`.
- dispatch: `[independent]`.

**Krok 2 → Storage: staging, transkoder, CLAS** `[independent]`, po Kroku 1
- what: `micro_geometry_storage.h/.cpp`: bufor staging (tworzony/zwalniany z
  pulą :200-215 i finalize), `update` :641-660 (upload do stagingu zamiast
  puli), `_build_page_clas` :160-190 (dispatch transkodera z uniform setami
  staging/pula/lookup, potem `clas_build` z `vertex_format = R16G16B16_SNORM`,
  stride 12, `TriangleInfo` z offsetami pamięciowymi liczonymi z nagłówków
  klastrów), `_read_page` :40-70 (zasięgi z nagłówków), upload metadanych
  :241-243 (ramka, UV, `first_primitive`); `micro_geometry_page.slang`
  (transkoder D5 + `primitive_lookup` z `first_primitive`; ewikcja strony
  czyści wpisy jak dziś). Potwierdzić w implementacji, że zapis do puli
  przez uniform set jest widziany przez RDG jako producent dla budowy AS
  (dziś porządek daje `compute_list_add_buffer_dependency` :177-179 i
  `geometry_dependency = pool` :184-186); jeśli nie, zachować te dwa
  wywołania.
- dispatch: `[independent]`.

**Krok 3 → Dekodowanie w shaderach, tangenty niejawne, ramka w TLAS i światłach** `[independent]`, po Kroku 2
- what: `micro_geometry_inc.slang` (dekodery pamięciowe), `scene_forward_clustered.slang`
  (walidator, `pull_vertex` z ramką, tangent per wierzchołek w groupshared,
  wstrzyknięcie :706-714, konsumenci :470-471,517-520,579-595,605-607),
  `geometry_decode_inc.slang` (walidator, `geometry_select_triangle`,
  `geometry_resolve_primitive`, `geometry_fetch_position` bez ramki, uv/color,
  `geometry_fetch_tbn` niejawnie), `rt_hit_context_inc.slang` (TBN, weryfikacja
  `model_normal_matrix`), `micro_geometry_rt.slang` (`micro_rt_transform` ×
  ramka), `light_sampling_inc.slang` (`light_decode_light_transform` ×
  ramka jako prawy czynnik dla `FLAG_CLUSTERED`); `render_raytracing.cpp`
  tylko weryfikacja, że transformacja świateł pozostaje gołą instancją. Sprawdzić
  `ddgi_trace_inc.slang`, `rtxdi_di.slang`, `surface_shading_inc.slang`,
  `geometry_positions.slang`. Budżet mesh shadera: sonda
`micro_geometry_mesh_supported` (`scene_shader_forward_clustered.cpp:798`)
podnosi wymagany `max_shared_memory_size` do faktycznego zapotrzebowania
przebiegu tangentów (pozycje, UV, indeksy, akumulatory dla 128
wierzchołków/trójkątów, rząd 6–7 KiB) i `static_assert` w
`scene_forward_clustered.slang:1062` obejmuje nowe tablice, w tym samym
kroku, żeby urządzenie poniżej progu wybrało wariant bez mesh shadera.
- dispatch: `[independent]`. Kroki 1→2→3 szeregowo.

**Krok 4 → Reimport, regeneracja, pomiar** (main agent)
- Build ordinary/double/template. Reimport dragona, `seam_grid.obj`, Lucy,
  Thai; konwerter → `native_assets/microgeometry_stress`. Zapisać: rozmiar
  `.mgdata` i B/tri (cel ≤ 6 B/tri DAG), VRAM puli, czas importu, czas
  transkodowania na stronę, max błąd pozycji, współczynnik zstd; kryterium:
  ≤ 7 B / tri DAG na dysku (oczekiwane), 6 B jako cel po Kroku 7.
- Edytor double, `microgeometry_stress/scene.escn` z `--benchmark --verbose`
  (ładowanie, pending pages, FPS, `Microgeometry shared cuts`); zrzuty:
  szwy (`microgeometry/scene.tscn`), stress vs obecny, DDGI wg
  `docs/reference/ddgi-diagnostics.md`, RTXDI, normal-mapa na klastrowym
  meshu (jakość tangentów niejawnych), normal-mapa na meshu konwencjonalnym
  (regresja po zmianie sygnatury `geometry_fetch_tbn`), światło emisyjne na
  dragonie.
  Status do `docs/research/<data>-mgdata-v3-compression-status.md` i
  `project-state.md`.

**Krok 5 → Bez tablic źródłowych dla powierzchni zmapowanych** `[independent]`, osobna zgoda (D6)
- jak w rewizji 3 (Krok 6): `micro_geometry_import.cpp`, `mesh.cpp:1515,1590`,
  `surface_get_arrays` z rekonstrukcją z liści, `mesh_storage.cpp` jawna
  obsługa powierzchni bez danych, usunięcie ścieżki upload/discard/re-create.
- dispatch: `[independent]`.

**Krok 6 → Pomiar po Kroku 5** (main agent): `.scn`/`.res`, RAM edytora,
`load_subset`, lista narzędzi na danych stratnych.

**Krok 7 (przyszłość, poza planem) → referencje do stron rodziców** (Nanite:
~30 % wierzchołków) i **DGF-owy dekoder przecięć** dla warstwy AS; wymagają
porządku instalacji rodzic-przed-dzieckiem i własnych prymitywów
proceduralnych.

## Zamknięcia obowiązkowe

- ClassDB/XML: brak nowych metod; `MicroGeometry.get_statistics` bez zmian;
  opcja importu przez `get_import_options` (nie ClassDB). Krok 5: sygnatury
  bez zmian, jedna linia w XML `ArrayMesh`/`RenderingServer`.
- SCons/GLSL: bez nowych plików shaderów (transkoder w `micro_geometry_page.slang`).
- RID/lifetime: jeden nowy bufor (staging), właściciel `MicroGeometryStorage`,
  zwalniany z pulą, nie w trakcie klatki w locie; tabela offsetów to
  istniejący per-stronowy `info_buffer` w `page.clas_resources`.
- Wątki: bez zmian granic; zapis do puli tylko przez shader na wątku renderu.
- Backendy: Vulkan (NVIDIA); D3D12/Metal bez CLAS, bez zmian.
- Upstream: `micro_geometry_*` fork-lokalne; importery: opcja + literał
  wersji; `importer_mesh.cpp`, `mesh.cpp`, `mesh_storage.cpp`,
  `render_raytracing.cpp`, `rt_hit_context_inc.slang` wąsko.
- Kompatybilność: v2 bez readera; `.mgdata` nie są w git; konwerter
  regeneruje native_assets.

## Ryzyka i niepewności (nieblokujące)

- Cel 6 B/tri zakłada stripy ~5 bit/tri i skuteczne zstd na bitstreamach
  wyrównanych do bajtów; Krok 4 mierzy, Tier jakościowy (normalne 2 × 10 bit)
  kosztuje +0.35 B/tri.
- Transkodowanie sekwencyjne per klaster: przy 16 stronach w locie i ~500
  klastrach na stronę to tysiące grup roboczych na klatkę streamingu; koszt do
  zmierzenia, Nanite raportuje ~50 GB/s na PS5 dla podobnego schematu.
- Tangenty niejawne: nieciągłość na krawędziach przy grubych klastrach
  (coarse LOD daleko od kamery), ocena wizualna w Kroku 4.
- Siatka 1/16 cm globalnie: mesh > ~10 m traci precyzję snorm w pamięci
  (ograniczenie do zapisania), a bardzo małe obiekty (< 1 cm) potrzebują
  mniejszego kroku przez opcję importu.
- Brak deduplikacji wierzchołków (Nanite: ~30 % jako referencje) kosztuje
  szacunkowo 15–25 % rozmiaru dysku względem Nanite; Krok 7.
- Ramka assetu powielona per powierzchnia w `GPUSurface` (wartość globalna
  dla assetu): builder zapisuje ją raz w nagłówku, storage kopiuje do każdej
  powierzchni; jedyne źródło to nagłówek.
