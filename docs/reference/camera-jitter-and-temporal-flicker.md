# Jitter kamery a migotanie przy rekonstrukcji czasowej

Ustalenia z sesji 2026-09-03, zmierzone na ścieżce path tracing + DLSS Ray Reconstruction.

## Objaw

Przy ruchu kamery cały obraz pulsował — właściciel opisał to jako stroboskop. Nie było
tego widać w bezruchu.

## Dlaczego długo tego nie widziałem

Pierwsze narzędzia mierzyły **średnie w blokach 8×8 na nieruchomej kamerze**. Ten sam
artefakt dawał tam 0,3% i wyglądał na margines. Zmierzony **per piksel, w trakcie
ciągłego ruchu**, drugą różnicą czasową (która kasuje płynny ruch, a zostawia to, co do
niego nie pasuje), okazał się **pięciokrotnym wybuchem co 8 klatek na wszystkich
krawędziach naraz**.

**Wniosek metodyczny: regime pomiaru trzeba dobrać do objawu.** Trzy hipotezy odpadły na
złym przyrządzie, zanim właściciel zwrócił uwagę, że mierzę nie to co trzeba.

Narzędzia w `gi_demo/`: `motionflicker.tscn` (per piksel, w ruchu — ten właściwy),
`d2map.tscn` (zapisuje mapę błędu jako obraz), `rest.tscn` i `tradeoff.tscn` (bezruch,
znacznie mniej czułe).

## Przyczyna

Wyłączenie jittera całkowicie **likwidowało puls** (szczyt 0.041 → 0.0064), więc źródło
było jednoznaczne. Kontrakt z DLSS sprawdzony i **poprawny**: `add_jitter_offset` dodaje
do `columns[3]`, czyli offset NDC; przy `taa_jitter = wartość / viewport_size` daje to
`wartość/2` piksela, a do Streamline wysyłamy `taa_jitter * internal_size * 0.5`, czyli
tę samą liczbę. Znak i skala się zgadzają.

Problemem była **powtarzalność sekwencji**, nie jej amplituda ani wielkość kroku. Halton
indeksowany `frame % jitter_phase_count` przechodzi w kółko przez ten sam skończony zbiór
przesunięć, więc błąd rekonstrukcji jest spójny klatka po klatce i akumulator czasowy się
na nim zatrzaskuje.

**Uwaga, bo łatwo tu o błędne wyjaśnienie:** kuszące jest tłumaczenie „R2 ma stały krok,
Halton ma jeden duży skok przy zawijaniu". **To nieprawda.** W przestrzeni pikseli krok
R2 przyjmuje cztery różne wartości, najgorsza to 0,946 piksela na ~1 klatkę na 10 —
praktycznie tyle samo co najgorszy krok Haltona (1,036 piksela, 1 na 8). Wielkość kroku
nie odróżnia tych sekwencji. Odróżnia je to, że R2 ma przyrosty niewymierne i **nigdy się
nie powtarza**.

## Co pomogło, zmierzone

Per piksel, w ruchu, DLSS-RR włączone, 1 spp:

| sekwencja jittera | d2_mean | d2_peak | pikseli gwałtownych |
|---|---|---|---|
| Halton, 8 faz | 0.01223 | 0.04078 | 1,10% |
| Halton, 16 faz | 0.01561 | 0.10012 | 0,90% |
| Halton, 32 fazy | 0.01563 | 0.16010 | 0,69% |
| jitter wyłączony | 0.00437 | 0.00639 | 0,06% |
| **R2 addytywne** | **0.00370** | **0.00681** | **0,10%** |

Jakość statyczna niezmieniona: szum po odszumianiu 0.01112 wobec 0.01113, jasność
0.42854 wobec 0.42885.

**Więcej faz Haltona jest gorsze, nie lepsze** — puls staje się rzadszy, ale
czterokrotnie gwałtowniejszy.

## Co NIE pomogło, zmierzone

- **Micro jitter per piksel w raygenie** (odpowiednik `DLSSRRMicroJitter = 0.1` z RTXPT).
  Bez wpływu na cykl, potwierdzone testem z wartością 4.0, która niszczy obraz, a szereg
  pozostawał identyczny.
- **Indeksowanie Haltona od 1** zamiast od 0, mimo że `get_halton_value(0)` zwracało
  zdegenerowane `-1.0` w obu osiach. Bez wpływu.
- **Format bufora normalnych** `R8G8B8A8_SNORM` → `RGBA16F`. Naprawiło realną
  niezgodność z kwalifikatorem `rgba16f` w shaderze (zapisy były niezdefiniowane), ale na
  migotanie nie wpłynęło.
- **Preset Ray Reconstruction** D kontra E. Dwa różne przyrządy dały sprzeczny wynik;
  nie umiem ich uszeregować.

## Co zostaje otwarte

Resztkowy garb o okresie 8, około dwukrotny wobec tła, powtarzalny między przebiegami.
Przyczyna **nieustalona**. Najbardziej oczywisty podejrzany — `taa_frame_count = frame %
jitter_phase_count` — **odpada**: występuje wyłącznie w shaderach rasteryzacji
(`scene_forward_clustered.glsl`, `scene_forward_mobile.glsl`, rotacja próbek miękkich
cieni) i ani razu w shaderach raytracingu.

## Niezmierzony zasięg zmiany

`jitter_phase_count` jest ustawiane dla **każdego** trybu temporalnego
(`renderer_viewport.cpp:275-286`), więc R2 dostają też TAA, FSR2 i MetalFX Temporal.
Zmierzona została wyłącznie ścieżka DLSS + path tracing. Sprawdzone, że integracja FSR2
w tym drzewie nie zakłada Haltona ani skończonej liczby faz — `fsr2.cpp:855-856` bierze
gotowy offset i nic więcej; jedyny ślad `ffxFsr2GetJitterPhaseCount` to komentarz
dokumentujący skopiowany wzór.
