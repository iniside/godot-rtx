# Wymiana modelu sceny na Flecs: research architektury

Data: 2026-09-09 UTC. Research źródeł i kontraktów, bez implementacji.
Pierwszy jawnie zapisany baseline podczas odczytów: `999bbd18ccffee8132c0a95ec47268318bf7f64f`.
Inne zadania równolegle rozwijają renderer; dokument nie jest audytem ich zmian.

## Cel właściciela i granica tego opracowania

Docelowo scena zawiera wyłącznie encje i komponenty, w edytorze i w grze.
Nie ma Node'a, Objecta ani Resource'a reprezentującego każdą encję lub jej
komponent, także jako wrapperu dla zachowania starego modelu sceny.
Hierarchia pozostaje relacją encji. Flecs jest wybranym fundamentem, wdrażanym
przed world streamingiem, aby streaming nie powstał na zależnościach od Node.

Dodatkowa decyzja właściciela podczas researchu: gęste populacje obiektów
powinny mieć płaską strukturę; nie zakładamy skomplikowanej hierarchii dla
każdego obiektu. Hierarchie są wspierane tam, gdzie wynikają z faktycznej
zależności zachowania lub authoringu. To założenie danych świata, nie twardy
limit głębokości ani zakaz wszelkich hierarchii.

Docelowe światy mają ponad 100 km i dużą gęstość, mogą być zapisane na dysku,
generowane lub łączyć oba sposoby. Autor nie ma ręcznie dzielić świata na sceny
ani zarządzać rezydencją. Ten cel wymaga ograniczonego zbioru danych w pamięci;
nie oznacza materializowania całego świata jako żywych encji Flecs.

Doprecyzowanie właściciela podczas researchu: sceny 2D nie są wspierane,
a UI gry zostanie zastąpione osobną warstwą pisaną w HTML/CSS. Nie projektujemy
odpowiedników Node2D/Control w ECS ani Node-based UI jako fallbacku świata gry.
Wybór silnika HTML/CSS i jego wdrożenie są osobnym zadaniem; tutaj potrzebna
jest granica danych/komend między UI a światem encji, bez wrapperów encji.

Kontrolki aplikacji edytora mogą nadal korzystać z infrastruktury Godota.
Zasoby współdzielone, np. mesh, materiał lub dźwięk, nie są encjami świata:
zakaz wrapperów encji nie jest poleceniem usunięcia całego `Object`/`RefCounted`
z silnika. Zachowanie tej infrastruktury nie tworzy drugiej sceny świata.

Research obejmuje architektoniczną mapę odpowiedzialności sceny, nie tylko statyczne
meshe. Nie jest zatwierdzonym planem wykonania ani zgodą na rozpoczęcie zmian
silnika. Automatyczne testy, buildy i uruchomienia nie były wykonywane.
Multiview, XR, VR i split screen pozostają poza zakresem projektu.

## Wniosek architektoniczny

Flecs pozwala zachować hierarchiczny sposób authoringu i dostarcza magazyn
komponentów, relacje, zapytania, prefaby oraz planowanie systemów. Wymieniana
jest obecna reprezentacja sceny i jej kontrakty z podsystemami, a nie sama idea
drzewa obiektów. Nie ma podstaw do twierdzenia, że trzeba napisać od zera cały
renderer, edytor UI lub wszystkie niskopoziomowe serwery Godota.

Największe granice wymiany to: trwała tożsamość i zapis, natywna edycja pól,
model skryptów, celowanie animacji i zdarzeń oraz własność zasobów podsystemów.
Bezpośrednie zastąpienie `Node*` przez `flecs::entity` nie zamyka tych kontraktów.

## Sprawdzone kontrakty Godota: zapis i uruchomienie

| Źródło w tym checkoutcie | Obecny kontrakt | Konsekwencja dla sceny encji |
| --- | --- | --- |
| `scene/resources/packed_scene.h:38`, `SceneState` | `NodeData`, owner/parent, NodePath, ID paths, właściwości i połączenia sygnałów | Nowy zapis musi przechowywać encje/komponenty/relacje; zmiana samego typu wyniku instantiate nie wystarczy |
| `scene/resources/packed_scene.cpp:865`, `SceneState::_parse_node` | Selekcja zapisywanych Node'ów według owner i editable instance | Własność dokumentu/prefabu trzeba oddzielić od rodzica transformacji i rezydencji |
| `scene/resources/packed_scene.cpp:2580`, `PackedScene::instantiate` | Wynik `Node*`, stan edycyjny na Node, ścieżka sceny i notyfikacja instancjacji | Uruchomienie świata musi ładować encje bez uprzedniej instancjacji drzewa Node |
| `scene/resources/packed_scene.cpp:2686`, `_bind_methods`; `doc/classes/PackedScene.xml` | Publiczne `pack(Node*)` i `instantiate() -> Node` | To stabilny upstream API, nie prywatny detal forka; zakres zerwania zgodności wymaga decyzji |
| `main/main.cpp:4511`, `4833`, `Main::start` | Uruchomienie gry rozpoznaje SceneTree, ładuje PackedScene i wywołuje `add_current_scene` | Zmiana obejmuje bootstrap gry, wybór głównego świata i obsługę niepowodzenia ładowania |
| `main/main.cpp:4573`, ładowanie autoloadów | Autoload sceny instancjuje Node | Potrzebny kontrakt usług/globalnych danych świata; stare autoloady nie zadziałają automatycznie |
| `core/io/resource_uid.cpp:162`, `ResourceUID::add_id`; `doc/classes/ResourceUID.xml` | Mapowanie ID zasobu na ścieżkę pliku | Zachować identyfikację assetów, ale nie utożsamiać jej z tożsamością encji wewnątrz świata |
| `core/io/resource_format_binary.cpp:1148`, `ResourceFormatLoaderBinary::load` | ResourceLoader, FileAccess, tryby cache zasobów | Infrastruktura odczytu jest użyteczna; nie dostarcza sama nowego modelu sceny |
| `core/io/file_access_pack.cpp:472`, `FileAccessPack::get_buffer` | Odczyt zakresu danych pliku w PCK | Nie ma potrzeby automatycznego zastępowania kontenera PCK; eksport nowych zależności wymaga osobnej integracji |
| `scene/SCsub`, `scene/resources/SCsub` | Osobne katalogi main/gui/3d/2d/animation/audio/resources i źródła zasobów microgeometry | Zachować rozdział zasobów od scene nodes; nowa rejestracja i kod ECS muszą działać także bez TOOLS_ENABLED |

Publiczny kontrakt PackedScene potwierdza również
[aktualna dokumentacja upstream](https://docs.godotengine.org/en/stable/classes/class_packedscene.html).
Lokalne `SceneState` ma już ID paths, ale nadal opisuje Node'y; obecność ID nie
oznacza gotowego formatu encji ani rejestru obiektów niezaładowanego świata.

## Flecs: możliwości sprawdzone w konkretnej wersji

Najnowsze wydanie podczas researchu: **v4.1.6 z 2026-06-29**, commit
`fb55f3c25660425cfe1bc4cf5e6bff8b3f18a9b8`. Zaobserwowany master:
`16504b2eaf51d5fd8c75b7f2315a49170861643c` z 2026-09-09. Header mastera nadal
deklaruje 4.1.6, więc makro wersji nie zastępuje przypięcia commita. Poniższe
ustalenia dotyczą taga. [Wydania](https://github.com/SanderMertens/flecs/releases).

| Mechanizm | Sprawdzone zachowanie | Znaczenie dla naszego modelu |
| --- | --- | --- |
| `ChildOf` | Pary z różnymi rodzicami rozdzielają tabele; usunięcie rodzica usuwa dzieci | Dobry dla szerokich grup; nie zakładać niskiego kosztu miliona małych drzew |
| `Parent` | Dostępny od 4.1.5; rodzic w danych, tabele według głębokości, uporządkowane dzieci; usunięcie pojedynczego dziecka O(liczby rodzeństwa) | Kandydat dla wielu małych prefabów, nie każdego kształtu hierarchii |
| Transform traversal | Parent ma ograniczenia Or/Not; ogólne wyszukiwanie w górę jest niecache'owane i kosztowne | Obliczenia według głębokości; jeśli storage mieszane, przeplatać je poziomami |
| Ogólne `DontFragment` | Inne ograniczenia niż Parent: m.in. brak JSON i części zachowań/operatorów relacji | Nie stosować automatycznie do wszystkich relacji jako rozwiązania fragmentacji |
| `IsA`/prefaby | Dziedziczenie i własne nadpisania komponentów, auto-override, instancjacja dzieci | Nie definiuje nadpisań pojedynczych pól i pełnych operacji authoringu |
| ID encji | 64-bitowy uchwyt z częścią wersjonującą życie encji, recykling numerów | Wykrywanie starego uchwytu nie jest trwałą tożsamością świata na dysku |
| Meta/JSON | Opis typów, serializacja odbitych wartości, adaptery opaque; nieścisły odczyt może pomijać wartości bez refleksji | Narzędzia, nie gotowy format trwałego świata |
| Staging/pipelines | Odroczone zmiany strukturalne, merge, deklaracje dostępu systemów; granice readonly wymagają wyłącznego dostępu | Jawne fazy i integracja schedulera, nie dowolne mutacje z workerów |
| Observers | Nieokreślona kolejność; OnSet nie obejmuje zwykłych zapisów przez wskaźnik bez modified() | Dirty state/publikacja nie mogą polegać wyłącznie na observerach |

Źródła przypięte do wydania:
[hierarchie](https://github.com/SanderMertens/flecs/blob/fb55f3c25660425cfe1bc4cf5e6bff8b3f18a9b8/docs/HierarchiesManual.md),
[traits](https://github.com/SanderMertens/flecs/blob/fb55f3c25660425cfe1bc4cf5e6bff8b3f18a9b8/docs/ComponentTraits.md),
[prefaby](https://github.com/SanderMertens/flecs/blob/fb55f3c25660425cfe1bc4cf5e6bff8b3f18a9b8/docs/PrefabsManual.md),
[encje](https://github.com/SanderMertens/flecs/blob/fb55f3c25660425cfe1bc4cf5e6bff8b3f18a9b8/docs/EntitiesComponents.md),
[JSON](https://github.com/SanderMertens/flecs/blob/fb55f3c25660425cfe1bc4cf5e6bff8b3f18a9b8/include/flecs/addons/json.h),
[systemy](https://github.com/SanderMertens/flecs/blob/fb55f3c25660425cfe1bc4cf5e6bff8b3f18a9b8/docs/Systems.md),
[observers](https://github.com/SanderMertens/flecs/blob/fb55f3c25660425cfe1bc4cf5e6bff8b3f18a9b8/docs/ObserversManual.md).

Dokument prefabów zawiera nieaktualne uogólnienie, że dzieci nie dziedziczą.
Przypięty [tree_spawner.c](https://github.com/SanderMertens/flecs/blob/fb55f3c25660425cfe1bc4cf5e6bff8b3f18a9b8/src/tree_spawner.c)
dodaje IsA(prefab_child) dla Parent. Zwykłe dzieci instancji Parent nie otrzymują
skopiowanych nazw, podczas gdy warianty prefabów mogą je otrzymywać. Trwałych
referencji nie opierać na nazwach dzieci.

Flecs ma automatyzację refleksji C++ przez `ECS_STRUCT` i `ECS_ENUM`: opis jest
konsumowany przy rejestracji komponentu. Nie trzeba ręcznie dublować list pól
dla prostych deklaracji. To parser deklaracji makra, nie ogólna refleksja C++.
Patrz [auto struct](https://github.com/SanderMertens/flecs/blob/fb55f3c25660425cfe1bc4cf5e6bff8b3f18a9b8/examples/cpp/reflection/auto_define_struct/src/main.cpp)
i [adapter vector](https://github.com/SanderMertens/flecs/blob/fb55f3c25660425cfe1bc4cf5e6bff8b3f18a9b8/examples/cpp/reflection/ser_std_vector/src/main.cpp).
Generator AST pozostaje rekomendacją dla typów Godota i semantyki authoringu;
powinien emitować Meta oraz metadane silnika z jednego źródła. Meta samo nie
generuje trwałych ID ani zasad undo/prefab overrides.

W świetle decyzji o płaskich gęstych populacjach podstawowy przypadek ma być
bez rodzica transformacji. Nie tworzyć unikalnego rodzica/prefabu-drzewa na
każdy statyczny element tylko w celu uporządkowania edytora. Nie ma potrzeby
projektowania od razu uniwersalnego schedulera dwóch storage hierarchii.
Opis Parent/ChildOf powyżej zachowuje fakty upstream; wybór dla potrzebnych
hierarchii pozostaje wąskim kontraktem. Ewentualne ich mieszanie wymaga
poprawnej kolejności zależności, ale nie jest wymaganiem pierwszej wersji.

## Sprawdzone kontrakty runtime i podsystemów

| Obszar i źródło | Co zachować | Co zmienić dla encji |
| --- | --- | --- |
| `scene/main/node.cpp:341`; `scene/main/scene_tree.cpp:639,688` | Punkt wejścia pętli aplikacji, infrastruktura edytora | Wejście/wyjście świata, grupy, odroczone usuwanie i callbacki zastąpić fazami ECS |
| `scene/3d/node_3d.cpp:391,412`; `scene/main/scene_tree.cpp:670` | Wymagania transformacji/interpolacji | Relacyjne local/global, dirty state, poprzedni/bieżący stan, teleport i historia; interpolacja renderowania nie nadpisuje symulacji |
| `servers/rendering/rendering_server.h:744`; `scene/3d/visual_instance_3d.cpp:205` | instance_create, scenario, asset RID, materiały, transformacje, widoczność | ECS posiada instancje RID i release; VisualInstance3D nie jest wymagany przez te API |
| `servers/rendering/rendering_server.h:751,771` | Mechanizmy wyszukiwania instancji | Attach/cull używają ObjectID; potrzebna identyfikacja/mapowanie RID→encja, bez podszywania się pod Object |
| `servers/rendering/rendering_server.h:531,577`; `scene/resources/3d/world_3d.cpp:44,52,185` | Kamera/viewport/scenario RID | World3D przechowuje Camera3D* obok scenario/physics space/nav map; nie jest gotowym kontekstem ECS |
| `servers/rendering/rendering_server_default.cpp:740,768` | Obecną granicę admission/FrameContext i przekazanie na render thread | Publikować stan ECS przez tę granicę; nie oddawać backendowi wskaźników do ruchomych komponentów |
| `scene/animation/animation_mixer.cpp:678,709,722`; `doc/classes/Animation.xml` | Assety i użyteczne algorytmy animacji | Mixer wiąże NodePath/ObjectID; potrzebne cele encja/komponent/pole, kości i semantyka method/audio tracks |
| `core/object/script_language.h:156`; `modules/gdscript/gdscript.cpp:407` | Możliwość użycia języka po zaprojektowaniu nowego API | Script::instance_create(Object*) wymaga Objecta; zwykłe przypięcie skryptu do encji odtwarza zakazany wrapper |
| `scene/3d/physics/collision_object_3d.cpp:64`; `servers/physics_3d/physics_server_3d.h:403,469,519` | API RID jako punkt porównania | Body/shape ownership, bezpieczne zmiany po callbackach, cele wyników; docelowy Box3D wymaga wyboru bezpośredniej integracji lub granicy serwera |
| `scene/3d/navigation/navigation_agent_3d.cpp:426`; `servers/navigation_3d/navigation_server_3d.h:192`; `doc/classes/NavigationServer3D.xml:153` | Serwer map/agentów/avoidance | Zależność od parent Node3D i cleanup: callback działa niezależnie od SceneTree; wyłączać i odrzucać spóźnione wyniki |
| `servers/audio/audio_server.h:434`; `scene/3d/audio_stream_player_3d.cpp:308,382` | Miksowanie, busy, streamy, playback | Emitter/listener/zone: dziś Camera3D/AudioListener3D i Area3D; system audio posiada aktywne głosy |
| `modules/multiplayer/scene_rpc_interface.cpp:473`; `modules/multiplayer/doc_classes/SceneMultiplayer.xml` | Transport jako kandydat do zachowania | RPC wymaga Node w SceneTree; replikacja, authority, spawn i cele wywołań muszą być encjami |

To mapa istotnych granic, nie audyt każdego Node'a ani obietnica parytetu
wszystkich funkcji Godota. Nie usuwać kategorii zachowania z docelowego zakresu
tylko dlatego, że pierwszy przekrój pokazuje statyczny mesh.

## Edytor bez Object proxy dla encji

| Kontrakt i źródło | Wymagana zmiana |
| --- | --- |
| `editor/editor_data.h:113`, EditorData::EditedScene | Root, selection i live-edit są Node*/NodePath; zastąpić tożsamością dokumentu/encji, zachować UI kart |
| `editor/scene/scene_tree_editor.cpp:380` | Rekurencyjny Node→TreeItem cache zastąpić ograniczonym widokiem hierarchii i katalogu danych niezaładowanych |
| `editor/docks/scene_tree_dock.cpp:2481`, _do_reparent | Transakcja obejmuje rodzica, kolejność, nazwy, referencje, owner, transformacje, undo i debugger; samo ChildOf tego nie odtwarza |
| `editor/editor_data.cpp:1283`, EditorSelection::add_node | Wymaga aktywnego Node/tree_exiting; selekcja encji rozróżnia unload od usunięcia |
| `editor/inspector/editor_inspector.cpp:5254,4249,3827`; `editor/inspector/editor_inspector.h:259` | Inspector i getter wartości kontrolki wymagają Object; zachować Controls/layout, wymienić dostęp do wartości na schemat/pole/komendę |
| `editor/inspector/editor_inspector.cpp:937` | Revert/pin zależą od Object/Node; potrzebne natywne defaults i prefab overrides |
| `editor/editor_undo_redo_manager.cpp:63,130`; `core/object/undo_redo.cpp:357` | Explicit history ID istnieje, ale wykonanie pomija komendy bez żywego celu ObjectDB; sam custom Callable bez celu nie wystarczy |
| `editor/scene/3d/node_3d_editor_viewport.cpp:746,6238`; `editor/scene/3d/node_3d_editor_gizmos.h:81` | Picking/gizma/commit_transform operują na Node3D; zachować geometrię/matematykę narzędzi, wymienić target, BVH identity i komendy |
| `editor/editor_interface.cpp:692,725,764`; `editor/plugins/editor_plugin.cpp:370,374` | Publiczne edit/handles/root/save związane z Node/Object; nowy kontrakt pluginów świata |
| `editor/import/3d/resource_importer_scene.cpp:74,131,3365,3440` | Importer/post-import wymagają Node, wynik jest PackedScene; potrzebny natywny wynik encji i stabilne source ID do reimportu |
| `editor/export/editor_export_platform.cpp:1364,694` | Selected scenes przyjmuje PackedScene, zależności bierze z EditorFileSystem; zarejestrować dokumenty/prefaby i zależności niezaładowanych danych |

Przepływ docelowy: ograniczony widok dokumentu → trwałe zaznaczenie encji →
schemat komponentu → transakcja → mutacja Flecs w bezpiecznej fazie →
odświeżenie Inspectora, hierarchii i narzędzi. Nie ma drugiego stanu świata w UI.

Undo może zachować obecną historię, jeżeli komendy celują w długowieczną usługę
dokumentu/komend edytora. Taki Object jest infrastrukturą, nie reprezentacją
encji. Alternatywą jest natywny backend komend niezależny od ObjectDB.
Nie wybieramy per-entity proxy po to, aby zachować Inspector.

Publiczne punkty sprawdzono także w lokalnych XML EditorSelection,
EditorInspectorPlugin, EditorProperty, EditorPlugin, EditorInterface,
EditorNode3DGizmo, EditorSceneFormatImporter i EditorScenePostImport.
Istnieją w stabilnym upstream, np.
[EditorSelection 4.4](https://raw.githubusercontent.com/godotengine/godot/4.4-stable/doc/classes/EditorSelection.xml).
Deklaracja Object w XML importera nie znosi lokalnego castu do Node.
Rekomendacja nowego modelu: natywny wynik importera. Ewentualna jednorazowa
konwersja tymczasowego drzewa importu nie może być autorytetem sceny
edytora/runtime i wymaga jawnego uzgodnienia zakresu.

## Kontrakty wymagane od pierwszego etapu Flecs

Poniżej są proponowane odpowiedzialności, nie nazwy istniejących API.

### Tożsamość, relacje i stan niezaładowany

Trzeba odróżnić trwały identyfikator encji, lokalny uchwyt encji Flecs w konkretnym
świecie oraz RID podsystemu. Zapis, undo, referencje między dokumentami i późniejszy
streaming używają trwałej tożsamości. Uchwyt ECS jest przyspieszeniem dostępu do
aktualnie załadowanych danych, a RID reprezentuje zasób renderera/fizyki.

Nie wolno traktować `ObjectID`, `ecs_entity_t` i RID jako wymiennych liczb.
Referencja musi rozróżniać co najmniej cel załadowany, niezaładowany i usunięty.
Unload zwalnia stan runtime; trwałe usunięcie zmienia dokument świata.
Undo usunięcia odtwarza trwałą tożsamość, ale nie zakłada tego samego uchwytu ECS.

Hierarchia transformacji, własność prefabu/dokumentu i przynależność przestrzenna
to różne odpowiedzialności. Nie używać jednego `ChildOf` jednocześnie jako rodzica
obiektu, właściciela zapisu i kontenera streamingu: destrukcja rodzica ma semantykę
usuwania dzieci. Dla zależności przecinającej granicę rezydencji trzeba ustalić,
czy utrzymujemy potrzebnych przodków, zapisujemy odpowiednią bazę transformacji,
czy ładujemy powiązany zestaw razem. To kontrakt fundamentu; wybór polityki
prefetch i rozmiaru komórek może nastąpić później.

Gęsta roślinność, kamienie i inne masowe dekoracje powinny w podstawowym
przypadku być niezależnymi encjami z transformacją i współdzielonymi assetami.
Folder/outliner, kategoria, pochodzenie proceduralne i komórka przestrzenna
mogą grupować je logicznie bez dodawania zależności transformacji. Złożony
obiekt może potrzebować hierarchii, lecz nie wymusza jej na całej populacji.
Płaska struktura ogranicza problem fragmentacji przez unikalnych rodziców;
nie eliminuje kosztów liczby encji, ich komponentów i zasobów podsystemów.

### Schemat komponentów i edycja

Jedna deklaracja komponentu ma zasilać jego rejestrację, dostęp do pól,
serializację i edytor. Jawnie opisujemy semantykę, której nie da się odgadnąć
z typu: jednostki, zakresy, trwałe identyfikatory pól, referencje do encji/assetów,
wartości domyślne oraz trwałość lub przejściowość danych.

Edytor identyfikuje cel edycji przez dokument, encję, komponent i pole; nie
przechowuje wskaźnika do komponentu jako trwałej tożsamości. Zmiana archetypu,
unload lub cofnięcie operacji może unieważnić taki wskaźnik. Komendy przechowują
wartości i identyfikatory, a dostęp do magazynu rozwiązują w chwili wykonania.

Wcześniejszy kierunek Clang AST pozostaje kandydatem dla pełnych deklaracji
C++, a nie dowodem, że Flecs nie ma własnej automatyzacji refleksji.
[LibTooling](https://clang.llvm.org/docs/LibTooling.html) i
[AST matchers](https://clang.llvm.org/docs/LibASTMatchers.html) pozwalają pobrać
deklaracje i ich atrybuty. Generator musi respektować rzeczywiste definicje
builda, obsługiwane typy, układ danych i źródło identyfikatorów schematu.
Nie wymaga to refleksji C++26 ani ręcznej listy każdego pola powtórzonej w edytorze.

### Cykl życia i podsystemy

Świat ECS posiada stan encji i kolejność faz. Systemy obliczają transformacje,
symulację i zmiany renderowania. Integracja z podsystemem posiada jego uchwyty,
tworzenie/usuwanie i obsługę błędów. Nie polegać na kolejności obserwatorów jako
zamienniku całego kontraktu `_enter_tree`, `_ready` i usuwania scen.

Pracownicy mogą przygotowywać dane zgodnie z zadeklarowanym dostępem; publikacja
zmian strukturalnych ma ustalony punkt. ECS nie daje prawa do wywoływania RD,
RenderingServer ani Object API z dowolnego wątku. Render thread pozostaje punktem
synchronizacji i submission zgodnie z kierunkiem obecnej przebudowy renderera.
Usunięcie encji nie może zwalniać RID-u lub pamięci używanej przez klatkę w locie.

### Zapis, prefaby i późniejszy streaming

Trwałe dane świata mają zawierać encje, komponenty, relacje, zależności assetów,
pochodzenie z prefabu oraz lokalne nadpisania. Nie zapisywać surowych tablic
archetypów, pointerów, uchwytów serwerów ani kolejności rejestracji typów jako
trwałego kontraktu formatu.

Logiczny dokument świata powinien już pozwalać adresować paczki i zależności
bez wymogu jednej żywej encji katalogowej na każdy obiekt świata. Nie wybieramy
teraz rozmiaru siatki, algorytmu I/O ani virtual texturing. Wymagamy możliwości
załadowania/wyładowania podzbioru i zachowania referencji oraz lokalnych edycji.

Prefab potrzebuje tożsamości swoich elementów niezależnej od nazwy i kolejności
dzieci. Flecs dziedziczy/instancjuje komponenty, ale engine musi zdefiniować
apply/revert, lokalne usunięcia, zmiany hierarchii i ponowny import. Dane z
generatora wymagają stabilnych kluczy wyników oraz trwałych nadpisań/usunięć;
sam nowy seed nie rozwiązuje utrzymania wcześniej edytowanych obiektów.

### Współrzędne i granica UI

Dla celu 100 km+ rekomendacja to trwałe pozycje globalne o wystarczającej
precyzji (np. double) oraz jawne przejście do lokalnych danych float w rendererze
i podsystemach. Nie zapisywać globalnej tożsamości miejsca wyłącznie w macierzy
float względem chwilowego początku świata. Trzeba doprecyzować przeliczenia
rodzic/dziecko, transformacje fizyki, kamery i historię temporalną; sam typ double
w jednym komponencie nie zamyka dużego świata. To propozycja kontraktu, nie
wynik audytu wszystkich ścieżek precyzji ani pomiaru GPU.

UI HTML/CSS pobiera ograniczony model prezentacji i wysyła komendy z adresami
encji/pól, bez bezpośrednich wskaźników do pamięci ECS. Backend HTML/CSS nie
staje się właścicielem transformacji ani komponentów świata. Wybór technologii,
integracja input/focus i kompozycja UI są przyszłym zakresem; nie portujemy
Control do ECS i nie udajemy, że sam HTML/CSS dostarcza gotowy runtime UI.

## Dlaczego istniejące mechanizmy nie zamykają całego zadania

| Kandydat | Co warto wykorzystać | Czego nie zastępuje |
| --- | --- | --- |
| Flecs | Encje, komponenty, relacje, zapytania, prefaby, systemy i Meta | Semantyka świata, edytora, trwałej tożsamości i streamingu |
| SceneTree/Node3D | Lista obecnych zachowań do odtworzenia; wewnętrzny UI edytora | Docelowy magazyn świata; wrapper Node na encję jest wykluczony |
| PackedScene | Źródło obecnych wymagań prefabów i owner; ewentualny wejściowy format konwersji po decyzji właściciela | Natywny zapis encji przez niezmienione `instantiate()` |
| RenderingServer/RID | Niskopoziomowe zasoby i instancje renderera | Tożsamość edycyjna encji, scheduling ECS i bezpieczne publikowanie zmian |
| ResourceLoader/ResourceUID/PCK | Współdzielone assety, identyfikacja plików, odczyt i pakowanie | Trwałe identyfikatory elementów świata i rozwiązywanie relacji niezaładowanych encji |
| Aktualna microgeometry | Automatyczna reprezentacja geometrii, strony, współdzielenie instancji | Model sceny, tekstury, symulacja, zapis i edycja świata |

## Kolejność zależności do przyszłego planu

To kolejność architektoniczna, nie plan autoryzujący zmiany ani obietnica,
że wszystkie kontrakty wykonawcze zostały już zamknięte.

1. Domknąć granicę sceny, zgodności i zachowań skryptowych; ustalić trwałą
   tożsamość, schemat komponentów i semantykę relacji w Flecs.
2. Wprowadzić natywny runtime sceny, transformacje, jawne fazy oraz własność
   zasobów serwerów; utworzenie i usunięcie świata bez drzewa Node.
3. Doprowadzić ten sam model przez zapis/prefaby oraz edycję: outliner,
   Inspector, picking, gizma, komendy i undo. Pierwszy działający przekrój to
   encja z meshem, edycja, zapis/otwarcie i uruchomienie jako encje.
4. Zamknąć pozostałe kategorie sceny: światła, kamery, fizykę, animację,
   zachowania, audio, nawigację i pozostałe funkcje wskazane w macierzy zakresu.
   Pierwszy statyczny przekrój nie jest końcem wymiany pełnego modelu sceny.
5. Domknąć import/eksport, bootstrap i debugowanie sceny encji, usuwając
   zastępowany świat Node w ramach tej samej zatwierdzonej przebudowy.
6. Dopiero na tym fundamencie wdrażać przestrzenny streaming świata i jego
   automatyczne budżety. Od początku fundament musi umożliwiać unload/reload.

Przed planem wykonania każdy etap wymaga dokładnych kontraktów, plików,
rejestracji, odpowiedzialności wątków i granic zastąpienia. Nie zostawiać
docelowo dwóch scen świata ani ukrytego fallbacku przez Node. Zachowane UI
edytora i zasoby nie są takim fallbackiem.

## Otwarte decyzje wpływające na zakres

- Sceny 2D i UI gry: **rozstrzygnięte podczas researchu**. Brak wsparcia scen
  2D; UI gry osobno w HTML/CSS. Nie wliczać ich portowania do ECS.
- Zerwanie zgodności ze scenami `.tscn`, skryptami Node i pluginami czy
  jednorazowa konwersja? Pytanie zadane; nie projektować stałego mostu. Obecne
  zasady repo zachowują stabilne API upstream, więc ich docelowa wymiana wymaga
  jawnego doprecyzowania przez właściciela.
- Docelowy sposób pisania zachowań: natywne systemy C++, skrypty operujące na
  uchwytach encji czy przebudowany model instancji języka? Zakaz Objecta na
  encję wyklucza bezrefleksyjne zachowanie obecnego attach-script.
- Kontrakt prefabów i referencji przy unloadzie oraz precyzja transformacji
  globalnych. Kierunek Box3D jest zapisany wcześniej; jego integracja nie jest
  już wykonana przez wybranie biblioteki ECS.

Te decyzje nie blokują przedstawionego researchu, ale blokują kompletny,
jednoznaczny plan implementacji całego modelu sceny.

Przed wykonaniem pozostają także konkretne specyfikacje: field-level overrides
i zmiany prefabów przy reimporcie, edycja celu aktualnie niezaładowanego,
osobne światy edycji/gry oraz stosowanie zmian runtime do dokumentu. Są to
własne dane encji, nie synchronizacja świata Node z ECS.

## Metoda i granice dowodów

Trzy niezależne konteksty read-only zbadały runtime/podsystemy, edytor oraz
upstream Flecs (Astra high, dobrany do architektury wielu podsystemów).
Synteza obejmuje źródła raportowane przez te konteksty bez ponownego
dublowania całej nawigacji. Formalny przegląd tego dokumentu jest osobnym
krokiem; zakończenie researchu podobszaru nie jest takim przeglądem.

Wykorzystano wcześniejszy
[research ECS/editor](2026-09-07-1312-native-ecs-world-editor-summary.md) jako
mapę źródeł, a nie dowód bieżącego zachowania. Nawigacja C++: `clang-nav` z
repozytoryjnym `compile_commands.json`, `find`/`docsym`, hover/refs/definition,
następnie odczyty znalezionych deklaracji i implementacji oraz XML i bindingów.
Jednorazowe refs PackedScene zwróciły tylko wyniki lokalne; nie są pełnym
spisem konsumentów. Brakujący skok z nagłówka uzupełniono nawigacją w jednostce
implementacji. Ukierunkowane wyszukiwanie tekstu służyło kotwicom i powierzchniom
nie-C++; wyniki nie są pełnym semantycznym grafem repozytorium.

`SceneState`/`PackedScene` i ResourceUID nie zmieniły się względem poprzedniego
baseline `675d3cbe7f`; Main::start odczytano ponownie, ponieważ plik się zmienił.
Kontrakty zapisane tutaj nie były modyfikowane w dirty working tree podczas
sprawdzenia tych ścieżek. Równoległych zmian renderera nie przypisujemy temu
researchowi. Nie ustalono wydajności Flecs w tym forku, maksymalnej liczby encji,
budżetu pamięci, czasu implementacji ani gotowości świata 100 km+.
