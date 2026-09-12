# Loader: redundant work, single-thread cost, asynchronous cell loading

Data: 2026-09-12 UTC (rewizja 4: przepustowość ładowania komórek). Rewizja badań: `f63243af4e`. Autorytety: `AGENTS.md`,
`.agents/shared/planning-dispatch.md`, `docs/reference/plan-writing-workflow.md`,
`.agents/shared/godot-rules.md`, [2026-09-12-1139-world-storage-text-cells-plan.md](2026-09-12-1139-world-storage-text-cells-plan.md).

## Kontekst i kształt zamówienia (właściciel, 2026-09-12)

Kolejność: **1. usunąć redundantną pracę, 2. zoptymalizować to, co możliwe
na jednym wątku, 3. w pełni asynchroniczny, nieblokujący load danych.**
Właściciel wskazał, że `install` (250 ms na komórkę 2250 encji) nie może
być kosztem tworzenia encji flecs; słusznie: to koszt warstwy schematu.

Dodatek właściciela (2026-09-12): na koniec **realna mapa stress**: komórki
256 m, każda z maksymalnym zagęszczeniem meshy Lucy/Thai (~10 k na komórkę),
zasięg streamingu 500 m, siatka 16 × 16 komórek.

Korekta właściciela (2026-09-12, po pierwszym otwarciu mapy 16 × 16: 22 GB
i minuty w `EntitySceneIO::load`): **scena nie listuje ani nie parsuje
niczego globalnie**. Nazwa katalogu komórki wynika z koordynatów, więc
scheduler liczy z pozycji kamery i zasięgu dokładne klucze komórek i sprawdza
istnienie tylko tych katalogów. Otwarcie = plik główny + `global/`.
Zawartość komórki czyta worker dopiero, gdy komórka wchodzi w zasięg; zwolniona
komórka jest zapominana. To zastępuje D4 planu world storage.

Wykluczenia: bez zmiany formatu tekstowego (paczkowanie runtime to osobny
task); bez zmiany semantyki komórek, `dirty`, histerezy i budżetu z planu
world storage; bez ProjectSettings; bez testów; bez nowego wątku
streamingu poza `WorkerThreadPool`.

## Stan faktyczny (pomiar komórki 2250 encji, stress, `--benchmark`)

| Faza | ms | Co robi |
| --- | --- | --- |
| `read_record` | 645 | `read_text` (UTF-8 → String całego pliku) + `VariantParser::parse` + `duplicate(true)` dla rekordów z cache klastra (`entity_scene.cpp:234-277`); surowe otwarcie+odczyt tych 2250 plików: 133 ms |
| `describe` | 99 | `_describe` → `_validate_fields` (`entity_scene.cpp:417-445, 348-415`): pełny **dekod każdego pola do tymczasowej wartości** (`field.validate`, `entity_component_schema.h:399-402`), klucze hex budowane `String::num_uint64` per pole, `get_key_list()` per rekord |
| `world` | 147 | `write_component` → `set_serialized` → `entity_decode_struct` (`entity_component_schema.h:426-446`): **`EntityComponentTraits<T>::fields()` buduje od nowa `Vector<EntityFieldSchema>` przy każdym wywołaniu** (generowane, `entity_component_schema.gen.cpp:5-…`, w tym `C defaults` + encode wartości domyślnej per pole), rekurencyjnie dla typów zagnieżdżonych (`EntityPose` → `EntityPosition`); drugi dekod tych samych pól po `validate` |
| `commit` | 16 | `_commit` kopiuje komponenty ze scratch world do świata dokumentu przez `copy_to` po **przejrzeniu wszystkich ~40 schematów `ecs_get_id` per encja** (`entity_scene.cpp:548-556` wzorzec w `_prepare`, `_commit`) |

Konsumenci `fields()`: `entity_encode_struct` (:409-423), `entity_decode_struct`
(:425-448), `entity_make_component` (:457, jedna kopia w `EntityComponentSchema::fields`),
`set_field` (:477); codegen `misc/scripts/entity_component_codegen.py:121,128`.
`Section::components` (lista hex) czytana w `entity_scene_commands.cpp:257,681`
i budowana w `_describe` oraz `entity_scene_io.cpp:535`.

Wątki: `EntityWorld::_is_owner()` na każdej mutacji (`entity_world.h:32-139`);
`EntityScene::_owner()`; `ResourceLoader::load` jest bezpieczny z wątków
roboczych (`thread_load_mutex`, `resource_loader.cpp:230,344,700-763`,
ładowanie z pod-wątków to istniejąca ścieżka edytora); precedens
`WorkerThreadPool::add_native_task` w forku: `micro_geometry_storage.cpp:772`
(odczyt stron); `ResourceLoader::load` z zadania puli zleca zagnieżdżone
zadanie i czeka (`resource_loader.cpp:730-748`). Rejestr schematów to
`EntitySchemaRegistry` (`entity_component_schema.h:56`), dziś budowany tylko
z `flecs::world` (:66); `EntitySceneIO::load` tworzy do tego jednorazowy
`EntityWorld` (`entity_scene_io.cpp:458-459`), a `~EntityWorld` woła
`finalize_services()` (RenderingServer/NavigationServer,
`entity_world.cpp:20-30,67-80`), więc taki świat powstaje i ginie tylko na
wątku właściciela. **Id komponentów flecs są per świat**
(`thirdparty/flecs/flecs.h:32064-32085`); `_commit` już dziś rozwiązuje
schemat w świecie przygotowanym przed `ecs_get_id` (`entity_scene.cpp:625-627`).
`_commit` (`entity_scene.cpp:584-670`) wykonuje też: unlink/relink rodzica
i `set<flecs::Parent>`, `order.insert`, `_component_changed`, `_index_prefabs`,
`_assign_cell`, podbicie `revision` (nie przy ładowaniu). `create_play_document`
(`:845-884`) czyta każdy rekord (`_read_stored` dla nierezydentnych) i woła
`_describe` z pełną walidacją. `_read_record` (`:341-343`) wpisuje `parent`,
`deleted`, `order` do słownika zwróconego przez `_read_stored`. Liczniki
`entity_asset_usec/loads/cache_hits` to zwykłe statyki
(`entity_component_schema.cpp:68-70,100-108`).
Statystyki klatki (p95/p99) daje narzędzie ruchu kamery
(`misc/scripts/editor_camera_motion.py`, `--wait`).

## Decyzje projektowe

**D1. Schemat statyczny (etap 1).** Codegen emituje
`static const Vector<EntityFieldSchema> &fields()` z function-local static
(inicjalizacja bezpieczna wątkowo, jedna budowa na proces); `EntityFieldSchema`
zyskuje gotowy klucz `String key` (hex) i `EntityComponentSchema` gotowy
`String key`. Wszyscy konsumenci (`entity_encode_struct`, `entity_decode_struct`,
`entity_make_component`, `set_field`, `_validate_fields`, `_describe`,
`EntitySceneIO::save/load`) używają referencji i gotowych kluczy zamiast
`String::num_uint64` i `hex_to_int` w pętlach. Opis komponentu staje się
**niezależny od świata**: `entity_make_component` dzieli się na statyczny
deskryptor (`id`, `name`, `key`, `fields`, `encode/decode`, `construct(void*)`
= placement new `T`, `destruct(void*)`, `size`, `alignment`, `copy_to`,
`set_serialized`, `set_field`) i część per świat (`runtime_id` z
`EntityCodec<T>::meta_type(world)`), którą `EntitySchemaRegistry` uzupełnia
przy rejestracji w danym `EntityWorld`. Deskryptory statyczne są dostępne bez
świata (`EntitySchemaRegistry::descriptors()`), z wątku roboczego, tylko do
odczytu.

**D2. Jeden przebieg walidacji i dekodowania (etap 1).** `_install` przestaje
wołać `_describe`; `write_component` → `entity_decode_struct` jest jedynym
walidatorem przy ładowaniu (odrzuca nieznany klucz, zły typ, złe zagnieżdżenie,
nieładowalny asset). `Section::components` powstaje z listy kluczy rekordu po
sprawdzeniu, że id jest znanym komponentem, w postaci **kanonicznej**
(klucz z deskryptora, nie surowy tekst rekordu; niekanoniczny klucz = błąd
jak dziś w `_describe:435`), a `Section::name` z pola `EntityName` rekordu
jak dziś w `_describe:422-431` (kod przeniesiony do `_install`). Jedna reguła
walidacji dla rekordów z dysku: `_validate_fields` przestaje wymagać
obecności każdego pola serializowanego (zachowuje: nieznany klucz, zły typ,
złe zagnieżdżenie, nieładowalny asset), więc `create_play_document`, zapis i
komendy widzą tę samą regułę co ładowanie. Konsekwencja do zatwierdzenia:
**brakujące pole serializowane przyjmuje wartość domyślną** (tak działa
`entity_decode_struct`; dotąd blokował to `_validate_fields`), co jest
dokładnie ulgą ergonomiczną zgłoszoną przy scenie testowej. Granularność
błędu ładowania spada z `typ/pole` do `typ` (komunikat z `_install`),
ścieżka pliku zostaje.

**D3. Bufor przygotowania zamiast scratch world (etap 2).** `_prepare` dla
`load_subset` nie instaluje do scratch `EntityWorld`; dekoduje rekord do
`PreparedEntity { id, parent, order, name, components: Vector<{schema id,
buffer}> }`, gdzie `buffer` to pamięć `size/alignment` z deskryptora,
**skonstruowana `construct`** (placement new) i dopiero potem zdekodowana
`decode`; bufory należą do `PreparedEntity` i są niszczone `destruct`
(w destruktorze, na każdej ścieżce błędu i przy odrzuceniu wyniku). Commit
na wątku właściciela rozwiązuje deskryptor w świecie dokumentu i wpisuje
przez istniejące `copy_to` (`set<T>` z kopii, `entity_component_schema.h`),
bez `ecs_set_id`/`ecs_get_type_info` i bez id z obcego świata. Atomowość
all-or-nothing zostaje: bufory są kompletne przed pierwszą mutacją świata.
Kopia po ~40 schematach znika. `_commit` zostaje **jedynym autorytetem
commitu**: dostaje abstrakcję zbioru przygotowanego (dziś: przygotowana
scena z komend/prefabów; nowe: bufory z ładowania) i wykonuje ten sam
komplet: rodzic (unlink/relink, `set<flecs::Parent>`), `order`,
`_component_changed`, sekcje, `_index_prefabs`, `_assign_cell`, `dirty`,
`revision` tylko dla komend. `_prepare` dla komend i prefabów zostaje na
`instantiate` jak dziś; scratch world znika, gdy nie ma już użycia.

**D4. Odczyt rekordu (etap 2).** `_read_stored` parsuje przez
`VariantParser::StreamFile` z `FileAccess` bez konwersji całego pliku do
`String`, albo przez `StreamString` na `String::utf8` — wybór po pomiarze
w Kroku 2 (obie ścieżki mierzone na komórce 2250); reguła własności rekordu:
`_read_stored` zwraca **nowy słownik najwyższego poziomu** (płytka kopia:
`parent`, `deleted`, `order`, `components`), więc `_read_record:341-343`
może w niego pisać, a `components` i słowniki zagnieżdżone są współdzielone z
cache klastra i **niemutowalne**; Krok 2 weryfikuje grepem, że żaden
konsument (`_install`, `_describe`, komendy, `create_play_document`, save)
nie pisze do zagnieżdżonych słowników rekordu z dysku, a miejsce, które
musi, robi `duplicate(true)` lokalnie. Format bez zmian.

**D5. Asynchroniczne ładowanie komórek (etap 3).** `request_cells` staje się
nieblokujące: dla każdej chcianej komórki (nierezydentnej, bez pracy w toku,
bez znacznika błędu) tworzy `CellJob` w `WorkerThreadPool::add_native_task`.
Zlecenie (wątek właściciela): lista rekordów = id komórki plus przodkowie
spoza komórki z `_collect_required`, z pominięciem rezydentnych; dla każdego
rekordu **niezmienny snapshot źródła** `{ id, parent, order, ścieżka
bezwzględna, flaga klastra, kopia Section::record jeśli rekord jest w
pamięci }`. Zadanie na wątku roboczym dotyka **wyłącznie** tego snapshotu,
plików, `VariantParser`, statycznych deskryptorów (D1), `ResourceLoader::load`
(mutex; zagnieżdżone zadanie puli, patrz ryzyka) i własnych buforów; nie
czyta żadnego pola `EntityScene` ani `EntityWorld`, nie woła `_fail`. Wynik:
`PreparedCell { key, entities, error, failing id/komponent }`. Na wątku
właściciela `commit_ready(budget)` w każdym ticku pobiera ukończone zadania
(każde zadanie jest zawsze zebrane `wait_for_task_completion`, także gdy
wynik idzie do kosza) i **rewaliduje względem aktualnego stanu**: encja,
która w międzyczasie stała się rezydentna (pin, `load_global`, `load_subset`,
komenda), jest pomijana (stan żywy wygrywa, bufor niszczony); przodek pominięty
przy zleceniu, który przestał być rezydentny, unieważnia komórkę (wynik
odrzucony, flaga "w toku" zdjęta, scheduler zleca ponownie z aktualnymi
przodkami). Commit idzie przez `_commit` (D3), przodkowie przed potomkami,
komórka jako całość: **komórka pozostaje atomowa** (nie ma stanu częściowo
rezydentnego; semantyka z planu world storage bez zmian), a budżet N encji
na tick ogranicza liczbę komórek commitowanych w jednym ticku (co najmniej
jedna). Koszt commitu to katalog + flecs + `_component_changed`, bez parsera
i dekodowania; jego pomiar na komórce 10 k (Krok 5) rozstrzyga, czy potrzebna
jest częściowa rezydencja, co byłoby zmianą semantyki i osobnym pytaniem do
właściciela. Anulowanie: `release_cells` komórki w toku oznacza zadanie do
odrzucenia. Domknięcie zadań (`flush_streaming()` = zebranie wszystkich zadań,
odrzucenie wyników): `EntitySceneIO::save` i Save-As przed pierwszą mutacją
dysku, `_relocate`, `create_play_document`, `~EntityScene`, zmiana dokumentu w
`Node3DEditor::set_scene_document`. Komendy/undo nie kolidują (brudne encje
są rezydentne, zadania czytają tylko czyste, nierezydentne). Błąd zadania
zgłaszany przez `_fail` przy commicie na wątku właściciela, komórka oznaczona
jako nieudana do zmiany `revision` (bez ponawiania co klatkę). Liczniki
profilu assetów stają się atomowe.

**D6. Scheduler i liczniki.** `EntitySceneStreaming::step` bez zmiany
sygnatury: `request_cells` (nieblokujące) + `commit_ready(budget)` +
`release_cells`. Liczniki `--benchmark`: zadania zlecone/ukończone/odrzucone,
encje zcommitowane na tick, czas wątku roboczego per zadanie, czas commitu
na tick; profil `load_subset` zostaje dla ścieżki synchronicznej
(`load_global`, `pin`).

**D7. Mapa stress 16 × 16 (generator).** Nowa komenda headless
`--generate-streaming-stress` obok istniejących `--convert-*`
(`main/main.cpp:1710,4123-4131`, `editor/entity_scene_energy_converter.h`)
generuje `demos/rtxdi_manual/streaming_stress.escn` + katalog: grid `stress`
o `size = 256`, `range = 500`, `default_grid = "stress"`; 16 × 16 komórek
(x, z ∈ −8..7, y = 0), w każdej 10 000 encji: naprzemiennie Lucy
(`uid://rl2u6vubqq1c`, `Basis` skala 0.00125231757 z obrotem jak w scenie
stress) i Thai (`uid://cuc6c6x7xrd4h`, skala 0.00504997965) na siatce
100 × 100 o rozstawie 2.56 m z deterministycznym jitterem (seed stały);
`global/`: environment (`native_assets/microgeometry_stress/environment.tres`),
słońce, kamera w środku mapy. **Mix obu form składowania w każdej komórce**
(właściciel): 5 000 rekordów w jednym pliku klastra
`cells/stress/<x>_0_<z>/statues.cluster.escn` i 5 000 rekordów jako
pojedyncze pliki `<id>.escn` w tym samym katalogu komórki (razem 10 000 na
komórkę; 256 klastrów + 1.28 M pojedynczych plików), zapisane bezpośrednio
przez `EntitySceneIO` z rekordów w pamięci per komórka (bez instancjonowania
2.56 M encji w dokumencie i bez `EntitySceneCommands`); rekordy minimalne
(`EntityName`, `EntityTransform`, `EntityMesh`, `EntityGeometry`), pola
domyślne pominięte dzięki D2. Główny plik zawiera `[types]` jak dziś.
Oczekiwane rozmiary: ~300 B na rekord ≈ 0.8 GB tekstu plus narzut NTFS
1.28 M plików; katalog przy otwarciu = odczyt 256 klastrów i 1.28 M plików
(to jest właśnie test limitu D4 z planu world storage: brak indeksu, pełny
odczyt; obie ścieżki mierzone osobno w licznikach); w zasięgu 500 m ~25
komórek = ~250 k rezydentnych encji z dwoma współdzielonymi `.mgdata`.
Generacja 1.28 M plików trwa minuty; generator raportuje postęp per komórka.

**D8. Odkrywanie po koordynatach (zamiana D4 world storage).**
- `EntitySceneIO::load` czyta plik główny, `global/` i `prefabs/`; nie
  wchodzi do `cells/`. Katalog dokumentu zawiera tylko encje `global/`,
  encje komórek rezydentnych lub w toku oraz encje brudne. Znikają:
  `get_cells()`, mapa wszystkich komórek, `_rebuild_cells` po całym drzewie;
  zostają `cell_for_position`, `cell_aabb`, `get_grid_*`, mapy komórek
  ograniczone do komórek znanych.
- Scheduler: dla każdej kamery i grida iteruje całkowite zakresy indeksów
  pokrywające kulę o promieniu `range` (i `range + size` dla histerezy),
  odrzuca komórki, których AABB nie przecina kuli, i dla pozostałych
  sprawdza `DirAccess::dir_exists` katalogu `cells/<grid>/<x>_<y>_<z>/`;
  wynik "brak katalogu" jest zapamiętywany do zmiany `revision` lub zapisu
  (bez sondowania dysku co klatkę). Komórki bez katalogu nie istnieją.
- Zadanie komórki: worker sam listuje katalog komórki (pliki `<id>.escn` i
  klastry), parsuje, dekoduje; snapshot z wątku właściciela to klucz,
  ścieżka katalogu i zbiór id już rezydentnych/brudnych w tej komórce (do
  pominięcia). Rodzic z rekordu musi być w tej samej komórce albo w
  `global/`; inaczej błąd ze ścieżką pliku. Commit wstawia rekordy katalogu
  (nowe wpisy) i materializuje jak dziś.
- `release_cells` **zapomina** czyste encje zwolnionej komórki (katalog,
  sekcje, mapy komórek); brudne zostają rezydentne. `unload_subset` bez zmian
  semantyki dla wywołań ręcznych.
- Zapis: encja z rodzicem trafia do komórki swojego korzenia (korzeń =
  najwyższy przodek; korzeń bez transformacji lub globalny → `global/`),
  więc cała hierarchia jest w jednym katalogu; reguła "przeniesienie
  potomków" upraszcza się do "hierarchia podąża za korzeniem". Save-As i
  zmiana ścieżki: kopia katalogu `cells/` starego drzewa (`DirAccess::copy_dir`
  per komórka nieznana dokumentowi) plus zwykły zapis brudnych; stare drzewo
  nietknięte.
- Dok encji pokazuje katalog (czyli rezydentne + globalne) i liczbę
  komórek rezydentnych; nazw nierezydentnych nie ma w pamięci.
- `get_dependencies` (skan edytora) zostaje strumieniowy po całym drzewie
  (`e2a908359b`), bo to jedyna operacja, która z definicji dotyczy całej
  sceny; `create_play_document` materializuje to, co znane.
- Domknięcie z round 2 Kroku 3: `cell_assets` sprzątane dla komórek, które
  nie są rezydentne, w toku ani chciane (sweep na końcu `request_cells`).

**D9. Przepustowość ładowania komórek (właściciel: "ładowanie komórek jest za
wolne", pomiar mapy 16 × 16 z `d6308c0f1c`: otwarcie 7 ms, pierwsza komórka
po ~10 s, 16 komórek po ~19 s; worker 2.6 s na komórkę 10 k = 0.26 ms na
rekord w `VariantParser`; 4 zadania w toku; pierwsza tura 4 zadań wyrzucona
po brakujących assetach i parsowana od nowa; commit 80–130 ms na komórkę
10 k).**
- Brak ponownego parsowania: zadanie trzyma sparsowane rekordy; po
  brakujących assetach wątek właściciela ładuje je raz (jak dziś), a
  zadanie jest **wznawiane od dekodowania** (worker lub owner, bez odczytu
  i parse). Assety `global/` i komórki najbliższej kamerze ładowane
  bezpośrednio po otwarciu (`load_global` zna referencje), więc pierwsza
  tura nie chybia.
- Zadania w toku: `MAX_CELL_JOBS` = liczba wątków `WorkerThreadPool`
  (`get_thread_count()`), nie stała 4; kolejność zleceń nadal po
  odległości.
- **Szybki parser rekordów**: dedykowany parser tekstu dla ograniczonej
  gramatyki plików encji i klastrów (słownik z kluczami String, liczby,
  bool, String, `Basis(...)`, `Vector2/3(...)`, `Color(...)`, `AABB(...)`,
  `Rect2(...)`, tablice, zagnieżdżone słowniki, `uid://` jako String),
  produkujący te same `Dictionary`/`Variant`, z fallbackiem do
  `VariantParser` na pierwszym nieznanym tokenie (bez zmiany formatu, bez
  drugiego enkodera; `VariantWriter` pozostaje jedynym pisarzem). Cel:
  ≥ 5× szybciej niż `VariantParser` na rekordzie encji; parity sprawdzana
  w Kroku 5 przez porównanie `Variant` z obu parserów na wszystkich
  rekordach `streaming_test` i jednej komórce mapy.
- Commit: rozbicie licznika `commit` na katalog/sekcje/flecs/
  `_component_changed`/`_assign_cell`; usunięcie alokacji per encja, które
  da się usunąć (rezerwacje map, przenoszenie `PreparedEntity` zamiast
  kopii, klucze sekcji współdzielone); cel ≤ 40 ms na komórkę 10 k. Komórka
  pozostaje atomowa.
- Dok encji (edytor): pomiar `d6308c0f1c` na mapie: `refresh_catalog`
  190–200 ms przy 200 k wpisów na każdą zmianę rezydencji, czyli większy
  hitch niż commit. Dok nie przebudowuje całej listy przy zmianie
  rezydencji: lista jest stronicowana z katalogu (widoczny zakres), a zmiana
  rezydencji odświeża tylko licznik i bieżącą stronę; cel ≤ 5 ms na
  zmianę rezydencji niezależnie od liczby encji.
- Miary sukcesu na mapie 16 × 16 (runtime i edytor, kamera w środku):
  pierwsza komórka ≤ 2 s po otwarciu (bez zimnej kompilacji shaderów),
  16 komórek ≤ 6 s, brak ticku wątku właściciela > 50 ms poza jednorazowym
  ładowaniem `.mgdata`.

## Trzy kąty dowodowe

- **API/kontrakty**: codegen i `EntityComponentTraits<T>::fields()` (referencja),
  `EntityFieldSchema::key`, `EntityComponentSchema::key`, `EntityScene`
  (`request_cells` nieblokujące, nowe `commit_ready`, `flush_streaming`,
  `PreparedEntity`/`PreparedCell` prywatne), `EntitySchemaRegistry`
  (deskryptory statyczne + `runtime_id` per świat), brak zmian bindingów,
  XML, ProjectSettings; format plików bez zmian.
- **Konsumenci**: `_install`/`_prepare`/`_commit`, `create_play_document`
  (ta sama reguła walidacji, `flush_streaming`), `EntitySceneCommands`
  (`_prepare` na ścieżce komend, `Section::components` kanoniczne dla
  `entity_scene_commands.cpp:257`), `EntitySceneIO`
  (klucze, `save` → `flush_streaming`), `Node3DEditor` i `EntitySceneRuntime`
  (bez zmian API), `entity_scene_editor.cpp` (nazwy z sekcji jak dziś),
  konwerter (pełny `load_subset` synchroniczny zostaje).
- **Własność/wątki**: wątek roboczy dotyka snapshotu zlecenia, plików,
  parsera, statycznych deskryptorów (read-only), `ResourceLoader`, własnych
  buforów; wątek właściciela dotyka `EntityWorld`, katalogu, sekcji, map
  komórek, `last_error`; bufory konstruowane/niszczone przez deskryptor
  (`construct`/`destruct`), `Ref<Resource>` w buforach zwalniane przy
  odrzuceniu wyniku lub skopiowane do ECS przez `copy_to`; każde zadanie
  zebrane przed zniszczeniem dokumentu.

## Sekwencja kroków

**Krok 1 → Usunięcie redundantnej pracy** `[independent]`
- what: `misc/scripts/entity_component_codegen.py` (static const `fields()`,
  klucz per pole/komponent), `scene/entity/entity_component_schema.h/.cpp`
  (konsumenci, `EntityFieldSchema::key`, `EntityComponentSchema::key`),
  `scene/resources/entity_scene.cpp` (`_install` bez `_describe`, buduje
  `Section::components` kanonicznie i `Section::name`; `_validate_fields`
  bez wymogu obecności pola, z gotowymi kluczami; `create_play_document` na
  tej samej regule), `scene/entity/entity_world.h/.cpp` (rejestracja
  `runtime_id` z deskryptorów statycznych; tylko nowe funkcje, bez dotykania
  hunków właściciela),
  `scene/entity/entity_scene_io.cpp` i `entity_scene_commands.cpp` (klucze).
- why now: zdejmuje koszt warstwy schematu przed jakąkolwiek zmianą struktury.
- how: D1, D2; zachowanie błędów: nieznany komponent/klucz/typ dalej błąd ze
  ścieżką pliku (granularność `typ`); brak pola = wartość domyślna (D2) na
  każdej ścieżce (ładowanie, play document, zapis). Liczniki `describe`/`world`
  zostają do porównania.
- dispatch: `[independent]`.

**Krok 2 → Optymalizacja jednowątkowa** `[independent]`, po Kroku 1
- what: `scene/resources/entity_scene.h/.cpp` (`PreparedEntity` z
  `construct`/`destruct`, `_prepare` dla `load_subset` do buforów, `_commit`
  na abstrakcji zbioru przygotowanego z `copy_to`, usunięcie scratch world
  jeśli bez użycia), `_read_stored` (D4, płytka kopia najwyższego poziomu,
  weryfikacja grepem braku mutacji zagnieżdżonych słowników).
- why now: bufor przygotowania jest formatem wyniku, który etap 3 produkuje
  na wątku roboczym.
- how: D3, D4; pomiar `read_record`/`install`/`commit` na komórce 2250 przed
  i po; `entity_world.cpp` ma niezacommitowane zmiany właściciela — edycja
  tylko wąskich, nowych funkcji, bez dotykania istniejących hunków.
- dispatch: `[independent]`.

**Krok 3 → Asynchroniczne, nieblokujące ładowanie komórek** `[independent]`, po Kroku 2
- what: `scene/resources/entity_scene.h/.cpp` (`CellJob` ze snapshotem,
  `PreparedCell`, `request_cells` nieblokujące, `commit_ready` z rewalidacją,
  `flush_streaming` we wszystkich punktach domknięcia, znaczniki błędów),
  `scene/entity/entity_component_schema.cpp` (atomowe liczniki assetów),
  `scene/entity/entity_scene_streaming.cpp` (kolejność: commit ukończonych →
  zlecenie nowych → release), liczniki, `scene/entity/entity_scene_io.cpp`
  (`save` → `flush_streaming`), `editor/scene/3d/node_3d_editor_plugin.cpp`
  (`set_scene_document` → `flush_streaming` starego dokumentu) i
  `scene/entity/entity_scene_runtime.cpp` tylko jeśli zmieni się wywołanie.
- why now: ostatni etap, bo commit korzysta z D3, a wątek roboczy z D1/D4.
- how: D5, D6; snapshot zlecenia, rewalidacja przy commicie, komórka
  atomowa, anulowanie, domknięcie zadań, błędy bez ponawiania co klatkę,
  brak dostępu do `EntityScene`/`EntityWorld` z wątku.
- dispatch: `[independent]`.

**Krok 3b → Odkrywanie komórek po koordynatach** `[independent]`, po Kroku 3
- what: `scene/entity/entity_scene_io.cpp` (`load` bez `cells/`, zapis po
  korzeniu, Save-As kopiuje nieznane komórki), `scene/resources/entity_scene.h/.cpp`
  (katalog tylko znanych encji, zadanie listujące katalog komórki, `release_cells`
  zapomina, `cell_assets` sweep, cache "brak katalogu", usunięcie `get_cells`),
  `scene/entity/entity_scene_streaming.cpp` (iteracja indeksów w zasięgu),
  `editor/scene/entity/entity_scene_editor.cpp` (liczba komórek rezydentnych,
  bez nazw nierezydentnych), `scene/entity/entity_scene_runtime.cpp` tylko
  jeśli zmieni się wywołanie.
- why now: bez tego otwarcie mapy jest liniowe w liczbie plików; cała
  reszta etapów 1–3 traci sens na dużej mapie.
- how: D8; format plików bez zmian; sceny demo bez regeneracji (układ
  katalogów jest już zgodny).
- dispatch: `[independent]`.

**Krok 3c → Przepustowość ładowania komórek** `[independent]`, po Kroku 3b
- what: `scene/resources/entity_scene.h/.cpp` (wznowienie zadania od
  dekodowania, preload assetów po otwarciu, `MAX_CELL_JOBS` z puli,
  rozbicie licznika commitu i optymalizacje commitu), nowy
  `scene/entity/entity_record_parser.h/.cpp` (szybki parser + fallback;
  `scene/entity/SCsub`), `scene/entity/entity_scene_io.cpp`
  (`read_variant_file` używa nowego parsera dla rekordów i klastrów),
  `scene/entity/entity_scene_streaming.cpp` (liczniki),
  `editor/scene/entity/entity_scene_editor.h/.cpp` (odświeżanie doku bez
  pełnej przebudowy).
- why now: bez tego ładowanie komórek jest ograniczone parserem i sztywnym
  limitem zadań, a pierwsza tura jest marnowana.
- how: D9; format i pisarz bez zmian; parity parserów mierzona.
- dispatch: `[independent]`.

**Krok 4 → Generator mapy stress 16 × 16** `[independent]`, po Kroku 1 (D2), równolegle z Krokiem 2/3 (rozłączne pliki)
- what: `editor/entity_scene_energy_converter.h/.cpp` (nowa funkcja
  `generate_streaming_stress_entity_scene()`), `main/main.cpp` (flaga
  `--generate-streaming-stress` w obu miejscach parsowania), `scene/entity/entity_scene_io.h/.cpp`
  (wejście zapisu klastra z listy rekordów: `write_cluster(path, records)`
  albo równoważne, używane też przez `save`, żeby nie było drugiego kodera).
- why now: dostarcza dane do walidacji Kroku 3 w skali, której nie ma żadna
  istniejąca scena.
- how: D7; deterministyczne id (seed) i rozstaw; `git`: katalog sceny jest
  nieśledzony jak pozostałe sceny demo (0.8 GB), generowany lokalnie.
- dispatch: `[independent]`.

**Krok 5 → Walidacja** (main agent)
- Build double editor i template. Stress scena w oknie: `--benchmark`,
  profil komórki 2250 po każdym kroku (cel: Krok 1 `install` 250 → ~100 ms;
  Krok 2 `install` → ~40 ms, `read_record` 645 → ~450 ms; Krok 3 czas wątku
  właściciela na tick = commit całych komórek: pomiar commitu komórki 2250
  i 10 k (mapa 16 × 16); jeśli commit 10 k przekracza ~5 ms, wynik idzie do
  właściciela jako pytanie o częściową rezydencję). Narzędzie ruchu
  kamery na `streaming_test` (sekwencje z sesji world storage) i na stress
  scenie: p95/p99 czasu klatki podczas dostreamowywania przed i po Kroku 3
  (cel: brak hitchy powyżej 2× mediany). Zero ERROR, czyste zamknięcie,
  `git status` scen czysty, zapis po edycji (właściciel w UI).
- Mapa stress 16 × 16 (Krok 4): czas otwarcia (plik główny + `global/`,
  oczekiwanie: poniżej sekundy) i RAM po otwarciu; czas do rezydencji
  komórek w zasięgu (koszt klastra vs pojedynczych plików osobno w
  licznikach workera); liczba rezydentnych komórek/encji przy
  kamerze w środku (oczekiwanie ~25 komórek, ~250 k encji) i czas do pełnej
  rezydencji zasięgu; przejazd kamery 1 km po osi X narzędziem ruchu
  (p95/p99 klatki, liczniki zleconych/zcommitowanych/zwolnionych komórek,
  brak hitchy z commitu powyżej budżetu); FPS przy ~250 k instancji dwóch
  meshy mikrogeometrii; VRAM/RAM; zero ERROR; czyste zamknięcie. Wynik
  jest miarą całego łańcucha (etapy 1–3), a limity, które wyjdą (parse
  klastra, RAM katalogu, koszt renderera przy 250 k instancji), trafiają
  do statusu jako wejście do batch reads i paczkowania. Status do
  `docs/research/`, wpis w `project-state.md`.

## Zamknięcia obowiązkowe

- Bindingi/XML bez zmian; brak ProjectSettings; brak nowego pliku źródłowego
  poza ewentualnym rozbiciem `entity_scene.cpp` (wtedy `SCsub`).
- Wątki: `_owner()`/`_is_owner()` na wszystkich mutacjach; `WorkerThreadPool`
  zadania z nazwą; brak `ResourceLoader` z wątku właściciela w ścieżce
  asynchronicznej poza `load_global`/`pin`.
- Zmiany właściciela w `entity_world.cpp`: bez dotykania istniejących hunków.
- Format i istniejące sceny demo bez zmian (brak regeneracji); nowa scena
  `streaming_stress` jest generowana lokalnie i nieśledzona.

## Ryzyka i niepewności (nieblokujące)

- Cel `read_record` zależy od kosztu `VariantParser`; jeśli parser dominuje,
  etap 2 zatrzyma się na ~450 ms, a odciążenie da dopiero etap 3.
- `ResourceLoader::load` assetów z wątku roboczego tworzy zasoby renderera
  z wątku (kolejka poleceń RS) i zleca zagnieżdżone zadanie puli, na które
  czeka (`resource_loader.cpp:730-748`): przy wielu komórkach w toku pula
  może się nasycić; precedens edytora (threaded load) istnieje, ale
  mikrogeometria (`.mgdata`) była dotąd ładowana z wątku właściciela.
- Czy flecs nadaje ten sam id komponentu w różnych światach jednego procesu,
  jest nieistotne po D1/D3 (worker nie używa id flecs).
- `flush_streaming` przed zapisem może zablokować zapis na czas jednego
  zadania (rząd setek ms przy komórce 2250).
- Mapa 16 × 16: 0.8 GB tekstu, 1.28 M plików i 2.56 M rekordów w katalogu (~500 MB RAM
  przy 200 B na wpis) mogą przekroczyć rozsądny czas otwarcia edytora
  (parse ~10–20 s); to zamierzony test limitu, a nie regresja; 250 k
  instancji mikrogeometrii to także test renderera poza zakresem tego planu.
