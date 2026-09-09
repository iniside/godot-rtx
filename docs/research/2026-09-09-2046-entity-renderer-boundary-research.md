# Granica encje–renderer: bezpośrednia przebudowa czy adapter

2026-09-09 UTC. Baseline `986f28ec3c368b6a67a32f103d990c8297299ec8`.
Research read-only kodu; bez zmian silnika, buildów, testów i pomiarów.

## Pytanie i kryterium

Właściciel dopuszcza dalszą bezpośrednią przebudowę renderera, jeśli będzie
prostsza od dokładania abstrakcji dla nowego modelu sceny. Polecił tę tezę
zbadać, nie zaakceptować bez sprawdzenia. Nie jest to autoryzacja przebudowy.

Kontekst: scena świata 3D w Flecs, bez Node/Object wrapperów encji, domyślnie
płaskie gęste populacje, późniejszy streaming 100 km+, brak scen 2D, osobne
UI HTML/CSS. Zachowanie niskopoziomowych zasobów i bezpiecznej własności GPU
nie wymaga zachowania obecnego modelu instancji świata.

Oceniamy osobno najmniejszy koszt pierwszej integracji, docelową liczbę
kontraktów/konwersji i uzasadniony zakres wymiany. Odczyt kodu nie pozwala
obliczyć zysku FPS ani udowodnić krótszego czasu wdrożenia całej opcji.

## Co wcześniejsza odpowiedź pomijała

`RendererSceneCull::Instance` już jest zwykłą strukturą C++, a nie Objectem
ani Node'em. Scenario ma tablice danych i bounds oraz indeksy BVH. Integracja
ECS przez obecne RID API nie tworzyłaby automatycznie zakazanego Node wrappera.
Teza, że renderer nadal odwzorowuje SceneTree jako drzewo Node'ów, jest błędna.

Jednocześnie brak Node'ów nie oznacza braku kosztów przedstawiania tych samych
instancji na kolejnych etapach: istnieje ogólna reprezentacja Cull, reprezentacja
geometrii Forward+ i trwałe rekordy RT. Część ich danych służy potrzebnej
widoczności, materiałom, historii i lifetime; część granic można skonsolidować.

## Wejście i przekazanie klatki — zweryfikowane źródła

| Źródło | Zachowanie | Wniosek |
| --- | --- | --- |
| `rendering_server_default.h:967,978,984` | Metody instancji trafiają do RSG::scene; transformacja przez FUNC2 | ECS wywołujący settery zachowuje obecny ingress per instancja i właściwość |
| `servers/server_wrap_mt_common.h:267` | FUNC2 kolejkuje wywołanie lub flushuje pending i wywołuje bezpośrednio | Nie ma dowodu, że każdy setter synchronicznie czeka na render thread; jest natomiast osobnym wywołaniem/komendą |
| `rendering_server_default.cpp:361,374` | Tworzy RendererSceneCull jako RSG::scene i przekazuje mu osobny RendererSceneRender | Istnieje konkretna granica wymiany przygotowania sceny bez wymiany całego backendu |
| `rendering_server_default.cpp:109,134,148` | Ustawia dane klatki, aktualizuje scenę, następnie renderuje viewport | Nowy właściciel sceny musi dostarczać gotowe dane przed konsumentami renderowania |
| `rendering_server_globals.h:52`; `rendering_server_default.cpp:717` | FrameContext ma interpolację, numery klatek i Streamline | To nie snapshot encji/sceny. Nie przedstawiać go jako gotowej ekstrakcji Flecs |
| `rendering_server_default.cpp:665,686,700,740` | Frame slots ograniczają admission; draw przechwytuje kontekst i kolejkuje pracę; end zwalnia slot po kolejce | Zachować własność klatki i punkt przekazania; ECS nie daje prawa render thread do dowolnego czytania mutowanej sceny |
| `rendering_server_default.h:985,1005`; `rendering_server.cpp:79,93` | Attach/cull przekazuje ObjectID; binding zwraca te same wartości jako int64 | Wynik pickingu musi otrzymać natywną tożsamość renderowania/encji, nie ecs_entity_t podszywający się pod ObjectID |

Niepełne ścieżki w tabeli powyżej są pod `servers/rendering/`, z wyjątkiem
wyraźnie podanego `servers/server_wrap_mt_common.h`.

## Uczciwe porównanie wariantów

### Fakty o obecnym frontendzie, istotne dla porównania

Kotwice tej tabeli są z commita `986f28ec3c`, nie z równolegle zmienianej
working copy. Pliki pod `servers/rendering/`.

| Źródło | Fakt | Co wynika, a czego nie można obiecać |
| --- | --- | --- |
| `renderer_scene_cull.h:403`, Instance | Plain struct z transformacją, RID assetów/skeletonu, bounds, flagami, scenario i dependencies | Można skonsolidować dane z modelem encji; nie jest to usunięcie Object/Node, bo takich tu nie ma |
| `renderer_scene_cull.cpp:698`, InstanceGeometryData | Tworzy RenderGeometryInstance backendu i przekazuje materiały/skeleton/transform/bounds; aktualizacja transformacji :1727 | Istnieją osobne etapy Instance → backend geometry. Ominięcie jednego nie usuwa automatycznie drugiego |
| `renderer_scene_cull.cpp:957`, instance_set_transform; :513, _instance_queue_update | Identyczna macierz wraca od razu; dirty flags i wpis kolejki są scalane | Nie twierdzić, że każdy setter przebudowuje całą instancję. Dla nieruchomej populacji koszt ingress może być mały |
| `renderer_scene_cull.cpp:3827`, _update_dirty_instance | Bounds i dependencies aktualizowane warunkowo; śledzenie materiałów/skeletonu zależne od flag | Nowy frontend musi zachować invalidation; nie porównywać go z fikcyjną pełną aktualizacją starego |
| `renderer_scene_cull.cpp:1698`; `renderer_scene_cull.h:794`, PairInstances | Zmiana → AABB, transform backendu, BVH, pary; alokacja kandydatów i zwalnianie starych rekordów par | Konkretny zakres do zmiany reprezentacji/batchowania, ale potrzeba widoczności i powiązań nie znika |
| `renderer_scene_cull.h:327`; `renderer_scene_cull.cpp:2896,2918` | Scenario ma PagedArray bounds/data, dwa BVH i płaską pętlę cullingu | Nowa integracja nie będzie pierwszym przejściem renderera z drzewa na tablice |
| `renderer_scene_cull.cpp:4056`, free; gałąź :4085 | update_dirty_instances przed i po odłączeniu pojedynczej instancji | Batch unload może ograniczyć liczbę globalnych drainów; ich rzeczywisty koszt nie został zmierzony |

Zachować semantykę asset invalidation i base teardown (`.h:489`, `.cpp:566`),
deformacji i skeleton bounds (`.cpp:537,1116,1990`), masek i shadow dirty
(`.cpp:171,1702`), teleport/reset motion (`.cpp:1727`) oraz usuwania BVH/par i
poprawiania indeksów po swap-remove (`.cpp:1911`). Są to obowiązki nowego
frontendu, nie argument za utrzymaniem starego obok niego.

### Konsumenci backendu — gdzie bezpośrednia zmiana musi sięgać

Kotwice tej tabeli pochodzą z working copy z aktywnymi zmianami worker
preparation. Prefiks RFC to `servers/rendering/renderer_rd/forward_clustered/`
`render_forward_clustered`, RT to ten sam katalog i `render_raytracing`.
To nie linie commita `986f28ec3c`; receipt poniżej rozróżnia oba źródła.

| Źródło | Rzeczywisty kontrakt | Zakres bezpośredniej wymiany |
| --- | --- | --- |
| `servers/rendering/renderer_geometry_instance.h:40,88` | RenderGeometryInstance to natywna klasa C++, nie Object; zawiera transform/bounds/RID i stan renderowania | Sam brak Objecta nie jest powodem usunięcia; konsolidację uzasadniać konsumentami |
| `storage_rd/render_data_rd.h:54,102` pod renderer_rd | Osobne tablice raster instances i rt_instances, osobne lights/rt_lights | Zachować rozdział widoczności; lista kamery nie zastępuje listy RT |
| RFC `.h:660,676,684`; `.cpp:5030` | GeometryInstanceForwardClustered ma osobne Data, historię i cache powierzchni wskazujące ownera | Sam nowy producent RenderDataRD nie usuwa tej reprezentacji |
| RFC `.cpp:1433`, _fill_render_list; :791, _render_list_template | Klasyfikacja powierzchni/materiałów/GI/motion i późniejsze dereferencje surface->owner od :828 | Wymiana reprezentacji wymaga zmiany rzeczywistych konsumentów, nie tylko adaptera list wejściowych |
| RT `.cpp:4917`, update_persistent_instance; RT `.h:411` | Konwersja geometrii do RTPersistentInstanceData: ID, transformy, bounds, flagi, surface handles/adresy GPU | Można produkować rekordy bez ogólnego ownera geometrii, zachowując sloty/generacje/retirement |
| RT `.cpp:3145`, build_tlas | Czyta rt_instances, geometry owners/cache; zbiera materiały, deformations, transformy i zadania microgeometry | To istotny zakres refaktoru konsumenta do natywnych danych, nie cienka warstwa zgodności |
| RT `.cpp:2546,2603,2637`, build_acceleration_structures | BLAS RIDs i numeryczne deskryptory prowadzą do tlas_build | Tego poziomu nie trzeba uzależniać od Flecs |
| RFC `.cpp:447`, _prepare_micro_geometry; micro_geometry_selection.h:39,57 | Zadania mają uchwyty instancji/surface/asset, counts, bins, flags i opcjonalny adres indirect; parametry mają kamerę/scenario/maski/budżety | Natywny producent może tworzyć te same potrzebne dane bez odtwarzania owner-pointer modelu |
| RFC `.cpp:709,783`, _render_micro_geometry | Konsumuje bins, selection buffers/pipelines; wykonuje indirect-count draw | Zachować selekcję GPU i draw path, zmienić producenta danych |

Warunkiem B jest wspólna wymiana producenta i zależnych od `surface->owner`
konsumentów. Samo ominięcie Cull z dalszym tworzeniem całej poprzedniej sceny
geometrii nie spełnia celu skonsolidowania tego łańcucha. Nie wynika z tego
nakaz skasowania każdej struktury RenderGeometryInstance — można przekształcić
jej odpowiedzialność i układ danych zamiast przenosić to samo pod nową nazwę.

Zachować co najmniej następujące dane i mechanizmy:

- Persistent slots/generations i retirement po ukończonym submission (RT
  `.cpp:4924,5091`), referencje buforów mesh/microgeometry (`:4964`), stabilną
  tożsamość powierzchni (`:5031`) i generacje/programy materiałów (`:5056`).
- Wspólne cache konwencjonalnych BLAS w process_surface (RT `.cpp:773`) i
  materiałów w process_material (`:1327`). Pozyskiwanie assetu przez owner
  można zastąpić; cache assetu nie jest drugim światem ECS.
- Microgeometry selection, współdzielone cut/BLAS, page pins i CLAS dependencies:
  RT `_prepare_micro_geometry:1850`, `_build_micro_geometry:2038`, BLAS dla
  reprezentanta cut `:2322`, aktualizacja użytkowników `:2412`. Encja nie
  powinna otrzymywać własnej kopii DAG-u lub współdzielonego BLAS.
- Deformations: bieżący/poprzedni bufor vertex, counters i generacja mesh
  instance (RT `.cpp:3308`); transform motion i teleport (`:3658`).
- Klasyfikacja shaderów/powierzchni (RFC `.cpp:426,4357`) oraz invalidation
  mesh/material/particle/skeleton (`:5001`). Encja nie czyni każdego mesha
  automatycznie zgodnym z rigid microgeometry.

Nie można usunąć wszystkich powiązań Cull na podstawie kierunku RTXDI/DDGI.
Sprawdzony Forward+ zwraca maskę `INSTANCE_VOXEL_GI` (RFC `.cpp:5167`), zapisuje
dwa probe RIDs (`:5246`), a ich konsumenci istnieją w zwykłej liście renderowania
(`:1604`), zadaniach microgeometry (`:529`) i klasyfikacji GI (`:4723`). Puste
per-geometry pair_light/reflection/decal w `.h:740` nie oznaczają usunięcia
scene-wide lights/decals/fog (`.cpp:2153,2154,2134`). Softshadow/projector state
(`:5260`) nadal wpływa na flagi materiału/pipeline (`:4495`). Zachować wymagane
informacje albo jawnie objąć usunięcie danej funkcji zakresem osobnej decyzji.

### Ocena wariantów

| Wariant | Co daje | Co pozostawia/kosztuje | Ocena |
| --- | --- | --- | --- |
| A. Flecs wywołuje istniejące instance_* na RID | Najmniejszą zmianę pierwszego przekroju; wykorzystuje aktualny lifecycle, culling i asset dependencies | Zachowuje per-instance ingress i kolejne reprezentacje CPU; potrzebuje rozwiązania pickingu | Technicznie poprawny i zgodny z brakiem Node wrapperów, ale nie redukuje istotnie modelu sceny renderera |
| B. Bezpośrednio przebudować przygotowanie sceny pod natywne dane encji | Pozwala usunąć zastępowaną reprezentację/ingress, scalić identyfikację i przekazywać zmiany zbiorczo | Wymaga jawnego przeniesienia odpowiedzialności Cull i konsumentów geometrii; więcej zmian początkowo niż A | Kierunek docelowy uzasadniony zakresem wymiany całej sceny; nie nowy ogólny adapter ani równoległa scena |
| C. Przepisać backend GPU, shadery i zarządzanie zasobami na Flecs | Sam wybór ECS nie wskazuje potrzebnego nowego zachowania tych warstw | Powiela obecne rozwiązania materiałów, RT, residency i synchronizacji; zwiększa zakres | Brak uzasadnienia w prześledzonej granicy sceny |

Rekomendacja B dotyczy mniejszej liczby docelowych kontraktów świata i
konwersji, nie dowiedzionego krótszego wdrożenia lub większej liczby FPS.
Jeżeli kryterium ograniczyć wyłącznie do najszybszego pierwszego obrazu encji,
A jest prostsze. Przy docelowej wymianie całego modelu świata warto rozważyć
bezpośrednią zmianę istniejącej odpowiedzialności, zamiast najpierw utrwalać
most do ogólnego modelu instancji.

## Granice bezpośredniej integracji

Flecs posiada stan świata. Renderer nadal potrzebuje pochodnych danych
renderowania: stabilnych slotów, list/buforów pracy, historii poprzedniej klatki,
powiązań materiałów/assetów i informacji o retirement. Nie są drugim
autorytatywnym światem. Usunięcie każdej kopii transformacji nie jest celem,
jeżeli kopia reprezentuje inną chwilę symulacji albo klatkę w locie.

Bezpośrednio oznacza zmianę istniejącej granicy sceny i jej rzeczywistych
konsumentów. Nie oznacza przekazania wskaźników do komponentów Flecs prosto do
zadań renderera i pozwolenia na równoległe zmiany strukturalne. Staging Flecs
reguluje zmiany świata, nie lifetime buforów i zasobów GPU.
[Kontrakt systemów/staging Flecs](https://github.com/SanderMertens/flecs/blob/fb55f3c25660425cfe1bc4cf5e6bff8b3f18a9b8/docs/Systems.md).

Nie narzucać ogólnego `SceneAdapter`/`RenderBridge` ani drugiej implementacji
świata tylko dla zgodności. Zmiany zbiorcze i stan konkretnej klatki należą do
obecnej odpowiedzialności renderera. API publiczne upstream i użycia edytora
wymagają osobnego rozstrzygnięcia zakresu, nie cichego ich usunięcia.

## Receipt źródeł i ograniczenia

Frontend zweryfikowano także przez `git show 986f28ec3c`, ponieważ późniejsze
linie cull.cpp/.h przesunęły się przez cudze zmiany worker cullingu. Stabilne
kontrakty backendu porównano z baseline: arrays RenderDataRD :54/:102 oraz
GeometryInstance owner w RFC.h :676/:684 są zachowane. Persistent conversion
ma w baseline RT.cpp:4469, a w odczytanej working copy :4917; tworzenie BLAS
cut odpowiednio :2303 i :2322. To nie nowe mechanizmy dopisane przez research.

SHA256 odczytanych plików backendu, pod `servers/rendering/renderer_rd/`:

| Plik | SHA256 |
| --- | --- |
| forward_clustered/render_forward_clustered.cpp | 3DC6EE0AFEB701E251B743E7A343E473893DC960CE2ACD2D74287B34BEED530C |
| forward_clustered/render_forward_clustered.h | E4404F3BF1A784929AFB7B3E54FD5ACD66CA9BC26BC196F433C8A79B41D133F2 |
| forward_clustered/render_raytracing.cpp | 686DE90CFE60967F15DB56D2C7E698CD53B58844616FBDDF15201F54E60135BA |
| forward_clustered/render_raytracing.h | 479E52CD1AD177153D170BD49B9E09AB16CC65D4C7389BA2D799CDDFF485C8F6 |
| forward_clustered/micro_geometry_selection.h | FEF3B2DF9501DF7424CEE919E0984D9F5DD5AEF80ECF0857B39D14D7E501AF0A |
| storage_rd/render_data_rd.h | 16DE2BE253FF11A0731FD152EFAE9A0892620D9C05C5E6E8B23E629C5B20481A |

RenderingServerDefault.cpp/.h oraz FrameContext były niezmienione względem
baseline podczas odczytu. Jego FrameContext nie jest snapshotem sceny;
zachowanie kolejki i admission nie rozstrzyga jeszcze układu danych ECS.

## Granice weryfikacji

Nawigacja: clang-nav z root compile_commands.json; find/docsym/hover/def,
następnie odczyty znalezionych implementacji, bindingów i scoped git diff.
Analizowano rzeczywiste ścieżki, nie samą dostępność publicznego RID API.
Frontend i backend badały dwa osobne konteksty read-only (Astra high);
główny kontekst sprawdził ingress, FrameContext, admission i identity binding.
Wyniki nie stanowią kompletnego audytu wszystkich rendererów Godota.
Zadania równoległe zmieniają culling/Forward+ pod workery. Rozróżniamy
baseline od odczytów working copy; nie przypisujemy ich zmian temu zadaniu.

Brak nowych pomiarów porównujących A/B. Istniejące wyniki profilowania
microgeometry nie są takim porównaniem. Nie wykonano implementacji,
kompilacji ani automatycznych testów. Dokładny zakres publicznych API,
wszystkich konsumentów oraz akceptacji wykonania należy zamknąć w planie.
Otwarte szczegóły: właściciel danych deformacji w nowej scenie, pełny zakres
native lights/decals/fog, zachowane funkcje GI/projector/shadow i dokładny model
udostępniania danych Flecs do klatki. To nie uzasadnia uniwersalnego mostu;
to konkretne kontrakty niezbędne do usunięcia starej odpowiedzialności.
