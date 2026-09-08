# Docelowy kierunek renderera i TODO

Zakres następnego planu doprecyzowany 2026-09-08 przy `4c0f7f97b5`:
import mesha → meshlety i automatyczny DAG → scena GPU, raster/RT selection,
stronicowanie i debug view rzeczywiście wybranych klastrów/trójkątów.
DAG obejmuje geometrię bez deformacji, także ruch rigid i instancing; obecne
deformowane meshe zachowują działanie. Foliage i wokselizacja są poza tym planem,
z pozostawieniem prostych granic danych do przyszłego rozszerzenia. Nowe odbicia
nie są wymaganiem. Właściciel wymaga prostego sterowania uproszczeniem RT:
tolerancja błędu i mnożnik poza ekranem, bez analizy wpływu świateł, receiverów
czy sond. [Research pod plan](../research/2026-09-08-2005-gpu-microgeometry-research-summary.md)
opisuje kod, rekomendacje i otwarte kontrakty. Źródło zakresu: bieżąca rozmowa;
dowody techniczne: analiza źródeł i dokumentacji, bez builda/pomiaru GPU.

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
