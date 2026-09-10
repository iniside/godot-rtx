# Natywny model sceny 3D na Flecs

Status: zatwierdzony przez właściciela 2026-09-10; implementacja rozpoczęta.
Task-start commit: `3a6a695fc859ea8aa271adf8eaddb03349f304c5`.
Aktualizacja wejścia kroku1: `0af41a64e2` zapisał populację stress w scenie,
usuwając runtime generation. Zachować aktualne serialized instancing i jego
effective transforms; historyczna recipe poniżej opisuje wymagany wynik,
nie nakaz przywrócenia generowania runtime ani pominięcia nowych danych.
Review: runda1 wykryła konflikt starego importera `.escn`; poprawiono krok4.
Świeży reviewer rundy2 potwierdził PASS dla zamknięcia jedynego zgłoszonego błędu.
To przegląd planu, nie dowód kompilacji ani działania przyszłej implementacji.
Data: 2026-09-10 UTC. Baseline: `b33710da3d563f28ff6fc99ed1d90fa308b99e22`.
Podczas przygotowania planu osobna praca przesunęła HEAD do `f9329167c6`
(`193ce66ab8`: usunięcie fixed microgeometry metadata admission cap).
Nie jest to implementacja tego planu. Zachować tę zmianę; przed startem
odświeżyć kotwice względem aktualnego HEAD, bez przywracania dawnego limitu.
Repozytorium: `G:/Projects/godot-rtx`. Zatwierdzony plan znajduje się w `docs/plans/`.

## 1. Wynik i decyzje właściciela

Scena świata 3D w edytorze i grze zawiera wyłącznie encje i komponenty Flecs.
Nie ma Node/Object/Resource wrappera na encję lub komponent ani równoległego
świata SceneTree. Kryterium to prostota docelowej architektury: bezpośrednio
wymieniamy istniejącą odpowiedzialność i jej konsumentów, nawet gdy wymaga to
więcej zmian niż dodanie adaptera. Nie tworzymy ogólnego SceneAdapter,
RenderBridge, drugiego schedulera ani nowej warstwy dostępu do całego silnika.

Wiążące ustalenia rozmowy:

- Flecs przed world streamingiem. Fundament ma umożliwiać późniejsze gęste
  światy 100 km+ z dysku lub generatora, ale ten plan nie implementuje streamingu.
- Gęste populacje domyślnie płaskie; hierarchie wspierane tylko tam, gdzie są
  potrzebne. Grupowanie edytora i przyszłe komórki nie wymuszają parent transform.
- Skrypty i nowy model języka poza zakresem. Nie przenosimy attach-script,
  `_ready`, `_process`, sygnałów skryptowych ani Node RPC na encje.
- Stare sceny mogą zniknąć. Przenosimy wyłącznie wymienione niżej sceny używane
  do sprawdzania renderera. Jednorazowa prosta konwersja danych jest dozwolona;
  uniwersalna zgodność `.tscn` nie jest celem. Node plugins nie są wspierane.
- Sceny 2D nie są wspierane. UI gry będzie osobno w HTML/CSS; jego silnik i
  integracja nie wchodzą do tego planu. Wewnętrzne UI edytora może używać Controls.
- Shared Mesh/Material/Texture/Animation/Audio zasoby oraz usługi aplikacji mogą
  pozostać Object/RefCounted. Nie reprezentują pojedynczych encji świata.
- Multiview/XR/VR/split screen poza zakresem; jeden game viewport.
- Automatycznych testów nie piszemy ani nie uruchamiamy. Weryfikacja obejmuje
  kompilację, faktyczne czynności edytora, działającą grę i realny Vulkan.

Właściciel jawnie zrezygnował ze zgodności starej sceny i Node pluginów.
To ograniczony wyjątek od zachowania upstream API dla świata: usuwamy stare
world-facing kontrakty wraz z konsumentami. Zachowane API zasobów, UI aplikacji
i innych nieobjętych tą wymianą usług nadal podlega normalnym regułom zgodności.
Nie usuwamy całego Object/ClassDB z silnika.

Zatwierdzony zakres obejmuje dodatkowo brak zamiennika
SoftBody3D, Generic6DOFJoint3D oraz CSG/GridMap authoring. Pierwsze dwa wynikają
z granic wybranego Box3D; drugie dwa nie są potrzebne rendererowym scenom i
oznaczałyby osobny port narzędzi authoringu. Nie przedstawiam tych wyłączeń
jako wcześniejszej decyzji właściciela. Szczegóły w7b/7e.

## 2. Podstawa źródłowa i nakładanie się odpowiedzialności

Źródła dotychczasowej analizy:

- `docs/research/2026-09-09-2023-flecs-scene-model-replacement-research.md`:
  PackedScene, editor selection/Inspector/undo/import/export, runtime subsystemy.
- `docs/research/2026-09-09-2046-entity-renderer-boundary-research.md`:
  Cull Instance, Forward+ owner consumers, FrameContext, RID i lifetime.
- `docs/plans/2026-09-10-0807-gpu-microgeometry-cpu-removal-plan.md` oraz
  `docs/reference/project-state.md`: obecne persistent GPU records, dirty-only
  input publication i praca workerów. Wczorajszy opis pełnego per-frame gather
  microgeometry jest historyczny i nie jest podstawą do jego odtworzenia.

| Istniejący mechanizm | Decyzja i uzasadnienie |
| --- | --- |
| Flecs v4.1.6, commit fb55f3c25660425cfe1bc4cf5e6bff8b3f18a9b8 | Przypiąć tę wersję; ECS, queries, relacje, prefaby i Meta są podstawą, bez własnego odpowiednika ECS |
| SceneTree/Node3D/PackedScene | Zastąpić jako autorytet świata; zachować tylko niezbędną infrastrukturę aplikacji/UI, bez przyjmowania scen Node jako świata gry |
| RendererSceneCull i przygotowanie Forward+/RT | Zmienić bezpośrednio ingress, tożsamość i owner-dependent konsumentów; nie odtwarzać Instance przez adapter |
| Persistent microgeometry/RT records i shared cut/BLAS | Zachować i zasilać zmianami encji; nie wracać do skanowania i hashowania całej populacji co klatkę |
| RD, Vulkan, material/mesh storage, lighting/denoisers | Zachować odpowiedzialność GPU i assetów; zmiany tylko tam, gdzie wymaga ich rzeczywisty nowy kontrakt danych |
| Editor Controls i historia operacji | Zachować widgety; wymienić model selekcji i dostęp do pól. Jedna usługa dokumentu dla undo jest dopuszczalna, proxy encji nie |
| ResourceLoader/ResourceUID/PCK | Zachować pliki/assety i eksport; dodać natywny dokument encji, nie zasób na każdą encję |
| WorkerThreadPool i obecna granica renderowania klatki | Wykorzystać; brak nowego uniwersalnego grafu zadań. FrameContext zawiera metadane klatki, nie gotowy snapshot świata |

Podstawowe kotwice: `PackedScene::instantiate` w `scene/resources/packed_scene.cpp`,
`Main::start` w `main/main.cpp`, `EditorData::EditedScene`, `EditorInspector::edit`,
`EditorSelection::add_node`, `SceneTreeDock::_do_reparent`,
`RenderingServerDefault::_init/draw/_capture_frame` i `RendererSceneCull::Instance`.
Ich deklaracje/implementacje, XML i bindingi były sprawdzone przez clang-nav oraz
ukierunkowane odczyty. Konkretny delta rendererowy i lista scen są w sekcjach niżej.

## 3. Docelowe kontrakty

Nazwy nowych typów i ścieżek poniżej są propozycją planu, nie istniejącymi API.
Implementer może dopasować prywatne nazwy; nie może zmienić własności i zakresu.

### Świat, tożsamość i relacje

`EntityWorld` jest zwykłym C++ właścicielem jednego `flecs::world`, kolejki
transakcji i stanu podsystemów. Świat edycji i świat uruchomionej gry są
odrębnymi instancjami tego samego modelu; gra nie mutuje dokumentu edycji.
Usługa aplikacji/MainLoop może być Objectem, ale encje i komponenty nie są.

Trwałe `EntityId` jest 128-bitowym kluczem zapisanym w dokumencie. Lokalne
`ecs_entity_t`, renderer slot/generation i fizyczne uchwyty nie są zapisywane.
Tabela rozwiązywania obejmuje tylko rezydentne encje; dokument ma katalog
trwałych rekordów bez tworzenia encji-placeholder dla całego świata.
Referencja rozróżnia resident, unloaded, missing i deleted. Unload nie zmienia
trwałych relacji; delete zapisuje usunięcie. Recreate po undo zachowuje EntityId,
ale otrzymuje nowy uchwyt runtime. Duplikowanie nadaje nowe ID i przepisuje
wewnętrzne referencje kopii, zachowując referencje zewnętrzne.

Domyślny renderowalny obiekt nie ma rodzica. Dla faktycznych hierarchii używamy
jednego wybranego storage `flecs::Parent`; nie wdrażamy równoległego schedulera
Parent/ChildOf. Grupowanie dokumentu jest metadanymi, nie transform parenting.
Reparent odrzuca cykl i ma jawny tryb keep-world/keep-local; edytor domyślnie
zachowuje world transform. Bieżąca wersja utrzymuje potrzebnych przodków w pamięci
przez pin zależności. Unload podzbioru nie usuwa rezydentnego dziecka razem z
rodzicem; permanentne delete-hierarchy jest osobną transakcją.

### Komponenty, metadane i transformacje

Typy komponentów i pola mają stabilne ID schematu. Generator z deklaracji C++
emituje Flecs Meta i natywny opis pól: typ, defaults, serializacja, edycja,
referencje, zakres/jednostki. Jawne adnotacje służą semantyce i ID, nie drugiej
liście pól. Generator obejmuje oznaczone deklaracje w `scene/entity/`, nie
przetwarza całego kodu Godota jako nowego systemu refleksji.

Wybrana implementacja narzędzia: host Clang AST z prawdziwymi definicjami
SCons i generowanie kodu w Pythonie; dostępne lokalnie clang++/clang-cl.
Nie wymaga C++26 ani oddzielnego runtime refleksji. Typy Godota, kontenery,
asset refs i EntityRef dostają generowane/typowane codecs; nie zapisujemy raw
memory, pointerów lub zależnego od kolejności rejestracji ID Flecs. Brak wsparcia
typu jest jawnym błędem generowania, nie pominięciem pola.

SCons przygotowuje scanner TU i jego defines/include paths przed kompilacją
generated headers; generator nie wymaga istniejącego compiledb ani własnego
wygenerowanego outputu do pierwszego uruchomienia. Jawne ID pól pozostają przy
rename; build śledzi wejściowe nagłówki i wersję generatora.

Globalna translacja używa double niezależnie od `real_t`; rotacja/skala i lokalna
matematyka mogą być float. Hierarchię obliczamy w kolejności głębokości; zmiana
rodzica oznacza aktualizację zależnych dzieci, płaskie encje idą bez traversal.
Przechowujemy rozdzielone current/previous simulation poses i render pose.
Teleport resetuje interpolację oraz właściwą historię rendererową. Konwersja
do camera-relative float odbywa się raz na ustalonej granicy danych renderera,
z zachowaniem poprzedniego początku układu dla motion vectors.

### Zapis, prefaby, edycja

Nowy `EntityScene` jest jednym zasobem dokumentu/prefabu, nie magazynem Objectów.
Format `.escn` ma manifest typów, asset dependencies, katalog EntityId→sekcja
i adresowalne paczki rekordów. Kodowanie sekcji jest prywatnym szczegółem;
obowiązkowe są odczyt podzbioru, jawna wersja formatu i atomowy zapis dokumentu.
Nie wprowadzamy teraz spatial partition schedulera, automatycznego I/O ani
world generation. UI nie wymaga ręcznego przydzielania obiektów do paczek.
Loader nie materializuje wszystkich komponentów do pokazania katalogu świata.

Dla rezydentnego rekordu jedynym edytowalnym autorytetem jest ECS; dokument
serializuje jego stan przy zapisie. Dla unloaded rekordu autorytetem jest
zapisana sekcja. Katalog i prefab provenance nie tworzą drugiej mutable kopii
komponentów. Save obejmuje zmienione rekordy oraz zachowuje unloaded sekcje.

Referencje assetów używają ResourceUID i opcjonalnej ścieżki diagnostycznej.
Brakujące zasoby/pola/typy zgłaszają błąd z EntityId i polem; brakujące mesh
można oznaczyć w edytorze, ale save/export nie zgłasza pełnego sukcesu z cicho
pominiętymi encjami. Nie tworzymy warstwy migracji starych `.tscn`.

Prefab ma stabilne ID elementów źródłowych. Instancja przechowuje pochodzenie
i nadpisania: wartość pola, add/remove component, dodanie/usunięcie dziecka,
reparent i kolejność. Revert usuwa konkretne nadpisanie, apply zmienia prefab
i aktualizuje jego użytkowników; usunięty element źródła z lokalnymi zmianami
staje się jawnym konfliktem do rozstrzygnięcia, nie ginie bez informacji.
Flecs IsA obsługuje runtime inheritance; nie zastępuje tych operacji dokumentu.

Komenda edycyjna adresuje document ID/EntityId/component ID/field ID i wartości
before/after. Nie przechowuje długowiecznego pointera do komponentu. Transakcje
create/delete/duplicate/reparent/set/add/remove są atomowe z punktu widzenia UI
i undo. Aktywna edycja pinuje potrzebne rekordy; komenda na unloaded celu
wczytuje tylko wymagany podzbiór. Zaznaczenie pozostaje trwałym adresem.

### Wątki i publikacja renderowania

Fazy: input/native commands → safe structural merge → fixed simulation →
animacja/transformacje zależne → render extraction zmienionych danych →
publikacja przy obecnej granicy klatki. Flecs systems deklarują dostęp; jobs
korzystają z istniejącego WorkerThreadPool. Kolejność observerów nie jest
mechanizmem planowania lifecycle. Dirty oznaczają też zapisy systemów do pól,
nie tylko OnSet emitowany przez set().

Gra nie przekazuje render thread pointerów do przenoszalnych komponentów.
Publikowane paczki zmian mają własną pamięć i generacje celów. Renderer posiada
pochodne stabilne sloty/historię i zasoby GPU; aktualizuje je raz dla zmiany.
Nie powstaje dodatkowy pełny snapshot populacji co klatkę. Usuwanie jest
zbiorcze; sloty/bufory/BLAS wracają do puli dopiero po właściwym submission.

## 4. Sekwencja wykonania

Każdy krok jest całą odpowiedzialnością `[independent]`, wykonywaną przez
oddzielny kontekst. Dobór modelu według złożoności w momencie dispatchu.
`comments: default NONE`. Kroki zależne nie biegną równolegle na tych samych
plikach. Dopuszczalne są pośrednie niekompilujące się etapy tej samej wymiany;
nie utrzymujemy działającego starego świata jako fallbacku.

### Krok 1 → Zachować skończony materiał rendererowy [independent]

Przed odcięciem PackedScene zapisać wejściowy manifest wybranych scen,
współdzielonych assetów, ustawień, kamer i wymaganych ruchów. Utworzyć dane
konwersji tylko dla jawnej listy rendererowych fixtures. Narzędzie jednorazowe
czyta SceneState/zasoby lub ograniczony parser danych; nie przenosi skryptów.
Nie uruchamia scen ani kodu gry podczas konwersji. Wypełnić osobno skończone
native recipes dla obiektów powstających wyłącznie w .gd.

Dlaczego teraz: późniejsze usunięcie Node-world nie może zniszczyć jedynego
opisu scen porównawczych. To authoring danych, nie uruchomienie nowych testów.
Narzędzie nie staje się częścią runtime loadera i zostaje usunięte po konwersji.

Allowlist w `demos/rtxdi_manual/` (docelowo te same ścieżki z `.escn`):

| Scena | Zachowane dane i native recipe |
| --- | --- |
| main.tscn | Galeria, cztery typy lights, environment, alternatywne camera poses; motion caster/morph, renderer modes, light removal/reorder, capture |
| test.tscn | zdm2, emissive cube, kamera i Box/ReflectiveSphere attachments; DDGI freeze/views i capture trajectory |
| shadows_merged.tscn | Facing/culling/mirrored transforms, double-sided shadows, ortho camera, 2-element MultiMesh; merged threshold |
| shadows_expanded.tscn | Ta sama macierz z expanded threshold |
| energy_directional.tscn | Szara płaszczyzna, directional light i kontrolowane materiały/environment |
| energy_emission.tscn | Emission-only płaszczyzna i ustawienia |
| unsupported.tscn | Custom light(), anisotropy/alpha diagnostics oraz galeria |
| microgeometry/scene.tscn | Dragon/seam/floor, native 3-element MultiMesh, rigid/morph motion, orbit, freeze/debug/error/far, unload/reload, emissive toggle, mesh roundtrip |
| microgeometry_stress/scene.tscn | Native 100×100 checkerboard: 5000 Lucy + 5000 Thai, shared assets, normalization/placement/floor/camera, orbit i timing |

Konwerter odczytuje PackedScene::get_state() i SceneState get_node_*/property_*
z `scene/resources/packed_scene.h:178`, bez instantiate(). Rozwiązuje cztery
`geometry_parts/{cube,sphere,plane,deformer}.tscn` wrappers, ich geometry.gltf
instancing/hidden sibling overrides oraz effective defaults. W razie złożoności
odtworzyć te skończone dane natywnie zamiast rozszerzać format konwertera.
Nieznane dane raportować. Zachować źródłowe meshes/materials/textures wraz
z morph target Deformer i współdzieleniem Lucy/Thai. Stress: wysokość2m,
clearance0.25m, Lucy X=-π/2, orbit delta*0.08, setup poza steady-state pomiarem.
DDGI: 8/40/8/40 rendered frames ruch/hold/powrót/recovery, rotation24°,
pose przed post-draw capture. HUD CanvasLayer/Label i secondary viewport usunąć;
komendy zachować w native fixture controls, panelu edytora i logach.

### Krok 2 → Wprowadzić Flecs i natywny schemat komponentów [independent]

Nowe pliki `scene/entity/entity_world.{h,cpp}`, `entity_id.h`,
`entity_components.h`, `entity_component_schema.{h,cpp}`, `scene/entity/SCsub`,
`misc/scripts/entity_component_codegen.py`; zmiany `scene/SCsub`,
`scene/register_scene_types.cpp`, odpowiednich źródeł SCons i rejestru licencji.
Flecs vendorować z przypiętego źródła z licencją i receptą aktualizacji;
nie ręcznie edytować wygenerowanego/thirdparty kodu.

Zaimplementować ID, mapowanie rezydencji, ownership świata, typowane operacje
komponentów i generowanie schematu. Rejestracja danych native nie przechodzi
przez ClassDB dla każdej encji. Ref assetów mają prawidłowe ctor/move/dtor;
Flecs hooks muszą respektować nietrywialne typy Godota. Rejestrację schematów
usunąć razem z ręcznymi duplikatami, nie dokładać drugiej listy właściwości.

Dlaczego teraz: wszystkie kolejne dane i operacje potrzebują jednej tożsamości
oraz opisu pól. Kod runtime nie zależy od bibliotek ani nagłówków edytora.

### Krok 3 → Wymienić cykl sceny, hierarchie i transformacje [independent]

Nowe `scene/entity/entity_scene_runtime.{h,cpp}`, `entity_transform_system.{h,cpp}`;
zmiany `main/main.cpp`, `scene/main/scene_tree.{h,cpp}` i konsumentów obecnego
world lifecycle. Źródła zastępowanych zachowań: `scene/main/node.cpp`,
`scene/3d/node_3d.cpp`, `scene/resources/3d/world_3d.cpp`.

Gra uruchamia EntityWorld; MainLoop/okno/viewport są usługami, nie encjami-proxy.
SceneTree pozostaje gospodarzem aplikacji edytora tam, gdzie wymaga go Control.
Usunąć wejście PackedScene jako current world, autoload scen Node i ich
world tick/notification. Nie zmieniać wewnętrznego UI na Flecs.
Zaimplementować Parent, reparent, visibility inheritance gdy jawnie użyta,
dirty transforms, interpolację, double global translation i reset teleportu.
Scenariusz renderera, kamera, przestrzeń fizyki/nav należą do kontekstu świata;
nie wymagają World3D z Camera3D pointers.

Dlaczego teraz: to prawdziwa wymiana właściciela stanu, przed konsumentami.
Fail/partial startup zwalnia utworzone komponenty i uchwyty w odwrotnej kolejności.

### Krok 4 → Natywny dokument, prefab i operacje transakcyjne [independent]

Nowe `scene/resources/entity_scene.{h,cpp}`, `scene/entity/entity_scene_io.{h,cpp}`,
`entity_scene_commands.{h,cpp}`; rejestracja loader/saver w scene registration,
`doc/classes/EntityScene.xml` dla zasobu dokumentu i odpowiedni SCsub.
Wykorzystać `core/io/resource_loader.*`, ResourceUID, FileAccess i PCK.

Rozszerzenie `.escn` jest obecnie zajęte przez EditorSceneFormatImporterESCN
w `editor/import/3d/resource_importer_scene.cpp:get_extensions/import_scene`;
stara implementacja ładuje PackedScene i wywołuje instantiate(). W TYM kroku
usunąć/unregister ten importer wraz z deklaracją i rejestrującymi konsumentami.
Nowy loader EntityScene ma wyłączną własność `.escn` od pierwszego zapisu;
nie odkładać tego konfliktu do kroku8. Dotychczasowe ESCN nie są kompatybilne.

Wprowadzić format i adresowalne rekordy, resolver assetów, prefab overrides,
clone/remap i lifecycle podzbioru. Stary PackedScene nie jest magazynem danych
nowego świata. Zostaje tylko tam, gdzie faktycznie wymaga go zachowane UI/
narzędzia, bez ścieżki otwarcia świata Node. Dla konwersji zużyć manifest kroku1.
Usługi dokumentu nie dublują rezydentnych komponentów jako Object properties.

Dlaczego teraz: edytor i eksport potrzebują wspólnego trwałego modelu.
Zapis/reload/prefab nie zależą od aktualnych uchwytów Flecs lub renderer RID.

### Krok 5 → Wpiąć encje bezpośrednio w przygotowanie sceny renderera [independent]

Zmiany `servers/rendering/renderer_scene_cull.{h,cpp}`,
`rendering_method.h`, `rendering_server_default.{h,cpp}`,
`renderer_geometry_instance.h`, `renderer_rd/storage_rd/render_data_rd.h`,
`renderer_rd/forward_clustered/render_forward_clustered.{h,cpp}`,
`render_raytracing.{h,cpp}`, `micro_geometry_selection.{h,cpp}` oraz native
producer w `scene/entity/entity_render_system.{h,cpp}`.

Przebudować istniejący frontend i owner-dependent consumers w tej samej zmianie.
Encje publikują zmiany i membership bez odtwarzania ogólnego Cull Instance
setter po setterze. Usunąć zastępowane struktury/pola/wywołania, a nie zostawić
ich pod nową nazwą. Zachować tylko render-owned slots, history, asset caches,
BVH/candidate streams i GPU data potrzebne wykonaniu klatki.

Microgeometry zachowuje permanent membership, GPU-written motion/TLAS records,
change-only uploads i obecny worker flow. Kamera nie powoduje CPU przebudowy
całej populacji eligible meshes. Conventional/deformed geometry nadal dostaje
właściwe kandydatury i dane; flattening nie oznacza przymusowej konwersji do
rigid microgeometry. Raster, RT i shadows mają osobne zakresy widoczności.
Przenieść faktycznie używane light/decal/fog/GI/projector dane, nie uznawać
wszystkich starych pairings za martwe. Assets/DAG/pages/shared cuts/BLAS,
Slang, RD/Vulkan, RTXDI/DDGI/PT/NRD/DLSS zostają w obecnych odpowiedzialnościach.

Wynik pickingu to native render handle+generation rozwiązany do EntityId;
nie ObjectID z wpisanym numerem Flecs. Gizma/grid/debug draw używają rendererowych
danych narzędzi, nie Node'ów świata. Zaktualizować/wycofać world-facing bindingi
RenderingServer oraz XML, zastąpić wszystkich ich pozostających konsumentów.

Dlaczego teraz: edytor musi mieć natywnie renderowalny i wybieralny świat.
Nie dodawać osobnego grafu CPU/GPU ani nowego backendu. Domknąć free/invalidation,
zniknięcie encji w kolejce, reuse slotów oraz asset reload we wszystkich ścieżkach.

Aktualne konkretne kotwice baseline b337:

- `renderer_scene_cull.cpp:_scene_cull:3207` iteruje conventional_instances;
  `_instance_update_cull_domain:4356` zmienia domenę. Zachować zmianowy routing.
- `render_forward_clustered.cpp:_update_dirty_geometry_instances:5312`
  publikuje dirty/motion aging przez update_persistent_instances; publisher
  `render_raytracing.cpp:5260` ma otrzymać native data zamiast czytać dawnego
  właściciela. Nie dodawać drugiego publishera.
- `_prepare_micro_geometry` w RFC:533 i raytracing:1857 przebudowuje retained
  dane po generation change, nie po samym ruchu kamery.
- `render_raytracing.cpp:5441` identity z instance_rid wymaga zmiany wszystkich
  temporal/emissive/picking konsumentów razem. Slot reuse czeka na completed
  GPU serial, release:5537 publikuje nieaktywność przed retirement.
- `micro_geometry_rt.slang:421` zapisuje geometry/material/motion/TLAS;
  zachować geometry_base/motion_base, SBT/custom index i conventional prefix.
- Zachować device-address dependencies, shared asset/cut/BLAS pins, host-admitted
  TLAS capacity bez same-frame count readback i ordering
  finalize_buffers → build_acceleration_structures → lighting.begin_history
  (`render_raytracing.cpp:4878`). Nie usuwać potrzebnych małych signatures.

### Krok 6 → Edytor encji: outliner, Inspector, picking i undo [independent]

Zmiany `editor/editor_data.{h,cpp}`, `editor/editor_node.cpp`,
`editor/editor_interface.{h,cpp}`, `editor/scene/scene_tree_editor.*`,
`editor/docks/scene_tree_dock.cpp`, `editor/inspector/editor_inspector.*`,
`editor/inspector/editor_properties.*`, `editor/editor_undo_redo_manager.*`,
`editor/scene/3d/node_3d_editor_viewport.*`, `node_3d_editor_gizmos.*` i ich SCsub.
Nowe wyłącznie potrzebne native view/command klasy pod `editor/scene/entity/`.

Karty edytora posiadają EntityScene document zamiast Node root. Outliner pokazuje
ograniczony widok katalogu/encji; nie tworzy TreeItem dla całego świata. Selection
przechowuje trwałe adresy. Inspector korzysta ze schematów i command API, w tym
multi-edit, defaults, add/remove component i prefab revert. Przebudować getter
wartości EditorProperty; nie dostarczać mu Object proxy.

Undo zachowuje historię UI, celując w jedną długowieczną usługę dokumentu,
ponieważ obecny UndoRedo pomija komendy bez żywego celu ObjectDB. Komendy native
używają ID i before/after data. Zamknięcie dokumentu odłącza historię bez
wywołania na nieistniejącym świecie. Picking/gizma/reparent mają nowe native
targets, zachowują transformacje, kolejność i poprawne undo. Edycja aktywnego
gizma pinuje encję i sprawdza generację po każdym structural merge.

Usunąć stare Node-specific world plugin dispatch i kontrakty edytowania Node;
zachowane generic UI/resource plugins nie dostają dostępu do encji przez Object.
Zaktualizować bindings/XML i specjalizowane narzędzia objęte dalszymi krokami.
Wszystko pod TOOLS_ENABLED. Pierwszy użyteczny przekrój: create mesh entity →
select → edit/gizmo → undo → save/reopen → run entity world.

### Krok 7 → Pozostałe systemy sceny 3D [independent]

Wykonać poniższe odpowiedzialności kolejno, w osobnych kontekstach. Wszystkie
używają schematu kroku2, poleceń kroku4 i natywnego Inspectora kroku6.
Każda usuwa zastępowane Node-world wejścia wraz z konsumentami, bindingami,
XML i rejestracją w swoim kroku, nie odkłada tego do końcowego sprzątania.

#### 7a. Lifecycle, input i fazy symulacji [independent]

Rozszerzyć `scene/entity/entity_scene_runtime.*`, `main/main.cpp:Main::iteration`
i game input w `scene/main/viewport.cpp:Viewport::push_input`. Zachować
Input/InputMap i OS events; natywne systemy dostają snapshot tick/frame,
Controls edytora zachowują focus i obsługę zdarzeń.

Fixed: merge → input/previous poses → animacja/root motion i intencje ruchu →
komendy fizyki/step → kopia wyników → dynamic transforms/events → hierarchia
i attachments → merge. Frame: animacja wizualna, interpolacja, audio, renderer.
Dynamic body jest właścicielem simulation pose; animacja/nav dostarcza intencję
lub jawnie przełącza ciało w kinematic. Render interpolation nie zapisuje
simulation pose. Enabled i fixed/frame/manual domain są jawne, bez dziedziczenia
Node process priority. Native timers/events używają generacyjnych adresów;
nie przenosić Object Tween ani SceneTreeTimer jako ukrytych celów świata.

Aktywacja jest transakcyjna. Usunięcie unieważnia callbacks/jobs, wyłącza udział
w systemach, zwalnia zależności odwrotnie do tworzenia. Wyniki async zawierają
world/entity generation; nie pointer do komponentu.

#### 7b. Box3D i natywne collision assets [independent]

Vendorować Box3D commit `47d7f7cc7e091142c08d11dc7d2e493c5d34f536`, licencję
i SCons C17. `BOX3D_DOUBLE_PRECISION` musi być zgodne w bibliotece i wszystkich
konsumentach. Nowe `scene/entity/entity_physics_system.*` i collision asset
storage zastępują world dispatch PhysicsServer3D, bez adaptera tego serwera.
Zmienić także `scene/resources/3d/shape_3d.*`: obecnie Shape3D posiada RID starego
serwera; shared collision Resource ma przechowywać natywne dane/cooked asset.
Usunąć stare backend registration/settings oraz world consumers w tej wymianie.

Świat posiada b3WorldId; komponenty body/collider/joint mają natywne uchwyty.
Zakres: static/kinematic/dynamic, convex shapes, static mesh/heightfield,
runtime compounds, layers/masks, materiały, siły/prędkości, sensory, joints
dostępne w przypiętym API i character mover. Mesh/heightfield/baked compounds
są static-only; pożyczona geometria żyje dłużej niż shapes. Reimport wymienia
collidery na granicy fazy i zachowuje stary asset do usunięcia ostatniego shape.

Character controller przenosi grounded/floating, slope/floor snap i moving
platform zachowania z `scene/3d/physics/character_body_3d.cpp`, wraz z teleportem
i usunięciem platformy. Area gravity/damping to native policy sensor memberships.
Zapytania ray/shape/overlap zwracają encję i subshape, nie ObjectID.

Simulation owner wywołuje b3World_Step poza zwykłym zadaniem puli. Enqueue/finish
callbacks używają istniejącego WorkerThreadPool i join; nie blokować parent job
na dzieciach w schemacie grożącym deadlockiem. Przed kolejnym step kopiować
transient body/contact/sensor/joint events. Filter/presolve czytają stabilne
dane bez mutowania świata.

**Proponowane dodatkowe wyłączenie do zatwierdzenia z planem:** SoftBody3D
i Generic6DOFJoint3D nie mają odpowiedników w tym API. Usunąć ich world support
i jasno odrzucać authoring/import; nie budować drugiego solvera. To nie jest
wcześniejsza decyzja właściciela ani obietnica pełnej zgodności fizyki Godota.

#### 7c. Animacja, skeleton, morph i narzędzia [independent]

Nowe `scene/entity/entity_animation_system.*` i native pose/playback types;
przebudować `scene/animation/animation_mixer.*`, `animation_tree.*`, blend tree,
blend spaces i state machine evaluator oraz odpowiadające timeline/graph UI.
Zachować interpolację/compression `scene/resources/animation.cpp`; zastąpić
NodePath/ObjectID/Object::set_indexed targety stabilnymi entity/component/field,
bone i morph IDs. Clip/graph assets są współdzielone; clocks/blends/transitions,
captured defaults, event cursors i root motion należą do instancji ECS.

Obsłużyć transform/value/Bezier/bone/morph/audio/nested-clip tracks. Method-call
i script/expression transitions odrzucać jawnie w ramach scripts-out. Graf
ocenia native context zamiast AnimationTree*. Nie tworzyć ukrytego player Node.

Skeleton to asset bone/rest/inverse-bind plus native local/global pose buffers;
kości nie muszą być encjami. Attachments adresują stabilne bone IDs. Przenieść
kolejność base pose → modifiers → skin publication z Skeleton3D::_notification.
Native modifier entries obejmują IK, look-at, retarget i spring; ich narzędzia
edytują native config. Ragdoll/physical bones używają Box3D i jawnych faz
animation→physics→pose, w granicach joints kroku7b. World posiada skeleton RIDs
i deform history; używa obecnego mesh/skeleton storage oraz ingress kroku5.

#### 7d. Nawigacja i audio [independent]

Nowe `scene/entity/entity_navigation_system.*` zastępują własność Node w
`scene/3d/navigation/`. Zachować NavigationServer3D i algorytmy path/avoidance.
Dodać bulk result drain z agent RID/map iteration/request ID do serwera,
`modules/navigation_3d/nav_agent_3d.*` i dummy implementacji: obecny bezpieczny
avoidance velocity powstaje przy dispatch Callable, samo agent_get_velocity
nie jest dowiedzionym zamiennikiem. Żadnych per-agent Object callback proxies.
Path/target reached/unreachable/repath/off-mesh traversal mają jawny stan;
wynik to intencja ruchu. Bake bierze native geometry przez add_mesh/add_faces,
nie scene Node parser. Usunięcie wyłącza avoidance i odrzuca stare wyniki.

Nowe `scene/entity/entity_audio_system.*` przejmują emitter/listener/zone z
`scene/3d/audio_stream_player_3d.*`, AudioListener3D i Area3D audio. Zachować
AudioServer playback/bus APIs, attenuation/panning/cone/filter/Doppler.
Jeden listener z fallbackiem do native camera; editor audition osobno.
World posiada voices, immutably publikuje parametry; stop/delete/completion
waliduje generacje. Audio tracks używają tej samej usługi i określają seek/loop.

Nav map/region ma jawny double origin, lokalna geometria pozostaje float;
audio odejmuje listener position w double przed narrowing. Box3D global positions
są double, broadphase nadal float: ten plan nie obiecuje gotowego solvera dla
dowolnie ogromnego aktywnego obszaru.

#### 7e. Zamknięcie zakresu pozostałych danych sceny [independent]

Komponenty rendererowe kroku5 obejmują mesh/MultiMesh, lights wszystkich obecnych
typów, camera/environment, decals, fog volumes, reflection/GI settings oraz
particles z istniejącym backendem i natywnym emitter lifecycle. Conventional
path zachowuje shader deformation/alpha/time, custom bounds, fades, layers,
visibility dependencies, uniforms i shadows; nie wyłączać ich bez komunikatu.
Specjalizowane authoring tools tych danych korzystają z edytora encji.

Nie obiecywać 1:1 portu każdej klasy upstream: CSG/GridMap authoring, Node scene
multiplayer/spawner/synchronizer i narzędzia wymagające skryptów nie otrzymują
zamiennika w tym planie; ich wejścia świata usunąć i odrzucać jawnie. Propozycja
wyłączenia CSG/GridMap jest częścią zatwierdzanego zakresu, nie wcześniejszą
decyzją właściciela. Shared gotowe meshes pozostają normalnymi assetami.
Low-level network peers/transports mogą zostać; nowy RPC/replication protocol
to osobne zadanie. Nie implementować HTML/CSS ani scen 2D.

### Krok 8 → Import, eksport i rendererowe sceny docelowe [independent]

Zmiany `editor/import/3d/resource_importer_scene.*`, odpowiednich importerów
glTF, `editor/export/editor_export_platform.cpp`, EditorFileSystem dependency
discovery, `main/main.cpp`, `project.godot` wybranych scen, nowe native `.escn`.

Importer ma wytwarzać encje/komponenty i shared assets z trwałymi source IDs,
nie PackedScene jako wynik świata. Zachować algorytmy mesh/material/skeleton
importu. Przejściowe struktury parsera nie są światem edytora/runtime; nie
uruchamiają skryptów, nie trafiają do SceneTree. Nie utrzymywać Node-based
post-import plugin API jako równoległej ścieżki authoringu świata.
Reimport aktualizuje rekordy po source ID i respektuje lokalne nadpisania;
konflikty trafiają do dokumentu, nie powodują cichego znikania zmian.

Eksport selected scenes rozpoznaje EntityScene i pełne dependency metadata,
także dla niezaładowanych sekcji, tekstur i danych microgeometry. Runtime
czyta ten sam model z PCK i poza nim. Main scene setting wskazuje `.escn`;
brakujące dane dają czytelny błąd bez fallbacku do Node scene.

Zamknąć konkretne wejścia: EditorNode::load_scene_or_resource/load_scene/
_save_scene, filtry quick-open/save i main-scene validation; rejestrację importu
i scene preview generator w editor_node.cpp; resource_importer_scene.cpp
get_save_extension/get_resource_type/import; editor_export_platform.cpp
selected-scenes filter i dependency traversal. Drag/drop viewportu zastępuje
PackedScene `_create_instance` natywnym prefabem. Rejestrację zmienić także
w editor/register_editor_types.cpp i scene/register_scene_types.cpp.
Przenieść project.godot i microgeometry/export_presets.cfg.example; stary
manual_export.gd nie wymusza portu EditorPlugin/SceneTree scripting.

### Krok 9 → Zamknąć starą ścieżkę świata i weryfikację [independent]

Usunąć nieużywane Node-world registration/callers/bindings, stare scene plugin
wejścia, world .tscn/.gd z przeniesionej listy i jednorazowe narzędzie konwersji.
Nie usuwać niepowiązanych plików właściciela. Rejestrację typów, SCons i XML
uporządkować względem faktycznie zachowanej infrastruktury UI i assetów.
Przejść końcową macierz poniżej; uaktualnić project-state i kierunek projektu
rzeczywistymi wynikami, nie zastępować ich samym statusem kompilacji.

Ten krok usuwa tylko pozostałości i sprawdza domknięcie: zastępowany autorytet
i jego działający stary ingress muszą zniknąć już w kroku swojej wymiany.

## Źródła zewnętrzne przypiętych decyzji

- [Flecs v4.1.6](https://github.com/SanderMertens/flecs/tree/fb55f3c25660425cfe1bc4cf5e6bff8b3f18a9b8)
- [Clang LibTooling](https://clang.llvm.org/docs/LibTooling.html)
- [Box3D API](https://github.com/erincatto/box3d/blob/47d7f7cc7e091142c08d11dc7d2e493c5d34f536/include/box3d/box3d.h)
- [Box3D task contract](https://github.com/erincatto/box3d/blob/47d7f7cc7e091142c08d11dc7d2e493c5d34f536/include/box3d/types.h)
- [Box3D precision limits](https://github.com/erincatto/box3d/blob/47d7f7cc7e091142c08d11dc7d2e493c5d34f536/docs/large_worlds.md)

## 5. Weryfikacja i warunek ukończenia

To zaplanowane sprawdzenia przyszłej implementacji, nie wykonane wyniki.

- Ordinary editor, precision=double editor i template_debug kompilują się z
  tym samym finalnym źródłem. Codegen działa w czystym buildzie oraz po zmianie
  deklaracji komponentu; brak linkowania edytora do template.
- Otworzyć pusty edytor i projekt bez wymuszania ukrytej alternatywnej ścieżki
  renderowania. Stworzyć encję, komponent, prefab/instancję, reparent/duplicate,
  multi-edit, undo/redo, zapis i reopen. Potwierdzić brak Node/Object wrapperów
  przez rzeczywistą strukturę świata, nie tylko wygląd outlinera.
- Uruchomić docelowe sceny na Vulkan, jeden game viewport: kamera/motion,
  microgeometry selection/freeze, cienie/offscreen RT, GI, materiały i asset
  reload. Export/PCK uruchomić poza katalogiem źródeł projektu.
- Usunąć/odtworzyć batch encji, zmienić archetyp i prefab oraz unload/reload
  podzbioru z referencją zewnętrzną. Brak dostępu przez stare uchwyty, utraty
  trwałego ID lub przedwczesnego zwalniania zasobów GPU.
- Współrzędne 100 km+ sprawdzić na małej zawartości w dużym oddaleniu, z ruchem
  kamery, hierarchią, fizyką i motion vectors. To dowód precyzji tej sceny, nie
  ukończony world streaming ani nieograniczona pojemność.
- Ręcznie sprawdzić native body/character/sensor, platform deletion, collision
  asset reload; clip/blend/morph/skin i bone attachment, root motion/ragdoll;
  nav path/repath/avoidance oraz usunięcie celu podczas query; audio emitter/
  listener/zone i stop po unload. Każdy przypadek obejmuje edycję właściwego
  komponentu, zapis/reopen i grę; osobno odnotować ograniczenia Box3D.
- Porównanie dense renderer fixture z aktualnym baseline w tych samych
  ustawieniach i trasie, bez profilu dla wall FPS; osobno czasy CPU/GPU.
  Warunek: nie przywrócić usuniętej pracy pełnej populacji ani istotnej regresji
  wynikającej z nowego ingress. Progu FPS nie wyznaczać z historycznych danych
  innej konfiguracji; raportować liczebności, warunki i ograniczenia porównania.

Ukończenie oznacza działającą edycję/zapis/uruchomienie natywnego świata i
przeniesione funkcje danych objęte krokiem7. Nie oznacza obsługi skryptów,
Node plugins, dowolnych starych projektów, streamingu 100 km+ lub HTML/CSS UI.

## 6. Workflow i granice zmian

Po zatwierdzeniu zapisać plan w repo i osobno commitować. Pracować na aktualnej
gałęzi, bez worktree/stash/reset; staged set sprawdzać przed każdym commitem.
Owned source kroku commitować przed świeżym review exact SHA+cumulative diff.
Automatyczne testy pozostają poza autoryzacją. Review/proof zakres dobierać
według aktualnych reguł repo i jawnych późniejszych poleceń właściciela;
nie przenosić automatycznie wyjątku z poprzedniego zadania optymalizacji.

Każdy dispatch zawiera właściwe klasy failure taxonomy: ClassDB/XML (1),
lifetime (2), renderer synchronization (3), jawny wyjątek kompatybilności (4),
SCons/TOOLS/precision (5), faktyczne dowody (6), wątki i błędy (7), scope (9).
Nawigacja: clangd/clang-nav z root compiledb → deklaracja/implementacja →
bindings/XML → history/upstream; rg tylko kotwica lub jawnie ograniczony fallback.
Nie dodawać ogólnych frameworków dla jednego miejsca integracji. Mniejsze
prywatne decyzje pozostają implementerowi; zmiana właścicielskiej granicy
świata, skryptów, kompatybilności lub reintrodukcja Node wymaga powrotu do właściciela.
