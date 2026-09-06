# Docelowy kierunek renderera i TODO

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
  i przyszłe trafienia sond DDGI. Zlecono research, nie implementację.
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

## TODO — dalszy research

- [ ] Ustalić zakres istniejącej integracji meshletów/CLAS oraz brakujące elementy
  systemu wzorowanego na Nanite: budowanie DAG-u, selekcję, rezydencję i streaming.
- [ ] Zbadać wpięcie ReSTIR DI i DDGI w Forward Clustered: dane powierzchni,
  światła emisyjne, historia, odszumianie i kompozycja bez podwójnego liczenia światła.
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

TODO nie upoważnia do implementacji ani uruchamiania automatycznych testów.
Nie ustalono jeszcze docelowego FPS, minimalnego GPU ani terminów wykonania.

## Powiązane dokumenty

- [Wcześniejszy research Mega Geometry i camera-relative](../research/2026-09-02-1904-megageometry-camera-relative-research.md)
- [Plan pierwszego przekroju CLAS dla statycznej geometrii](../plans/2026-09-02-2042-clas-static-geometry-plan.md)

Powyższe dokumenty opisują wcześniejszy stan i zakres konkretnych prac.
Bieżący kierunek docelowy zapisany jest tutaj; ich historyczne ograniczenia
nie rozszerzają ani nie zastępują powyższych decyzji właściciela.
