# World storage: tekstowe sceny katalogowe, siatka 3D i streaming po komórkach

Data: 2026-09-12 UTC (rewizja 3: nazwane gridy, zero sierot). Rewizja badań: `73d96f2fc7`. Autorytety: `AGENTS.md`,
`.agents/shared/planning-dispatch.md`, `docs/reference/plan-writing-workflow.md`,
`.agents/shared/godot-rules.md`, `docs/plans/2026-09-10-1023-flecs-native-scene-plan.md`
("Zapis, prefaby, edycja").

## Kontekst i kształt zamówienia (decyzje właściciela, 2026-09-12)

1. `.escn` jest **tekstowy i tylko dla edytora**; runtime dostanie osobny
   zoptymalizowany format — **paczkowanie to osobny, późniejszy task**; do
   tego czasu runtime czyta te same pliki tekstowe.
2. Główny plik sceny jest **mały i nie zawiera referencji do encji**; scena
   odkrywa z konwencji katalogowej, co na niej jest.
3. **Jedna encja na plik**, nazwa pliku = EntityId, friendly name w pliku.
4. **Płaska lista** (bez hierarchii katalogów; `parent` pozostaje polem).
5. **Klastry**: wiele encji w jednym pliku dla treści proceduralnej zapisanej.
6. **Podział przestrzenny od razu**: siatka **3D**, **nazwane gridy** o
   konfigurowalnym rozmiarze komórki i opcjonalnym własnym zasięgu
   streamingu, konfigurowane per scena, niehierarchiczne; encja jest
   przypisana do grida po nazwie, brak nazwy = grid domyślny; brak zasięgu =
   zasięg domyślny; **streaming po komórkach od razu** w edytorze i runtime.
7. **Zero śmieci na dysku**: żadnych osieroconych wpisów ani plików encji;
   edytor może być wolniejszy (pełny odczyt plików przy otwarciu, później
   batch reads), ale nie produkuje sierot.

Wykluczenia: bez readera starego binarnego `.escn` (sceny regeneruje
konwerter); bez ProjectSettings; bez testów; bez zmiany modelu prefabów poza
przeniesieniem instancji do plików; eksport gry i paczki runtime poza planem
(eksport `.escn` jest dziś i tak niedziałający: `editor_export_platform.cpp:1364`
filtruje po `PackedScene`).

## Stan faktyczny (anchory z badań)

- Warstwa zależna od jednego pliku binarnego jest mała: `EntityScene::Section{offset,length,bytes}`
  (`scene/resources/entity_scene.h:16-22`), `source/storage_path` (:27-28),
  `_read_bytes` (`entity_scene.cpp:71-89`), `EntitySceneIO::{read_manifest,load,save}`
  (`scene/entity/entity_scene_io.cpp:63-350`) i cztery override'y loadera/savera
  (:376,:385,:418,:441). Wszystko wyżej (`_read_record`, `_describe`, `_install`,
  `_prepare`/`_commit`, `EntitySceneCommands`, undo/redo, prefaby, dok, runtime)
  operuje na słownikach Variant. `create_play_document` (:615-645) dowodzi, że
  dokument działa bez offsetów.
- Rekord = `{components: {hex_type: {hex_field: Variant}}, parent, deleted, order}`
  (`entity_scene.cpp:114,161-163`); friendly name to komponent `EntityName`
  `0x2000000000000001` pole `1` (`entity_components.h:43-48`); assety jako
  stringi `uid://…[::sub]`; `_validate_fields` (`:167-244`) buduje zależności
  `{uid,path,type,entity,field}`; manifest `types` waliduje schemat.
- Nagrobki `deleted=true` są potrzebne prefabom (`entity_scene_commands.cpp:654,736`),
  undo (`:8-12`) i `_collect_required` (`entity_scene.cpp:311-316`).
- Prefaby: `prefab_instances` (`entity_scene.h:32`) z kluczami
  `uid,path,document,revision,mapping,overrides,conflicts` (`entity_scene_commands.cpp:641`);
  `_reconcile_prefab_catalog` (:634-764) mintuje lokalne id przy otwarciu i
  wstawia sekcje bez bajtów (:684-691); `_prefab_record` (:570-632) skanuje
  liniowo wszystkie instancje per nierezydentny rekord i ładuje cały prefab.
- Undo/redo to czyste słowniki (`Transaction`, `entity_scene_commands.h:31-37`).
- Rezydencja: `load_subset` (`entity_scene.cpp:473-540`) all-or-nothing,
  przodkowie przed potomkami, `_prepare` (:326-376) tworzy **nowy `EntityWorld`
  z pełną rejestracją ~40 schematów na każde wywołanie**, kopiuje komponenty
  dwa razy, `_validate_fields` dwa razy; `unload_subset` (:542-592) zostawia
  zakodowane bajty w RAM na zawsze, odmawia przy pinach (skan liniowy) i
  rezydentnych dzieciach; `pin/unpin` (:594-613) to istniejący refcount.
  `_owner()` (:32-38) przypina wątek: brak ścieżki wątkowej.
- Brak danych przestrzennych w dokumencie; granice per komponent:
  `EntityMesh` → `Mesh::get_aabb()` (cache, `mesh.cpp:2046`), `EntityGeometry::custom_aabb/procedural_aabb/extra_cull_margin`,
  `EntityLight::range`, `EntityDecal::size`, `EntityFogVolume::size`,
  `EntityReflectionProbe::size`, `EntityParticles::visibility_aabb`,
  `EntityMultiMesh`; pozy świata w `EntityTransform::current` (double
  translation, `entity_components.h:27-59`), precedens rzutowania AABB przez
  transformację w `entity_scene_editor.cpp:554-575`.
- Tick: edytor `Node3DEditor::_notification NOTIFICATION_INTERNAL_PROCESS`
  (`node_3d_editor_plugin.cpp:2211-2220`), kamera `get_editor_viewport(0)->camera`
  (precedens histerezy `update_grid` :2004-2011); runtime `EntitySceneRuntime::process`
  (`entity_scene_runtime.cpp:205-213`), kamera z `query<EntityCamera,EntityTransform>`
  (`node_3d_editor_plugin.cpp:2863-2874`). `set_scene_document` liczy sun/environment
  raz (:2837-2842). Dok odświeża się po `revision`, którego `load_subset` nie
  podbija (`entity_scene.cpp:511-513`, `entity_scene_editor.cpp:497-499`).
  `EntityWorld::drain_changed()` (`entity_world.cpp:348-357`) istnieje bez wywołań.
- Renderer: jeden pakiet na klatkę (`renderer_scene_cull.cpp:978-982`), unload
  encji retiruje instancję przez `_mark_changed(ALL)` (`entity_world.cpp:190`),
  `Ref<Mesh>` zwalniane po zakończeniu submisji GPU (:713-740); `.mgdata`
  podąża za rezydencją komórek; mesh na granicy komórek może thrashować.
- System plików edytora: skan kosztuje 2–3 operacje na plik i ~1 KB RAM na
  plik (`editor_file_system.cpp:1229-1412`); `ACTION_FILE_ADD` wywołuje
  `set_uid` = pełny load+save (:921-935); `.gdignore` wyklucza katalog ze
  skanu, doku, dialogów i eksportu (:3502-3505, `editor_file_dialog.cpp:78-84`,
  `editor_export_platform.cpp:639-713`). Precedens UID w pierwszej linii:
  `resource_format_text.cpp:1563-1578,2161-2233`. Dok nie paruje pliku z
  katalogiem (`filesystem_dock.cpp:1535-1605`, `editor_file_system.cpp:3127`,
  `dependency_editor.cpp:735-830`, `_check_existing` :2025); zakładka `.escn`
  nie jest przepinana po rename (:1581-1592). Sesja/recent trzymają ścieżkę
  głównego pliku (`editor_node.cpp:6406-6466,5418-5448`); editstate `.cfg`
  kluczowany md5 ścieżki (:2132,:4772). Dialogi Open/Save nie znają `.escn`
  (:3434,:3572,:3968).
- Serializacja: `VariantWriter`/`VariantParser` (`variant_parser.cpp:1986-2575`)
  jest bezstratna (grisu2, int64, Basis, StringName, klucze słownika sortowane,
  jeden klucz na linię, `-0` → `0`); `ConfigFile` używa jej (kolejność
  wstawiania); JSON stratny (float 14 cyfr, brak typów). Zapis atomowy
  pojedynczego pliku: `entity_scene_io.cpp:212-350` (temp + `MoveFileExW`),
  editor backup-save podwaja koszt zapisu per plik (`file_access_windows.cpp:199-286`).
  Brak transakcji wieloplikowej w drzewie.

## Mapa nakładania się

- **Binarny `.escn` v1** (`entity_scene_io.cpp`): zastępowany w całości, bez
  readera; sceny odtwarza konwerter (`editor/entity_scene_energy_converter.cpp`).
- **`load_subset`/`unload_subset`/`pin`**: zostają jako prymityw rezydencji;
  streaming buduje na nich API po komórkach, nie drugi mechanizm.
- **`EditorFileSystem`**: nie indeksuje katalogu sceny (`.gdignore`); scena
  sama jest swoim katalogiem.
- **`drain_changed`**: zostaje sygnałem treści; rezydencję zgłasza osobny
  licznik `residency_serial` (D5a), żeby nie mieszać dwóch znaczeń w jednym zbiorze.
- **`.tres`/`ConfigFile`**: dostarczają enkoder; nie zastępują loadera `.escn`.

## Decyzje projektowe

**D1. Układ katalogu** (`world/` to przykład; scena może leżeć gdziekolwiek):

```
<scene>.escn                      mały plik: uid (1. linia), document id, revision,
                                  [grids] nazwa -> {size, range?}, default_grid, default_range
<scene>/.gdignore
<scene>/global/<id>.escn          encje nieprzestrzenne, zawsze rezydentne, przypięte
<scene>/cells/<grid>/<x>_<y>_<z>/ komórka grida <grid> (nazwa z konfiguracji sceny)
    <id>.escn                     jedna encja
    <name>.cluster.escn           klaster: słownik id -> rekord
<scene>/prefabs/<instance id>.escn  słownik instancji prefabu (klucze jak dziś)
```

Główny plik nie wymienia encji ani komórek; scena odkrywa `global/`,
`cells/*/*/` i `prefabs/` przez listowanie katalogów. Environment, kamera
edytora, słońce to encje `global/` (są komponentami już dziś); główny plik nie
duplikuje ich.

**D2. Format pliku encji**: jeden słownik `VariantWriter::write_to_string`
(parser `VariantParser::parse`): `{ "parent": "<id>", "order": N, "components": {...} }`,
klucze sortowane (własność `VariantWriter`), nazwa w komponencie `EntityName`
(bez drugiego pola `name`, żeby nie było dwóch autorytetów). Klaster:
słownik `id -> rekord`. Nagrobek: plik `{ "deleted": true, "parent": ..., "order": ... }`
tylko dla id, które występują w `mapping` jakiejś instancji prefabu (potrzeba
`DELETED` vs `MISSING`, `entity_scene_commands.cpp:654`); pozostałe usunięcia
= usunięcie pliku, `_collect_required` traktuje brak pliku jak `MISSING`.
`types` z manifestu przenosi się do głównego pliku (jedna kopia). Reguły z
`EntitySceneIO::encode` (:14-40: zakaz OBJECT/RID/CALLABLE/SIGNAL, klucze
String) zostają na ścieżce zapisu. Wartości double: akceptujemy skrót float w
`rtos_fix` (`variant_parser.cpp:1996`), bo round-trip jest dokładny.

**D3. Przypisanie do komórek (przy zapisie, w edytorze)**: nowy komponent
`EntityStreaming { String grid; }` (serializowany, opcjonalny); brak
komponentu lub pusta nazwa = grid domyślny (`default_grid` w głównym pliku).
Grid ma `size` (rozmiar komórki w metrach, jednakowy na trzech osiach) i
opcjonalny `range` (zasięg ładowania w metrach od kamery); brak `range` =
`default_range`. Komórka jest chciana, gdy jej AABB przecina kulę o
promieniu `range` wokół kamery; histereza unloadu = `range + size`.
Komórka = `floor(origin / size)` na osiach XYZ, gdzie origin to translacja
pozy świata liczona z serializowanego `local` po łańcuchu rodziców
(`EntityTransformSystem::compose`, `entity_transform_system.cpp:14-23`),
z ECS dla rekordów rezydentnych, z plików dla nierezydentnych. Bez
rozpiętości: rozmiar obiektu nie wpływa na przypisanie, o gridzie decyduje
autor. Encja bez transformacji, `EntityEnvironment`, `EntityCamera`, światło
kierunkowe → `global/`. Reguła przeniesienia: zmiana pozy encji przenosi
przy zapisie także potomków (rekordy potomków czytane z plików, komórki
liczone na nowo, pliki przenoszone przez rename). Nieznana nazwa grida w
pliku encji = błąd zapisu i ładowania ze ścieżką (bez cichego przeniesienia
do domyślnego).

**D4. Odkrywanie bez indeksu**: otwarcie sceny listuje `global/`,
`cells/<grid>/<x>_<y>_<z>/` i `prefabs/` i **czyta każdy plik encji i klastra**
(nagłówek rekordu: `parent`, `order`, `EntityName`, komponenty), budując
katalog, mapę id→plik/komórka i nazwy dla doku. Plik jest jedynym źródłem
prawdy; nie ma danych pochodnych na dysku, więc nie ma sierot ani dryfu.
Koszt: 10 004 pliki ≈ 1–2 s na NTFS, 100 k ≈ 10–20 s; optymalizacja przez
batch reads to osobny późniejszy task. Loader: zduplikowane id w dwóch
plikach = błąd ze ścieżkami; plik o niepoprawnej nazwie w katalogu komórki =
błąd ze ścieżką (nie ignorowany po cichu). `get_dependencies` głównego pliku
= ten sam pełny odczyt (`EditorFileSystem` woła go tylko przy zmianie
głównego pliku, więc lista zależności może być nieświeża do następnego
zapisu; przenoszenie ścieżek assetów idzie po uid, a eksport jest poza
planem).

**D5a. Brudne encje, undo i zdarzenia rezydencji.** `EntityScene` prowadzi
jawny zbiór `dirty` (id zmienione od ostatniego zapisu), wypełniany przez
`EntitySceneCommands::execute/undo/redo` i czyszczony przez zapis; to jest
zakres zapisu w D6. Encja brudna jest przypięta i nie podlega unloadowi
komórki (komórka z brudnymi encjami zwalnia tylko czyste); dlatego cele
undo/redo są zawsze rezydentne, a `Transaction` działa jak dziś. Dla
encji czystych `unload_subset` nie trzyma `Section::bytes` (plik jest
autorytetem). Zdarzenia rezydencji nie idą przez `EntityWorld::changed`
(to sygnał treści dla renderera i dziś jest czyszczony przez `drain_changed`,
`entity_world.cpp:348-357`), tylko przez nowy licznik
`EntityScene::residency_serial`, podbijany przy każdym `request_cells`/`release_cells`;
dok odświeża listę po `revision` **lub** `residency_serial`. `revision`
podbija się tylko wtedy, gdy zapis faktycznie zapisał zmieniony rekord lub
instancję prefabu, więc zapis bez zmian nie zmienia żadnego pliku, a
detekcja zmian prefabów (`entity_scene_commands.cpp:591,670`) pozostaje.

**D5. Rezydencja i streaming**: nowe API `EntityScene::request_cells(Vector<CellKey>)`
/ `release_cells(...)`, zbudowane na `load_subset`/`unload_subset`/`pin`:
komórka rezydentna = jej encje plus przodkowie, przodkowie liczeni
refcountem (dzisiejsze `pins`; skan liniowy w `unload_subset:548-554`
zastąpiony mapą id→licznik), `global/` przypięty na stałe wraz z encją
kamery. Zwolnienie komórki: najpierw odpięcie jej przodków, potem
`unload_subset` czystych encji komórki (dzieci przed rodzicami), pominięcie
brudnych i przypiętych. Encje zmintowane przez `_reconcile_prefab_catalog`
(bez pliku) należą do komórki korzenia swojej instancji (zapisanej w pliku
instancji) i są oznaczone jako wygenerowane do pierwszego zapisu.
Scheduler per klatka (edytor: początek `NOTIFICATION_INTERNAL_PROCESS`
przed `begin_tick`; runtime: `EntitySceneRuntime::process` przed
`interpolate`): pozycje kamer (edytor: suma zasięgów kamer **wszystkich
widocznych viewportów** `viewports[i]`, bo wszystkie renderują scenariusz
dokumentu, `node_3d_editor_plugin.cpp:2846-2848`; runtime
`query<EntityCamera,EntityTransform>` z `current`), zbiór chcianych
komórek per grid w jego zasięgu, histereza +1 komórka na unload, priorytet po
odległości, budżet N encji na klatkę w **jednym** `load_subset` (jeden pakiet
renderera na klatkę), unload komórek poza zasięgiem. Koszt stały `_prepare`
usunięty: jeden świat tymczasowy trzymany w `EntityScene` i czyszczony
zamiast tworzonego na każde wywołanie; jedno `entity_decode_asset` na
referencję (walidacja pola i zależności współdzielą wynik); `unload_subset`
przestaje trzymać `Section::bytes` dla encji mających plik (rekord czyta się
z dysku). Zmiany rezydencji zgłaszane dokowi przez `drain_changed()`;
`set_scene_document` ponownie liczy `native_directional_light`/`native_environment`
po zmianie rezydencji. Skan `_prefab_record` po instancjach zastąpiony mapą
`local id -> instancja` budowaną przy otwarciu.

**D6. Zapis**: zakres = zbiór `dirty` (D5a) plus przeniesienia komórek
potomków (D3). Atomowość per plik (temp + `MoveFileExW`, bez editor
backup-save dla plików dzieci). Kolejność: (1) zmienione rekordy zapisywane
w miejscu (atomowa podmiana pod dotychczasową ścieżką), (2) przeniesienia
między komórkami jako atomowy `rename` pliku (bez okna z duplikatem lub
sierotą), (3) główny plik tylko gdy zmieniony, (4) usunięcia plików encji
usuniętych (nagrobek tylko dla id z `mapping` prefabu, D2), puste katalogi
komórek usuwane. Każdy krok jest atomowy per plik, więc awaria zostawia
stan, w którym każdy plik jest kompletną encją, a żaden nie jest sierotą;
loader po awarii widzi co najwyżej niezapisane zmiany, nigdy śmieci. Zapis
dwukrotny bez zmian nie dotyka żadnego pliku. Uchwyty plików zwalniane
przed rename. Save-As materializuje wszystkie rekordy (czyta nierezydentne
z plików) do nowego drzewa; stare drzewo pozostaje nietknięte. `.gdignore`
pisze `save` przy tworzeniu katalogu.

**D7. Integracja z edytorem**: `has_custom_uid_support` zostaje, UID w
pierwszej linii głównego pliku, `get_resource_uid` czyta jedną linię,
`set_uid` przepisuje jedną linię (`resource_format_text.cpp:2161-2233`);
loader zwraca `INVALID_ID` dla ścieżek wewnątrz katalogu sceny bez otwierania.
Parowanie pliku z katalogiem jest fork-lokalne: pomocnik
`EntitySceneIO::companion_directory(path)` (typ `EntityScene` po
rozszerzeniu i typie zasobu) wołany z `FileSystemDock::_try_move_item`,
`_try_duplicate_item`/`EditorFileSystem::_copy_file`, `DependencyRemoveDialog::show`
i `_check_existing`; bez nowej wirtualnej w upstreamowym
`ResourceFormatLoader` (unikamy GDVIRTUAL i XML dla API, którego nie
potrzebuje nikt poza sceną natywną); przepięcie zakładki `.escn` po rename
(`filesystem_dock.cpp:1581`);
sesja i recent sprawdzają istnienie katalogu; Save-As/Open dialogi rozpoznają
`EntityScene`. Loader: `<scene>.escn` bez katalogu → twardy błąd z ścieżką.

**D8. Runtime**: ten sam loader i scheduler; `EntitySceneRuntime::setup`
przestaje ładować wszystko (:31-41) i przechodzi na komórki. Paczkowanie i
eksport: osobny task.

## Trzy kąty dowodowe

- **API/kontrakty**: format katalogu i plików (D1–D4), `EntityScene` (nowe
  `request_cells`/`release_cells`, `CellKey`, mapa id→komórka, scratch world),
  `EntitySceneIO` (tekst), `ResourceFormatLoaderEntityScene` (uid, deps,
  companion paths), `ResourceFormatLoader::get_companion_paths` (nowa
  wirtualna, upstream-tracked; domyślnie puste), `EntitySceneRuntime::setup`,
  `Node3DEditor` scheduler, `EntitySceneEditor` odświeżanie, główny plik
  `ConfigFile`. Bindingi skryptowe bez zmian (`EntityScene::_bind_methods`
  `entity_scene.cpp:23-30` sprawdzić, czy eksponuje coś z sekcji).
- **Konsumenci**: `EditorNode::load_scene/_save_native_scene/_load_open_scenes_from_config`,
  `EntitySceneRuntime`, konwerter (trzy sceny), `EntitySceneCommands`
  (`_reconcile_prefab_catalog`, `_prefab_record`, encode do `Section::bytes`
  :906,:1015,:1163,:1226), dok, `Node3DEditor::set_scene_document`, dok
  systemu plików, `EditorFileSystem`, `main.cpp:4172`.
- **Własność/wątki**: wszystko na wątku właściciela (`_owner()`); brak
  wątku streamingu w tym planie (budżet per klatka); zasoby renderera
  zwalniane przez istniejące retire; `Ref<Mesh>` podąża za rezydencją.

## Sekwencja kroków

**Krok 1 → Format tekstowy i IO katalogowe** `[independent]`
- what: `scene/entity/entity_scene_io.h/.cpp` przepisane: `save` pisze
  katalog wg D1/D2/D6, `load` czyta główny plik + listuje `global/`, `cells/`,
  `prefabs/`, czyta każdy plik (D4), buduje katalog, mapę id→plik/komórka, nazwy i
  `prefab_instances`; `EntityScene::Section` traci `offset/length`, zyskuje
  ścieżkę; `_read_bytes` → odczyt i parse pliku (klaster: cache słownika
  klastra); `EntitySceneIO::encode/decode` → `VariantWriter`/`VariantParser`
  z dotychczasową walidacją typów; `get_resource_uid`/`set_uid` na pierwszej
  linii; `get_dependencies` z pełnego odczytu plików encji + prefabów; `rename_dependencies`
  przepisuje tylko pliki instancji prefabów; nagrobki wg D2; usunięcie
  formatu binarnego. `EntitySceneCommands` miejsca kodujące do
  `Section::bytes` używają słowników (bez bajtów).
- why now: kontrakt danych dla wszystkiego poniżej.
- how: przypisanie do komórek przy zapisie wg D3 (grid z `EntityStreaming`,
  origin z pozy świata); nowy komponent `EntityStreaming` w
  `scene/entity/entity_components.h` (schemat generowany przez
  `misc/scripts/entity_component_codegen.py`); cały zapis deterministyczny
  (sort po id, po uid).
- dispatch: `[independent]`.

**Krok 2 → Rezydencja po komórkach i koszt ładowania** `[independent]`, po Kroku 1
- what: `scene/resources/entity_scene.h/.cpp`: `CellKey{grid,x,y,z}`,
  mapa komórka→ids, zbiór `dirty`, `residency_serial`, `request_cells`/`release_cells`
  na `load_subset`/`unload_subset` z refcountem przodków (mapa zamiast
  skanu `pins`), przypięcie brudnych; scratch `EntityWorld` reużywany w
  `_prepare`; pojedyncze `entity_decode_asset` na referencję
  (`_validate_fields` + `write_component`); `unload_subset` bez trzymania
  bajtów dla encji z plikiem; `_prefab_record` z mapą local id→instancja;
  `global/` przypięty; `drain_changed` jako źródło zdarzeń rezydencji.
- dispatch: `[independent]`.

**Krok 3 → Scheduler streamingu w edytorze i runtime** `[independent]`, po Kroku 2
- what: `editor/scene/3d/node_3d_editor_plugin.cpp` (tick przed
  `begin_tick`, kamera, budżet, histereza; re-ewaluacja słońca/environment;
  `set_scene_document` bez `load_subset` całości), `editor/editor_node.cpp`
  `load_scene` (:4922-4930: ładuje tylko `global/`), `editor/scene/entity/entity_scene_editor.cpp`
  (odświeżanie po rezydencji, nazwy z katalogu odczytanego przy otwarciu), `scene/entity/entity_scene_runtime.cpp`
  (`setup` i `process`). Liczniki: encje/komórki załadowane i zwolnione na
  klatkę, w istniejącym raporcie `--benchmark`.
- dispatch: `[independent]`.

**Krok 4 → Integracja z systemem plików edytora** `[independent]`, po Kroku 3 (wspólny `editor_node.cpp`)
- what: `scene/entity/entity_scene_io.h/.cpp` (`companion_directory`),
  `editor/docks/filesystem_dock.cpp` (`_try_move_item`, `_try_duplicate_item`,
  `_check_existing`, przepięcie zakładki), `editor/file_system/editor_file_system.cpp`
  (`_copy_file`; `ACTION_FILE_ADD` nie wywołuje `set_uid`, gdy uid jest już
  zapisany), `editor/file_system/dependency_editor.cpp` (usuwanie z
  katalogiem), `editor/editor_node.cpp` (sesja/recent: istnienie katalogu;
  dialogi Open/Save-As z `EntityScene`).
- dispatch: `[independent]`.

**Krok 5 → Konwerter i fixture'y** `[mechanical]`, po Kroku 1
- what: `editor/entity_scene_energy_converter.cpp`: trzy konwersje zapisują
  nowy układ (siatka domyślna); regeneracja `energy_directional`, `main`,
  `microgeometry_stress`; usunięcie starych plików `.escn` binarnych z
  `demos/rtxdi_manual`; `.gitattributes` bez zmian (tekst, LF).
- dispatch: `[mechanical]`.

**Krok 6 → Scena testowa streamingu, autorstwo tekstowe** (main agent)
- `demos/rtxdi_manual/streaming_test.escn` + katalog, napisane **ręcznie jako
  pliki tekstowe** przez agenta (bez konwertera): grid domyślny `size = 10`,
  `default_range = 5`; siatka 5×1×5 komórek (50 m × 50 m, komórki `-2..2`
  na X i Z, `0` na Y), w każdej komórce 3 encje z meshami z istniejących
  assetów natywnych (`native_assets/energy_directional/{cube,sphere,plane}.res`
  po uid), rozstawione wewnątrz komórki; `global/`: environment, słońce,
  kamera edytora w środku sceny (0, 1.5, 0). Oczekiwanie: przy kamerze w
  środku ładują się tylko komórki, których AABB przecina kulę 5 m, czyli 1–4
  komórki (zależnie od pozycji względem granic), reszta pozostaje na dysku;
  przesunięcie kamery o 10 m zmienia zbiór rezydentnych komórek o dokładnie
  ten sam rząd. To także test ergonomii formatu: agent tworzy i edytuje
  scenę bez edytora, a edytor po otwarciu ma pokazać ją poprawnie.
- Wynik zapisany w statusie: lista załadowanych komórek przy trzech pozycjach
  kamery, liczniki load/unload, czas otwarcia, brak sierot po zapisie z
  edytora (`git status` katalogu sceny po zapisie bez zmian = czysty).

**Krok 7 → Walidacja** (main agent)
- Build ordinary/double/template. Konwersja trzech scen; `git diff` po
  dwukrotnym zapisie tej samej sceny = pusty. Edytor double na stress:
  otwarcie (tylko `global/` + komórki w zasięgu), przebieg z narzędziem ruchu
  kamery (`misc/scripts/editor_camera_motion.py`, bez zmian focusu),
  liczniki załadowanych/zwolnionych komórek, zero ERROR, zrzut okna;
  edycja encji, odjazd kamery (komórka z brudną encją nie zwalnia jej),
  powrót, zapis i ponowne otwarcie = zmiana zachowana; undo po odjeździe;
  instancja prefabu obejmująca dwie komórki; rename/duplicate/delete sceny w
  doku; sesja po restarcie. Runtime: uruchomienie `scene.escn` natywnie z
  buildu edytora i template (`--path`, `--rendering-driver vulkan`), ruch
  kamery, brak błędów. Status do `docs/research/` i `project-state.md`.

## Zamknięcia obowiązkowe

- ClassDB/XML: `EntityScene` bindingi bez zmian (sprawdzić
  `entity_scene.cpp:23-30`); brak nowych wirtualnych w `ResourceFormatLoader`
  (parowanie fork-lokalne w `EntitySceneIO`).
- Brak ProjectSettings; parametry siatki w głównym pliku sceny.
- Wątki: wszystko na wątku właściciela; loader `ResourceLoader` może być
  wywołany z wątku ładowania — `EntitySceneIO::load` nie dotyka `_owner()`
  (jak dziś).
- Upstream: `filesystem_dock.cpp`, `editor_file_system.cpp`,
  `dependency_editor.cpp`, `resource_loader.*`, `editor_node.cpp` wąsko;
  reszta fork-lokalna.
- Kompatybilność: stary binarny `.escn` bez readera; regeneracja konwerterem.
- Eksport: katalog sceny jest `.gdignore`, więc do czasu tasku paczkowania
  sceny natywne działają z drzewa projektu (uruchomienie przez `--path`), nie
  z eksportu; to jawna konsekwencja wykluczenia, nie regresja (eksport
  `.escn` nie działa także dziś).
- Pamięć katalogu: rząd 200 B na encję w RAM (id, parent, order, nazwa,
  komórka), czyli ~20 MB przy 100 k encji; do zmierzenia w Kroku 6.
- Czas otwarcia sceny rośnie liniowo z liczbą plików (pełny odczyt przy
  otwarciu); batch reads to osobny task.

## Ryzyka i niepewności (nieblokujące)

- Obiekt większy niż komórka swojego grida jest ładowany po origin, więc
  autor musi przypisać duże obiekty do grida o większej komórce; brak
  HLOD/impostorów w tym planie.
- Mesh współdzielony przez sąsiednie komórki może być wielokrotnie
  ładowany/zwalniany na granicy zasięgu (histereza łagodzi, nie eliminuje).
- Prefaby obejmujące wiele komórek trzymają źródłowy prefab w RAM, dopóki
  którykolwiek członek jest rezydentny.
- Budżet per klatka jest liczbą encji; koszt per encja mierzy istniejący
  profiler `load_subset`.
- Klastry proceduralne: zapis całego pliku klastra przy zmianie jednej encji.
