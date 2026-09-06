# Integracja RTXGI-DDGI — research

Stan sprawdzony 2026-09-06. Godot: `33b555c7f225afd1e586bf749f2b485c1de260df`.
SDK NVIDIA: `f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6`, wersja **1.3.6** według
[CMakeLists.txt SDK](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/rtxgi-sdk/CMakeLists.txt).

To raport integracyjny, nie zatwierdzony plan implementacji. Właściciel wybrał
research DDGI przed RTXDI i polecił korzystać z przykładu NVIDIA. Przecieki są
znanym ograniczeniem do oceny, nie wymaganiem ich całkowitego wyeliminowania.
Docelowe decyzje pozostają w [rendering-direction.md](../reference/rendering-direction.md).

## Wniosek

Integracja jest wykonalna architektonicznie. Największa praca dotyczy udostępnienia
istniejącej sceny RT rendererowi rastrowemu, zasięgu geometrii dla sond i poprawnego
wpięcia do RenderingDevice. Nie trzeba wymyślać algorytmu DDGI ani budować drugiego
systemu materiałów i geometrii.

Rekomendacja do przyszłego planu: zachować obliczeniowe shadery SDK jako źródło,
kompilować HLSL do SPIR-V przez DXC i wykonywać przez RD. Dostosować funkcje
odczytu irradiancji oraz shader promieni sond do GLSL i materiałów tego forka.
To rekomendacja z analizy źródeł; kompilacja, refleksja zasobów i zgodność layoutów
nie zostały jeszcze sprawdzone przez działającą integrację.

## Co daje SDK i przykład

[Integration.md](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/docs/Integration.md)
przypisuje aplikacji scenę RT, TLAS, SBT i shadery trafień. SDK dostarcza obsługę
sond i ich odczytu. Konkretna kolejność pochodzi z `Execute()` w
[DDGI_VK.cpp](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/samples/test-harness/src/graphics/DDGI_VK.cpp#L1655):

1. Aktualizacja i upload parametrów wybranych wolumenów.
2. Promienie sond zapisujące radiancję i odległość trafienia.
3. Osobne aktualizacje irradiancji i odległości sond.
4. Relokacja i klasyfikacja, opcjonalnie pomiar zmienności.
5. Odczyt GI przy cieniowaniu widocznych powierzchni.

[ProbeTraceRGS.hlsl](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/samples/test-harness/shaders/ddgi/ProbeTraceRGS.hlsl)
pokazuje osobną obsługę miss, backface i frontface. Na trafieniu oblicza światło
bezpośrednie i pobiera wcześniejszą irradiancję sond; dalsze odbicia narastają
między aktualizacjami. Stałe promienie relokacji/klasyfikacji nie zasilają blendu
oświetlenia. To wzorzec do adaptacji, nie kamera renderowana z pozycji sondy.

[DDGIVolume.md](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/docs/DDGIVolume.md)
opisuje atlasy ray data, irradiancji, odległości, danych sond oraz zmienności.
Relokacja i klasyfikacja są gotowe w SDK. Jest też scrolling wolumenu:
przesunięcie odnawia skrajne płaszczyzny sond, zachowując wnętrze. Planowanie
aktualizacji z ograniczonym budżetem lub asynchronicznie pozostaje zadaniem silnika.

## Granica kopiowania przykładu

Przejmujemy matematykę, indeksowanie i pakowanie sond, octahedral maps, blend,
relokację, klasyfikację oraz kolejność zależności. Nie przejmujemy modelu sceny
i materiałów test harnessu.

Istotne szczegóły sprawdzone w kodzie NVIDIA:

- `ProbeTraceRGS.hlsl` ogranicza końcową radiancję przez `saturate`. Nie należy
  przenosić tego bez decyzji o HDR i ekspozycji Godota; końcowy wzór nie dodaje
  też osobnego składnika emisji powierzchni.
- [Lighting.hlsl](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/samples/test-harness/shaders/include/Lighting.hlsl)
  używa pętli po światłach. W `EvaluatePointLight` indeks nie zawiera
  `lightIndex`. To konkretny powód, aby wykorzystać własną obsługę świateł,
  zamiast uznawać oświetlenie przykładu za gotowe dla scen miejskich.
- [IndirectCS.hlsl](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/samples/test-harness/shaders/IndirectCS.hlsl)
  używa G-bufferów test harnessu i mnoży wynik przez albedo/PI. Godot ma własne
  miejsce zastosowania albedo; skopiowanie końcowej kompozycji może policzyć je dwa razy.

## Wpięcie w Godota

| Obszar | Sprawdzona istniejąca odpowiedzialność | Wymagana zmiana do rozpatrzenia w planie |
|---|---|---|
| Scena RT | [RenderRaytracing::build_tlas](../../servers/rendering/renderer_rd/forward_clustered/render_raytracing.cpp) i `gather_lights`; [RTViewportState](../../servers/rendering/renderer_rd/forward_clustered/render_raytracing.h) | Udostępnić obecne zasoby i ich lifecycle dla DDGI przy rasteryzacji; zachować zgodność TLAS z buforami materiałów i instancji dla danego viewportu. |
| Aktywacja | [RenderForwardClusteredPT::_setup_rt](../../servers/rendering/renderer_rd/forward_clustered/render_forward_clustered_pt.cpp) | Uniezależnić potrzebę sceny RT od wyboru pełnego PT. |
| Culling | [renderer_scene_cull.cpp](../../servers/rendering/renderer_scene_cull.cpp), `cull.rt_enabled`, `cull.rt_aabb`, `rt_geometry_instances` | Zbieranie RT jest włączane przez `environment_get_pathtracing_enabled`; zakres to AABB kamery oparte o far plane. Zakres sond i ich promieni wymaga własnego uzasadnienia pokrycia. |
| Kolejność klatki | [RenderForwardClustered::_pre_opaque_render](../../servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp) | Zapewnić aktualizację sond przed ich konsumpcją przez opaque shading, z zależnościami RT → compute → raster. |
| Zasoby GI | [GI::RenderBuffersGI i GI::process_gi](../../servers/rendering/renderer_rd/environment/gi.cpp) | Wzorzec własności zasobów RD, przebiegów compute i sprzątania; wolumeny świata oraz ich współdzielenie między viewportami pozostają decyzją projektu. |
| Odczyt diffuse GI | [scene_forward_clustered.glsl](../../servers/rendering/renderer_rd/shaders/forward_clustered/scene_forward_clustered.glsl), wybór GI i finalizacja `ambient_light` | Wpiąć DDGI do istniejącego wyboru źródła GI; ustalić kontrakt irradiancja/PI i zastosować albedo raz. |

Obecny culling RT pomija także `FLAG_CAST_SHADOWS_ONLY` i stosuje visibility ranges
oraz reguły visibility-parent. Nie wolno przyjąć, że jest kompletną sceną dla GI.
Brak przeszkody w TLAS tworzy błędne oświetlenie niezależnie od jakości sond.
Pokrycie musi uwzględniać również promienie do świateł, nie tylko objętość wolumenu.

Lightmapy mają już własny priorytet; istnieją ścieżki SDFGI i VoxelGI oraz dalsze
modyfikacje ambientu przez reflection probes/SSIL. Należy ustalić pojedynczego
właściciela diffuse indirect w trybie DDGI, zachowując wymagane kontrakty upstream.
To nie jest zgoda na globalne usunięcie pozostałych API GI.

## HLSL, Vulkan i RenderingDevice

[RenderingDevice::shader_compile_spirv_from_source](../../servers/rendering/rendering_device.cpp)
obsługuje tutaj GLSL; istnienie enuma HLSL nie oznacza działającego kompilatora.
`shader_create_from_spirv()` daje punkt wejścia dla wcześniej skompilowanych shaderów.
[DDGI.cpp przykładu](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/samples/test-harness/src/graphics/DDGI.cpp)
pokazuje kompilację DXC z `-spirv` i targetem Vulkan 1.2.

Rekomendowany wariant wymaga więc określenia integracji DXC z buildem, wariantów
shaderów, refleksji i mapowania bindingów. Query z
[Irradiance.hlsl](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/rtxgi-sdk/shaders/ddgi/Irradiance.hlsl)
trzeba dostosować do GLSL oraz ustalić wspólny układ parametrów CPU/HLSL/GLSL.
W tym shaderze widoczność jest aproksymowana momentami odległości i ważeniem
ośmiu sąsiednich sond; nie jest dokładnym testem widoczności każdego odczytu.

Alternatywa istnieje: unmanaged host SDK przez callback RD, analogicznie do
[DLSSEffect::_upscale_internal_graph_callback](../../servers/rendering/renderer_rd/effects/dlss.cpp).
Nie jest jednak bezkosztowym skrótem. [DDGIVolume_VK.cpp SDK](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/rtxgi-sdk/src/ddgi/gfx/DDGIVolume_VK.cpp)
sam zapisuje dispatch i bariery. Unmanaged ustala własność zasobów, ale nie
rozwiązuje automatycznie śledzenia ich użycia, layoutów i synchronizacji przez RD.
Nie wybrano jeszcze ani nie wdrożono żadnej z tych integracji.

## Ograniczenia i sensowny pierwszy zakres

- Przecieki: odległości sond, bias i relokacja ograniczają problem, nie usuwają go.
  Dokumentacja NVIDIA ostrzega przed ścianami bez grubości lub cienkimi względem
  rozstawu sond. To znane ograniczenie do oceny wizualnej, nie obietnica naprawy.
- Foliage: backface zamkniętej ściany i druga strona cienkiego liścia nie mogą
  bez analizy mieć tej samej semantyki dla klasyfikacji. Transmisja liści oraz
  przyszłe trafienia wokselowe wymagają odrębnego zbadania.
- Budżet: przykładowe `16^3` sond × 128 promieni daje 524 288 promieni aktualizacji,
  jeszcze bez promieni cieni. To rachunek pracy, nie zmierzony czas GPU ani preset.
- Pierwszy proponowany zakres: jeden stały wolumen, istniejące materiały i światła,
  rasteryzacja z diffuse DDGI, wizualizacja sond/ray data/odległości. Bez uzależniania
  tego etapu od RTXDI, ukończenia DAG-u czy wokselowego foliage.
- Do planu pozostają: publiczna reprezentacja wolumenu i własność world/viewport,
  zakres cullingu, układ współrzędnych i normalnych, HDR/emisja, wariant integracji
  shaderów, polityka GI oraz budżety aktualizacji. Nie ustalono FPS ani minimalnego GPU.

## Metoda i granice weryfikacji

Odczytano przypięte źródła SDK i przykładu Vulkan, nie tylko README. Pliki pobrano
do tymczasowego cache poza repozytorium; nie dodano SDK jako zależności projektu.
Lokalne punkty wejścia pochodzą z utrzymywanej mapy i ukierunkowanych odczytów
źródeł. Sprawdzenie scoped diff od `ca35348c` nie wykazało zmian w zbadanych
lokalnych punktach integracji; `compile_commands.json` nie był dostępny.
Wyszukiwania tekstowe były ograniczone do konkretnych plików/katalogów i mają
charakter dolnego oszacowania pokrycia. Nie uruchamiano przykładu, kompilacji,
edytora, testów ani pomiarów GPU. Wykonalność binarna i wydajność pozostają niezweryfikowane.
