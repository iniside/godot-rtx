# GPU-driven microgeometry — research pod plan

Data: 2026-09-08, 20:05 UTC. Baza źródeł: `4c0f7f97b534a7997ab5b14b87931f1a6b243f01`.
Metoda: trzy niezależne analizy importu, rasteryzacji GPU i RT; clangd przez
`clang-nav` z istniejącym `compile_commands.json`, odczyty implementacji,
bindingów/XML, SCons i historii oraz pierwotne źródła meshoptimizer/NVIDIA/Vulkan.
Jednorazowe wyniki references nie dawały pełnej mapy prywatnych wywołań;
uzupełniono je konkretnymi odczytami źródeł. Inwentaryzacje tekstowe są dolną
granicą pokrycia. Badane źródła silnika nie różniły się od bazy; istniejące
niezwiązane zmiany dokumentacji i scen pozostają własnością innych prac.

To materiał do napisania planu, nie zatwierdzony plan wykonawczy. Nie zmieniono
silnika, nie uruchomiono buildów, automatycznych testów ani pomiarów GPU.
Rekomendacje poniżej nie są dowodem poprawności ani przyspieszenia implementacji.
Trzy analizy źródeł zakończono. Próba osobnego świeżego review commita
`1c720084755f56643f4c151d3bef3828478fbde9` została zablokowana przez harness
(`agent thread limit reached`); raport nie ma końcowego niezależnego werdyktu.

## 1. Zakres właściciela

- Import mesha do Godota, automatyczne meshlety i DAG uproszczeń; statyczne
  obliczenia wykonywane podczas importu, bez ręcznych proxy i LOD-ów obiektów.
- Trwała scena GPU, selekcja klastrów, culling, generowanie pracy indirect,
  stronicowanie geometrii i wykorzystanie CLAS przez scenę RT.
- Osobny wybór geometrii dla kamery i RT. RT domyślnie może używać znacznie
  prostszej reprezentacji, szczególnie poza ekranem; obecnymi konsumentami
  jakości są cienie i DDGI. Nowe odbicia nie należą do wymagań.
- Co najmniej jeden rzeczywisty debug view trójkątów lub klastrów/meshletów.
- Końcowe doprecyzowanie właściciela: proste sterowanie stopniem uproszczenia
  RT, bez algorytmów analizy wpływu świateł, receiverów czy sond. Tolerancja
  błędu RT i mnożnik poza ekranem wystarczają do pierwszego planu.
- Doprecyzowanie z tej rozmowy: DAG dla geometrii bez deformacji, także dla
  obiektów poruszanych transformacją i instancingu. Zachować działanie obecnych
  deformowanych meshów, w tym szkieletów, blend shapes i deformacji shaderowej.
- Foliage i wokselizacja są poza zakresem. Granice danych mają umożliwiać ich
  późniejsze dodanie bez przebudowy całego renderera; nie tworzyć teraz pustych
  implementacji ani ogólnego frameworka reprezentacji.

ECS, world partition, streaming tekstur, nowe oświetlenie i nowe backendy RT nie
są potrzebne do tego przekroju. Istniejący PT pozostaje konsumentem wspólnego
dekodowania geometrii i materiałów, bez rozbudowy funkcji odbić.

## 2. Co już istnieje i co faktycznie trzeba zastąpić

Kotwice poniżej podają ścieżki względem repozytorium i linie badanej bazy.
W dalszym tekście krótkie nazwy odnoszą się do już wskazanych plików;
`shaders/raytracing/` i `effects/` oznaczają podkatalogi
`servers/rendering/renderer_rd/`.

| Obszar | Stan i kotwica | Konsekwencja |
| --- | --- | --- |
| Import sceny | `editor/import/3d/resource_importer_scene.cpp:2811`: LOD-y powierzchni → shadow mesh → optymalizacja indeksów → `generate_clusters()` | Budowa DAG-u musi następować po finalnych zmianach topologii i atrybutów. |
| Klasteryzacja | `scene/resources/3d/importer_mesh.cpp:574`, `ImporterMesh::generate_clusters()` | Są przestrzenne meshlety, bez hierarchii uproszczeń. Obecne parametry to 128 wierzchołków i 96–128 trójkątów. |
| Format | `importer_mesh.cpp:563`, zapis `:651`; `importer_mesh.h:66` | `CLUS` v1: nagłówek, rekordy klastrów, lokalne indeksy u8 i pozycje float3. Brak DAG-u, błędów, bounds i stron. |
| Zasób GPU | `servers/rendering/renderer_rd/storage_rd/mesh_storage.cpp:381`, `:495`, `:809` | Cały blob jest walidowany, wysyłany na GPU i odtwarzany przez readback przy pobieraniu powierzchni. To nie streaming. |
| Raster | `servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp`, `_fill_render_list()`, `_fill_instance_data()`, `_render_list_template()` | CPU tworzy listy i wykonuje pętlę powierzchni z wiązaniem materiału, geometrii i pipeline. Pass powierzchni wyłącza zwykły wybór LOD (`:1156`). |
| Członkostwo RT | `servers/rendering/renderer_scene_cull.cpp:3227` | RT zbiera rezydentne Mesh/MultiMesh niezależnie od frustum/HZB kamery. Zachować tę własność. |
| Statyczny CLAS | `servers/rendering/renderer_rd/forward_clustered/render_raytracing.cpp:1200`, `_populate_cluster_blas()` | CPU buduje wszystkie klastry powierzchni i jeden BLAS wskazujący cały zbiór. |
| Rigid MultiMesh | `render_raytracing.cpp:1996`, `_build_merged_mm_blas()`, wywołanie `:2881` | Obecnie replikuje geometrię instancji do zwykłego BLAS; musi wejść do nowej ścieżki DAG. |
| Deformacje | `render_raytracing.cpp:2752`, `process_deformed_surface()`; `_populate_surface_blas():1442` | Zachować właściwą ścieżkę deformowanej geometrii. Nie jest starym fallbackiem dla statycznego DAG-u. |

Nie dodawać drugiego konkurencyjnego rejestru sceny. Rozdzielić trwałe dane
zasobów/instancji od per-viewport selekcji i list pracy w istniejącym ownership
RenderingServer/renderer RD. CPU nadal publikuje zmiany i wiąże grupy pipeline;
GPU przejmuje pracę zależną od liczby instancji i wybranych klastrów.

## 3. Import: algorytm i dane przygotowane offline

Rekomendacja: wykorzystać istniejący meshoptimizer i oprzeć builder na aktualnym
`clusterlod.h`, zamiast wprowadzać stary `nv_cluster_lod_builder` jako dodatkową
podstawę. NVIDIA przeszła na meshoptimizer; stary opis biblioteki jest użyteczny
algorytmicznie, ale nie odzwierciedla aktualnej zależności sampla.
[Aktualny builder](https://github.com/zeux/meshoptimizer/blob/0870c3881655df9b7d22faa35c825393534416bc/demo/clusterlod.h),
[zmiana zależności](https://github.com/nvpro-samples/vk_lod_clusters/blob/22301a0cb682eec543dd851a6502c26ffdf436f5/CHANGELOG.md).

Pętla budowy: klastry → grupy sąsiadujących klastrów → uproszczenie całej grupy
z zablokowaną granicą → ponowna klasteryzacja → grupowanie przekraczające dawne
granice. Niezależne upraszczanie każdego meshletu nie zapewnia zgodnych połączeń.
DAG opisuje pochodzenie uproszczeń; osobna hierarchia przestrzenna przyspiesza
wyszukiwanie grup. Granice i błąd selekcji muszą być monotoniczne w relacjach
uproszczeń; ciasne bounds pojedynczego klastra służą cullingowi.
[Opis generowania i selekcji](https://github.com/nvpro-samples/vk_lod_clusters/blob/22301a0cb682eec543dd851a6502c26ffdf436f5/docs/lod_generation.md).

`modules/meshoptimizer/SCsub` już obejmuje partition/simplifier/clusterizer/
meshletutils; `register_types.cpp:38` podpina obecne callbacki SurfaceTool.
Zbadane API zależności obejmuje `meshopt_simplifyWithAttributes`, bounds i
partitioning. To narzędzia budowy, nie gotowy lokalny DAG. Przy planowaniu
integracji przypiąć konkretny builder i sprawdzić jego wymagania wobec lokalnej
wersji; zmiany zależności prowadzić przez istniejący mechanizm importu/patchy,
bez ręcznych edycji `thirdparty/`.

Offline przygotować:

- lokalne indeksy, pozycje i atrybuty potrzebne każdemu poziomowi;
- grupy, relacje refinement, zbiór terminalnych grup i hierarchię wyszukiwania;
- bounds klastrów/grup, błąd uproszczenia w jednostkach mesha, wymagane ograniczenia
  szwów i materiałów; opcjonalne cone bounds tylko tam, gdzie są poprawne;
- mapowanie surface/material, tożsamość liści/trójkątów i układ atrybutów;
- strony, zależności grup, katalog zakresów danych, skompresowane payloady;
- wersję buildera/formatu, ustawienia importu, identyfikator zawartości i statystyki.

Pozostają runtime: widoczność, projekcja błędu, transformacje, rezydencja,
adresy GPU oraz sprzętowe CLAS/BLAS/TLAS. Nie zapisywać sprzętowego AS do zasobu.
Nie zakładać jednego terminalnego klastra: uproszczenie może zatrzymać się na
wielu grupach, np. przez szwy. Metryka błędu geometrycznego nie gwarantuje sama
zachowania otworów, topologii ani jakości cienia.

Pierwszy builder powinien zachować powierzchnie materiałowe jako granice.
UV0/UV1, normalne, tangent handedness, kolor/alpha i custom streams muszą
przetrwać na poziomach uproszczonych. Polityka nieznanych custom semantics musi
być jawna: ochrona odpowiednich wierzchołków lub ograniczenie uproszczenia;
nie uśredniać arbitralnych identyfikatorów jak koloru. Zgodne pozycje brzegów
między powierzchniami wymagają wspólnej reguły blokowania/kwantyzacji.

Nie ograniczać integracji do glTF. glTF wywołuje `ImporterMesh::add_surface`
(`modules/gltf/gltf_document.cpp:1991`), ale samodzielny OBJ zapisuje mesh osobno
(`editor/import/3d/resource_importer_obj.cpp:665`, `:697`), bez obecnej
klasteryzacji. Oba importery mają korzystać z jednego buildera.
`SurfaceTool::commit()` (`scene/resources/surface_tool.cpp:733`) i
`ImporterMesh::from_mesh()` (`importer_mesh.cpp:1085`) też wymagają jawnej polityki.
Rekomendacja pod plan: importowane niezmienne meshe dostają DAG; runtime-created
i modyfikowalne zasoby zachowują obecną obsługę, dopóki nie mają poprawnie
przygotowanego zasobu pochodnego. Ewentualne publiczne API runtime bake jest
osobną decyzją zakresu, nie niejawnie nowym wymaganiem.

## 4. Format, reimport, eksport i prawdziwe stronicowanie

Rekomendowany kontrakt: manifest zasobu pochodnego i oddzielnie adresowalny
payload stron. Manifest zawiera tożsamość, ustawienia, układ atrybutów, metadane
DAG-u, mapowanie materiałów, katalog stron i jawne zależności payloadu.
Potrzebny jest zbiór rezydentnych grubych grup pokrywający cały zasób; korzeń
logiczny nie musi być pojedynczym meshletem. Grupa replacement jest jednostką
poprawności, strona jest jednostką I/O i może zawierać kilka grup.

Nie zastępować jednego wielkiego `PackedByteArray` drugim: parser binarny czyta
całą taką właściwość (`core/io/resource_format_binary.cpp:494`). PCK daje
`FileAccessPack::seek()` i `get_buffer()` (`core/io/file_access_pack.cpp:442`,
`:472`), czyli możliwy jest zakresowy odczyt stron z wyeksportowanej gry.
Katalog wymaga offsetów/rozmiarów, rozmiaru po dekompresji i kontroli integralności.

`r_gen_files` rejestruje pliki importu (`editor/file_system/editor_file_system.cpp:3001`),
ale samo to nie dowodzi eksportu payloadu. `ResourceFormatImporter::get_dependencies()`
przekazuje zależności importowanego zasobu (`core/io/resource_importer.cpp:461`),
a exporter usuwa sekcję deps importu (`editor/export/editor_export_platform.cpp:1642`).
Rekomendacja: manifest z natywną obsługą zależności zasobu i jawnie enumerowanym
payloadem; jeżeli payload jest plikiem surowym, plan musi zamknąć jego dołączenie
przez exporter. Zwykły String ze ścieżką nie wystarcza. Obejmie to także export
wybranej sceny i customization zasobów, nie tylko eksport całego projektu.

Wersja importera sceny (`resource_importer_scene.cpp:287`) i odpowiednia wersja
importera OBJ muszą wymusić przebudowę starego forkowego formatu. Nie dodawać
czytnika-migratora `CLUS` v1. Upstream `ArrayMesh`/`ImporterMesh` oraz ich publiczne
API array/LOD pozostają zgodne; stare whole-surface LOD-y nie sterują nowym DAG-iem.
Zachować external mesh `save_to_file`/takeover/reload (`resource_importer_scene.cpp:2823`).

Import publikuje kompletny wynik albo opisuje błąd ze wskazaniem mesha/surface.
Obecne `generate_clusters():574` jest `void` i pomija część błędów; to za słaby
kontrakt dla wymaganego payloadu. Reimport wymienia generację atomowo; wyniki
opóźnionego I/O starszej generacji nie mogą nadpisać nowego zasobu.

## 5. GPU raster: rekomendowany zakres pierwszego wdrożenia

Trwałe tablice: zasoby geometrii, instancje, transformacje bieżące/poprzednie,
odwołania do materiałów i rezydentnych stron. Stabilny identyfikator obiektu
nie może być pozycją w skompaktowanej liście widoczności. Per-viewport pozostają
parametry selekcji, historia cullingu i listy wybranych klastrów.

Istniejące `RT_GeometryData`/`RT_MaterialData` (`render_raytracing.h:55`, `:109`)
i `BindlessBlock` (`servers/rendering/renderer_rd/bindless_block.h:39`) są
punktami rozszerzenia wspólnych kontraktów. `finalize_buffers():1958` dziś
przesyła całe bieżące tablice per viewport, więc sam reuse bufora nie oznacza
trwałego rejestru sceny. Zachować rasterowy high/low transform double
(`render_forward_clustered.cpp:872`) oraz per-viewport origin RT
(`render_raytracing.cpp:114`); wspólne strony pozostają w przestrzeni mesha.

Rekomendacja: compute traversal + grupowanie pracy według zgodnych materiałów/
pipeline + indexed indirect count z pobieraniem danych geometrii z tablic GPU.
Nie uzależniać tego planu od visibility buffer, mesh shaderów i software rasterizer.
Współdzielony format klastrów i oddzielona selekcja umożliwiają późniejszą zmianę
backendu rasteryzacji bez przebudowy importera.

Materiały nadal wymagają zgodnego programu/pipeline i danych uniform. Sam
bindless tekstur nie umożliwia mieszania dowolnych programów `.gdshader` w jednym
draw. Zakres rekomendowany: GPU-driven dla kwalifikowanej geometrii opaque i
alpha-tested; istniejące sortowane alpha blending oraz deformacje zachowują
działanie. Jawnie opisać eligibility w planie, bez obiecywania migracji całej
przezroczystości. Nowy pass musi nadal dostarczać sześć surface attachments,
depth i motion używanych przez obecne oświetlenie/rekonstrukcję
(`render_forward_clustered.cpp:2009`).

Driver już ma count-indirect, Vulkan implementuje go w
`drivers/vulkan/rendering_device_driver_vulkan.cpp:5980`, `:5993`.
Brakuje połączenia tej operacji z RD/graph; istniejące
`RenderingDevice::draw_list_draw_indirect()` (`rendering_device.h:1623`,
`rendering_device.cpp:6518`, `doc/classes/RenderingDevice.xml:382`) otrzymuje
liczbę drawów od CPU. Rozszerzyć istniejący kontrakt, nie pisać duplikatu backendu.
Vulkan pozwala czytać liczbę drawów z bufora GPU z ograniczeniem hosta
`maxDrawCount`. [Specyfikacja](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdDrawIndexedIndirectCount.html).

Frustum i HZB dotyczą rasteryzacji, nie usunięcia geometrii RT. Dla temporalnego
HZB potrzebna jest obsługa odkrycia powierzchni przy ruchu i teleportach;
rekomendowany układ dwóch przebiegów z ponownym sprawdzeniem odrzuconej pracy
przeciw bieżącej głębokości. Nie kopiować bezkrytycznie sampla: jego changelog
opisuje błędy poprzedniego HZB oraz persistent traversal o zachowaniu poza specyfikacją.
Rekomendowany traversal pierwszej wersji: ograniczone kolejki i wieloprzebiegowe
dispatch indirect; przepełnienie zachowuje poprawny grubszy cut, nie dziury.
[Źródło ograniczeń](https://github.com/nvpro-samples/vk_lod_clusters/blob/22301a0cb682eec543dd851a6502c26ffdf436f5/CHANGELOG.md).

## 6. RT: automatycznie prostsza geometria, realna zmiana authority

`RenderingDevice::clas_build()` (`rendering_device.cpp:708`) już przyjmuje
bufory descriptor/count. Natomiast `blas_build_from_clusters():758` zabrania
ponownej budowy, a Vulkan `command_build_blas_from_clusters():6972` zapisuje
pojedynczy blok argumentów z CPU. `tlas_build():797` przyjmuje CPU
`Span<AccelerationStructureInstance>`. Sama obecność wywołania Vulkan z nazwą
Indirect nie oznacza, że renderer jest GPU-driven.

Nowy kontrakt musi objąć batched BLAS z list klastrów i liczników GPU oraz TLAS
z rekordów instancji GPU. Wzorce śledzenia AS w
`servers/rendering/rendering_device_graph.cpp:1885`, `:1918` trzeba rozszerzyć
na wszystkie inputy, liczniki, CLAS storage, scratch, wyjścia i ich lifetime.
Usunięcie samej blokady jednorazowego buildu jest niewystarczające.
Specyfikacja wymaga osobnej synchronizacji danych indirect i zapisów AS;
operacje wewnątrz batcha nie mają domyślnego porządku.
[Kontrakt Vulkan](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBuildClusterAccelerationStructureIndirectNV.html).

Proponowana polityka zgodna z końcowym doprecyzowaniem właściciela: jeden RT cut
na instancję dla wspólnych cieni/DDGI, z grubą reprezentacją całego rezydentnego
scenariusza. Dwa ustawienia w sekcji geometrii RT:

- **RT Geometry Error** — dopuszczalny rzutowany błąd uproszczenia; większa
  wartość oznacza prostszą geometrię. Używa istniejących w DAG-u bounds/error
  i prostego przeliczenia odległości, skali instancji oraz projekcji.
- **Off-screen Error Multiplier** — mnożnik tolerancji dla całej instancji poza
  frustum kamery. Wartość 1 nie dodaje uproszczenia; większa je zwiększa.

Tolerancja jest wspólna dla instancji, a cut respektuje grupowe reguły DAG-u.
Nie dodawać analizy istotności świateł, refinementu przy receiverach, feedbacku
promieni ani selekcji według kaskad DDGI. Culling kamery nie usuwa obiektów z RT.
Ustalenie jednostki w UI (rekomendowane piksele docelowego obrazu, niezależnie
od rozdzielczości RTXDI/DDGI) i wartości początkowych należy do planu; nie ma
jeszcze pomiaru uzasadniającego konkretny default. Brak szczegółowej strony
oznacza użycie dostępnego grubszego cutu i zgłoszenie żądania, nawet gdy zadana
tolerancja chwilowo nie jest osiągnięta. Nie wdrażać osobnych TLAS dla efektów.

Zachowanie całego rezydentnego scenariusza jest prostą regułą istniejącego RT.
DDGI używa długości przekątnej zewnętrznej kaskady jako długości promienia
(`effects/ddgi_effect.cpp:243`, `:283`), a promienie światła z trafień sond mogą
sięgać dalej. To uzasadnia niewycinanie sceny do pudełka sond, bez projektowania
nowego algorytmu zasięgu ani streamingu świata.

**Rozbieżność raster–RT.** `shaders/raytracing/surface_shading_inc.slang:120`
startuje cień z rasterowej powierzchni z `TMin=0.001`. Uproszczona powierzchnia
może znaleźć się po drugiej stronie początku promienia. Przy agresywnych
ustawieniach możliwe są błędy własnego cienia, kontaktu i cienkich prześwitów.
W pierwszym planie właściciel reguluje ten kompromis tolerancją RT; nie dodawać
automatycznego solvera ani dużego biasu maskującego błędy. Walidacja ma pokazać
zakres jakości ustawień, nie obiecywać poprawności dowolnego uproszczenia.
[Znane ograniczenie różnych LOD-ów raster/RT](https://developer.nvidia.com/blog/?p=50632).

**Dekodowanie trafienia.** `shaders/raytracing/geometry_decode_inc.slang:48`
mapuje cluster ID na pierwszy oryginalny trójkąt. Uproszczony trójkąt wymaga
nowej wspólnej tożsamości asset/cluster/local triangle i odpowiednich atrybutów.
Zmiana musi przejść przez coverage, native material hit, RTXDI/DDGI/PT oraz SBT
(`render_raytracing.cpp:3489`). Zachować sidedness, UV i dane instancji.

**Emisja.** `render_raytracing.cpp:2474`, `:3248` wiążą światła z geometrią i
oryginalnym primitive ID. `shaders/raytracing/native_light_inc.slang:32`
sprawdza zgodność obu identyfikatorów w wąskim przedziale odległości trafienia.
Uproszczenie emitera bez zmiany tego kontraktu psuje pozycję/PDF albo odrzuca
światło. Rekomendacja ograniczająca zakres: powierzchnie potencjalnie emisyjne
automatycznie utrzymują liście DAG-u, z trwałymi identyfikatorami próbkowania.
Dotyczy też shaderowej emisji i zmian materiału; nie wystarczy chwilowy kolor
emisji równy zero. To ogranicza oszczędność pamięci na emiterach. Uproszczona
emisja wymaga osobnego projektu zgodności próbkowania z wybraną geometrią.

## 7. Rezydencja, koszt AS i historia

Wspólny manager stron zbiera sumę potrzeb raster/RT i wszystkich viewportów.
Korzenie, strony atrybutów wymagane przez emitery, tablice, CLAS, BLAS/TLAS,
scratch, bufor uploadu i historia także wchodzą do budżetu. Nie obiecywać
stałego globalnego limitu VRAM na podstawie limitu samych stron.

GPU zgłasza brakujące strony; CPU asynchronicznie odczytuje/dekompresuje i
planuje limitowane uploady. GPU publikuje nowe strony po zakończeniu transferu
i potrzebnych buildów CLAS. Dopiero komplet wymaganej grupy pozwala zastąpić
grubszą reprezentację. Opóźnienie I/O, pełna kolejka albo brak miejsca nie mogą
wycinać mesha. Uwzględnić budżet RAM/dekompresji i histerezę evictionu.
[Przykład cyklu streamingu](https://github.com/nvpro-samples/vk_lod_clusters/blob/22301a0cb682eec543dd851a6502c26ffdf436f5/docs/streaming.md).

Zacząć od jawnie ograniczonych pul i pośrednich identyfikatorów stron; sparse
buffers nie są wymaganiem tej pierwszej wersji. Rozdzielić warstwę alokacji od
ID, żeby późniejsza zmiana alokatora nie zmieniała assetów. Sprzętowy rozmiar
CLAS jest znany po buildzie; rezerwacje hosta mogą być dużo większe. Dobór
alokacji/kompaktowania wymaga pomiaru, a nie skopiowania limitów sampla.
[Alokacja CLAS](https://github.com/nvpro-samples/vk_lod_clusters/blob/22301a0cb682eec543dd851a6502c26ffdf436f5/docs/clas_allocation.md).

Niezmienione klastry współdzielą CLAS między instancjami. Cut/BLAS powinien być
ponownie używany, gdy wybór się nie zmienił. Współdzielenie BLAS między różnymi
cutami może zmniejszyć buildy kosztem większej liczby trójkątów; nie zakładać
automatycznej wygranej i nie wdrażać wszystkich wariantów cache/merge naraz.
[Analiza BLAS sharing](https://github.com/nvpro-samples/vk_lod_clusters/blob/22301a0cb682eec543dd851a6502c26ffdf436f5/docs/blas_sharing.md).

Zwolnienie wymaga usunięcia odwołań i zakończenia rzeczywistego użycia GPU przez
wszystkie widoki/AS. Obecne `_release_cluster_blas():1141`,
`_sweep_dead_cluster_surfaces():1177`, `prepare_frame():635` pokazują odrębnych
właścicieli zasobów. Zachować rejestrację BDA dependencies
(`render_raytracing.cpp:3410`, `:3523`). Nie zastępować fence/retirement arbitralnym
odczekaniem dwóch klatek.

Zmiana cutu potrzebuje jawnej generacji: obecny hash BLAS RID (`:3061`) nie
wykryje przebudowy zawartości pod tym samym RID. Odróżnić zmianę reprezentacji
od relokacji pamięci. Ustalić lokalne odświeżanie sond po zmianach geometrii oraz
reguły historii RTXDI/PT/NRD/RR; sam epoch ustawień DDGI nie pokrywa streamingu.
Ruch rigid zachowuje poprzednią transformację, a zmiana topologii nie może
podszywać się pod stabilny primitive ID.

## 8. Diagnostyka i przyszłe rozszerzenia

Minimum rekomendowane: kolorowanie faktycznie wybranych klastrów, przełącznik
zbioru raster/RT i zamrożenie selekcji przy swobodnym oglądaniu sceny. Kamera
inspekcji nie zmienia zamrożonego cutu. Wireframe/triangle view może być drugim
trybem, ale pokolorowanie obiektów lub oryginalnej siatki nie spełnia wymagania.
Pokazać liczbę wybranych trójkątów/klastrów osobno dla raster i RT oraz pamięć,
zaległe strony, odrzucone żądania i liczbę buildów BLAS. Używać stabilnych ID do
kolorów, żeby zmiana fizycznego slotu pamięci nie wyglądała jak zmiana LOD-u.

Wpięcie UI: `ViewportDebugDraw` (`servers/rendering/rendering_server_enums.h:575`),
bindingi RenderingServer i Viewport (`rendering_server.cpp:3002`,
`scene/main/viewport.cpp:5532`), ich XML oraz menu edytora
`editor/scene/3d/node_3d_editor_viewport.cpp:4707`, `:6822` i odpowiadający nagłówek.
Obecny debug klastrów dotyczy świateł, a wireframe nie pokazuje geometrii RT.
Rekomendowany RT view: promienie z kamery inspekcji kolorują faktycznie trafione
klastry wybranego RT cutu; nie oznacza wdrażania odbić. `rt_material_hit.slang:87`
ma `GetClusterID()`, ale payload przy `:96` gubi cluster ID — trzeba przekazać
go do debug output, zamiast rekonstruować z oryginalnego primitive ID.

Dla przyszłego foliage/wokseli wystarczą teraz trzy granice: instancja świata
nie jest stroną trójkątów; format danych geometrii nie zawiera polityki światła;
selekcja zwraca odwołania do pracy, a konsument raster/RT rozwiązuje je przez
wspólny kontrakt materiału i tożsamości. Używać aktualnych konkretnych typów dla
DAG i istniejącej deformowanej geometrii, bez nieużywanych wariantów voxel.
Nie gwarantuje to zerowych zmian przy dodaniu nowej reprezentacji, ale ogranicza
potrzebę zmiany formatu świata, materiałów i całego zarządzania instancjami.

## 9. Materiał do kolejności planu i pozostałe decyzje

Zależności: kontrakt zasobu/ID i eligibility → builder/import/reimport/eksport →
trwała scena i rezydencja → RD indirect/AS → wspólna selekcja z osobnymi cutami →
raster i RT/material decoding → polityka błędu/history → diagnostyka i końcowa
walidacja. Debug view warto uruchomić już przy pierwszej selekcji. Każdy etap
zastępuje odpowiednią starą forkową authority; końcowy stan nie utrzymuje dwóch
statycznych ścieżek dla tej samej geometrii. To kolejność zależności do planu,
nie instrukcja rozpoczęcia implementacji.

Przed uznaniem planu za wykonawczy trzeba dopiąć:

1. Dokładny schemat manifestu/payloadu i native export closure, właściciela
   zasobu i zachowanie `ArrayMesh` arrays/save bez wymuszonego pełnego readbacku.
2. Konkretny pin buildera i zakres ochrony custom attributes. Parametry rozmiaru
   grup/stron są do dobrania, nie należy z góry traktować sampla jako optimum.
3. Kontrakty GPU instance/material/SBT i count-indirect oraz obsługę passów
   depth/surface/motion/alpha dla istniejących materiałów. Nie wystarczy podgląd
   jednokolorowych trójkątów.
4. Rejestrację, jednostki, zakres i defaults dwóch prostych ustawień RT;
   zachowanie zmian emisji i historii. Nie projektować zaawansowanej selekcji
   według wpływu na oświetlenie. Bez pomiarów nie ma podstaw obiecywać konkretnej
   redukcji ani poprawnego oświetlenia dowolnego mesha przy agresywnych ustawieniach.
5. Zachowanie mesha po modyfikacji vertex/index/attribute data:
   `mesh_storage.cpp:655`, `:671`, `:687`, `:703` dziś nie aktualizują kopii
   klastrowej. Unieważnienie DAG-u i atomowe przejście do poprawnej reprezentacji
   musi poprzedzać jego dalsze użycie.

Walidacja końcowego planu powinna obejmować build edytora/template i double
(dotykamy transformacji), import/reimport OBJ i glTF, zapis oraz eksport PCK,
rzeczywisty Vulkan z aktualnym binarium, ruch/teleport/multi-viewport, szwy i
cienkie otwory, cienie spoza kamery, DDGI, emisję, instancing, zachowane deformacje,
streaming przy ograniczonej pamięci i usuwanie zasobów podczas lotu GPU.
Porównania tej samej trasy: CPU submission, traversal/culling, draw, CLAS/BLAS/TLAS,
trace/shading, RAM/VRAM i transfery. Sam licznik trójkątów nie jest wynikiem
wydajnościowym. Automatyczne testy nie zostały zamówione i nie są proponowane.

## 10. Granice ustaleń zewnętrznych

Sprawdzone 2026-09-08: `vk_lod_clusters` HEAD
`22301a0cb682eec543dd851a6502c26ffdf436f5` (2026-08-28), meshoptimizer HEAD
`0870c3881655df9b7d22faa35c825393534416bc` (2026-09-08), dawny builder HEAD
`e27f05a8cb3d4537f0fd4dd40f6f28d0fcd8fb7b` (2025-11-11). Piny odczytano z API
GitHub; wskazane kod/dokumenty były czytane, nie kompilowane tutaj.

Sample NVIDIA stanowi wykonalny wzorzec algorytmu i GPU submission, nie gotowy
moduł Godota. Rozdziela raster i RT, ma uproszczone materiały i własny cache.
Jego optymalizacja odrzucania pozycji po budowie CLAS w ścieżce RT nie może być
automatycznie przeniesiona do naszej hybrydy, która potrzebuje pozycji także do
rasteryzacji. [Opis sampla](https://github.com/nvpro-samples/vk_lod_clusters/tree/22301a0cb682eec543dd851a6502c26ffdf436f5).
