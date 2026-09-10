# Docelowy kierunek renderera i TODO

## Model sceny — decyzja właściciela 2026-09-09

Flecs jest docelowym fundamentem całej sceny świata 3D, w edytorze i runtime.
Scena zawiera encje i komponenty, bez Node/Object/Resource wrapperów encji;
hierarchie pozostają relacjami. Gęste populacje mają być domyślnie płaskie,
bez skomplikowanego drzewa dla każdego obiektu; grupowanie w edytorze lub
streamingu nie wymusza rodzica transformacji. Hierarchie tylko tam, gdzie są
potrzebne. Najpierw ten model, potem world streaming.
Cel to automatyczna obsługa gęstych światów 100 km+, zapisanych/proceduralnych,
z trwałą tożsamością niezależną od rezydencji. Sceny 2D nie są wspierane;
UI gry będzie osobną warstwą HTML/CSS. Infrastruktura UI edytora i współdzielone
assety mogą zachować typy Godota. Zastępuje to wcześniejszy otwarty wybór ECS.
[Research źródeł i kontraktów](../research/2026-09-09-2023-flecs-scene-model-replacement-research.md)
przy `999bbd18c` określa granice wymiany. Właściciel zatwierdził implementację
2026-09-10: [plan](../plans/2026-09-10-1023-flecs-native-scene-plan.md),
commit `e4f73a8286`, task-start `3a6a695fc8`. Model skryptów i Node plugins są
poza zakresem; stare sceny nie wymagają kompatybilności, zachowujemy skończony
zestaw scen rendererowych. Plan wyłącza też SoftBody/Generic6DOF i authoring
CSG/GridMap. Zabezpieczenie danych scen `05996de60b` przeszło source review
i niezależny audyt danych. Fundament Flecs `8b645f23a7` z poprawką `4e2db6d6ab`
przeszedł finalny source review oraz ordinary/double/template buildy.
Wymiana cyklu świata `9588beb0d4` z poprawką `7e4513db52` kompiluje się na
trzech osiach; pełny edytor i pusta gra Vulkan kończą się kodem 0. Finalny
source review tego etapu przeszedł. Dokument `c1927f7324` z poprawkami
`22158a5585` i wznowioną przez właściciela `2a7768e58c` przeszedł świeży review,
trzy buildy i start edytora/pustej gry Vulkan. Niecommitowane renderer/konwerter
zapisują i odczytują już natywne energy_directional. Pierwsze klatki Vulkan
ujawniły transpozycję transformacji konwertera i błędne flagi proceduralne;
poprawki dają widoczną szarą płaszczyznę z natywną kamerą i materiałem.
Istniejące liczniki GPU potwierdzają próbkowane wykonanie światła kierunkowego.
Operacje prefab/UI oraz cała integracja renderera pozostają nieukończone. Zobacz
[status i granice dowodów](../research/2026-09-10-1149-flecs-scene-implementation-status.md).
Reakcja na zmianę energii światła, pozostałe przypadki renderowania i wydajność
są niesprawdzone.


Zakres implementacji zatwierdzony 2026-09-09, plan `414a19fdef`:
import mesha → meshlety i automatyczny DAG → scena GPU, raster/RT selection,
stronicowanie i debug view rzeczywiście wybranych klastrów/trójkątów.
DAG obejmuje geometrię bez deformacji, także ruch rigid i instancing; obecne
deformowane meshe zachowują działanie. Foliage i wokselizacja są poza tym planem,
z pozostawieniem prostych granic danych do przyszłego rozszerzenia. Nowe odbicia
nie są wymaganiem. Właściciel wymaga prostego sterowania uproszczeniem RT:
tolerancja błędu i mnożnik poza ekranem, bez analizy wpływu świateł, receiverów
czy sond. [Plan wykonania](../plans/2026-09-09-0900-gpu-microgeometry-plan.md)
wybiera meshoptimizer/clodBuild bez METIS i własnego simplifiera. Importer i
format stron są zapisane w `fd74e8ca4f`; API RD przeszło review przy `57fa1e2e07`.
[Status](../research/2026-09-09-0900-gpu-microgeometry-implementation-status.md)
oddziela skompilowane obiekty od nieukończonej integracji i brakującej walidacji
importu/GPU. Źródło zakresu: bieżąca rozmowa i zatwierdzony plan.

Zakres domknięcia uzgodniony 2026-09-08: DLSS/RR, path tracing i DDGI.
VR/XR nie jest wspierane ani objęte naprawami lub weryfikacją tego zadania.
DDGI nie ma osiągać zgodności obrazu z path tracingiem; porównania służą
ocenie zachowania obu metod i ich ograniczeń, nie wymuszaniu identyczności.

Aktualizacja wykonania 2026-09-08: DDGI podążające za kamerą, wspólny natywny
path tracer, wybór NRD/RR/raw oraz dostarczenie DLSS 4.5 z presetem M są wdrożone.
Końcowe źródła `968d4e9e8e` przeszły świeże review; buildy edytora, double
i template oraz eksport SR/RR zakończyły się poprawnie. Dowody GPU, hot reloadu,
zapisu ustawień i granice weryfikacji opisuje
[status domknięcia](../research/2026-09-08-0935-ddgi-pt-rr-closure-status.md).
Poniższy kierunek architektury pozostaje szerszy od tego wdrożenia.

Ustalenia właściciela z 2026-09-06. Dokument zapisuje przyjęty kierunek i pytania
do dalszego researchu; nie jest planem implementacji ani potwierdzeniem wykonania.

## Cel

Gęste sceny z foliage połączone ze scenami urbanistycznymi. Priorytetem jest
wydajność renderera gry przy ograniczeniach dostępnego sprzętu. Pełny path tracing
nie jest docelową podstawą oświetlenia tych scen.

## Przyjęte decyzje

- Renderer hybrydowy: rasteryzacja widocznych powierzchni, ReSTIR DI dla światła
  bezpośredniego i DDGI dla rozproszonego światła pośredniego.
- Kolejność doprecyzowana 2026-09-06: najpierw RTXDI, potem DDGI. Wspólna
  podstawa światła bezpośredniego ma obsługiwać powierzchnie widoczne z kamery
  i przyszłe trafienia sond DDGI. Research RTXDI zakończono; właściciel zatwierdził
  plan integracji i polecił rozpoczęcie implementacji. DDGI pozostaje późniejszym etapem.
- Doprecyzowanie właściciela 2026-09-06: zgodność wsteczna i obsługa platform
  bez ray tracingu nie są wymaganiami. Można usunąć utrudniające integrację
  stare API i ścieżki bez RT; nie dodawać dla nich fallbacków ani migracji.
- Plan integracji można podzielić na etapy bez działającego builda/renderowania
  pomiędzy nimi. Wymagane jest działające renderowanie na ostatnim etapie;
  wcześniejsze sprawdzenia mają służyć pracy, nie utrzymywaniu starej ścieżki.
- System geometrii wzorowany na Nanite: meshlety/klastry, automatyczne budowanie
  DAG-u uproszczeń, selekcja klastrów i streaming. Mega Geometry/CLAS stanowi
  podstawę reprezentacji klastrowej dla ray tracingu.
- **DAG zostaje; manualne LOD-y odpadają.** Odrzucone są ręcznie przygotowywane
  poziomy LOD całych obiektów. Hierarchia automatycznie uproszczonych klastrów
  pozostaje częścią systemu.
- Foliage ma osobną ścieżkę reprezentacji i obsługi geometrii/animacji.
- Bliskie foliage używa rzeczywistej geometrii liści i gałęzi bez masek alfa
  wycinających ich kontury. Opacity Micromaps nie są założoną podstawą tej ścieżki.
- Od pewnej odległości foliage przechodzi na reprezentację wokselową.
  Próg i sposób przejścia pozostają do zbadania.
- Zabudowa, geometryczne foliage i wokselowe foliage uczestniczą we wspólnym
  oświetleniu, cieniach i GI.

## Stan ujednolicenia shaderów

Weryfikacja 2026-09-07, finalne dowody `c148e8fbb9`: zatwierdzona migracja zastąpiła
spatial/surface, RTXDI i NRD/HDR natywnym Slangiem do SPIR-V. Wspólne cieniowanie,
geometria i światła mają jedną implementację; usunięto dawny kompilator PT,
bundles i bramki gotowości TLAS. Publiczne .gdshader/VisualShader pozostają,
a odrębne 2D/UI, sky/fog/particles i pozostałe efekty zachowują GLSL.
Kroki 1–4 przeszły niezależne przeglądy kodu i dowodów. Edytor, template i double
kompilują się; rzeczywiste obrazy obejmują materiały, hot reload, wiele viewportów
i eksportowaną paczkę poza VulkanSDK. Poprawka awarii inlinera re-spirv podczas
eksportu przeszła przegląd kodu i dowodów. Końcowy krok walidacji również otrzymał
niezależne wyniki PASS; zatwierdzony zakres migracji jest ukończony.
[Status migracji](../research/2026-09-07-1126-shader-unification-status.md) opisuje
dokładny zakres i ograniczenia. Próba przy współrzędnych około 1e8 ujawniła
istniejące ograniczenie precyzji pozycji świateł i transformacji RT typu float;
nie jest to potwierdzenie poprawnego oświetlenia dużego świata. DDGI i przebudowa
współrzędnych pozostają osobnymi pracami; nie ma deklaracji wzrostu wydajności.

## TODO — dalszy research

- [ ] Ustalić zakres istniejącej integracji meshletów/CLAS oraz brakujące elementy
  systemu wzorowanego na Nanite: budowanie DAG-u, selekcję, rezydencję i streaming.
- [x] Zbadać wpięcie ReSTIR DI w Forward Clustered: dane powierzchni, światła
  emisyjne, historia, odszumianie i kompozycja; zatwierdzono plan implementacji.
- [ ] Po RTXDI dopracować integrację DDGI ze wspólnym oświetleniem bezpośrednim.
- [ ] Zbadać reprezentację wokselowego foliage: gęstość/pokrycie, materiał,
  prześwity koron oraz przechodzenie promieni cieni i GI.
- [ ] Ustalić sposób tworzenia wokseli oraz przejścia geometria–woksele,
  uwzględniając stabilność obrazu i zgodność rasteryzacji z widocznością dla RT.
- [ ] Zbadać instancing, wiatr i budżet aktualizacji geometrii oraz struktur RT,
  w tym przydatność partitioned TLAS.
- [ ] Zbadać model cienkich, dwustronnych liści i przepuszczania światła;
  brak masek alfa nie rozstrzyga modelu transmisji materiału.
- [ ] Ustalić rozmieszczenie i budżet aktualizacji sond DDGI dla wnętrz,
  zabudowy i terenów z roślinnością oraz sposób obsługi odbić zwierciadlanych.
- [ ] Określić docelowy zakres GPU, budżet klatki i VRAM przed wyborem parametrów.

Otwarte TODO nie upoważniają do ich implementacji. Integracja RTXDI jest osobno
zatwierdzona w planie; automatyczne testy nie są autoryzowane.
Nie ustalono jeszcze docelowego FPS, minimalnego GPU ani terminów wykonania.

## Powiązane dokumenty

- [Wcześniejszy research Mega Geometry i camera-relative](../research/2026-09-02-1904-megageometry-camera-relative-research.md)
- [Plan pierwszego przekroju CLAS dla statycznej geometrii](../plans/2026-09-02-2042-clas-static-geometry-plan.md)

Powyższe dokumenty opisują wcześniejszy stan i zakres konkretnych prac.
Bieżący kierunek docelowy zapisany jest tutaj; ich historyczne ograniczenia
nie rozszerzają ani nie zastępują powyższych decyzji właściciela.
