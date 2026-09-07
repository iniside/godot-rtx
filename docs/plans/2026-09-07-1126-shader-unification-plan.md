# Ujednolicenie shaderów i wspólnego cieniowania przed DDGI

Status: zatwierdzony do implementacji 2026-09-07; niezależny hostile review PASS.
Baseline: G:/Projects/godot-rtx, `a89cd8a3b064427e29a89af0f0b6823bfac21da7`.
Właściciel zatwierdził wykonanie poleceniem „no to zaczynaj”. Implementacja etapowa.
Przegląd obejmował wersję o SHA-256
`7A9E7BA2B27BDDC248A0D0C2032BF76F6A23E991253CFF0550D2CBEE39F99596`;
po przeglądzie uzupełniono wyłącznie status i zapis zatwierdzenia.

## Kontekst i decyzje do zatwierdzenia

Właściciel chce ograniczyć narastanie adapterów GLSL/HLSL i różnych interpretacji
materiałów, światła oraz widoczności przed DDGI. Wynik: jeden toolchain Slang ->
SPIR-V dla migrowanego rdzenia geometrii/surface/RTXDI oraz jedna biblioteka jego
cieniowania. RAB i NRD pozostają cienkimi granicami SDK. Nie powstaje drugi renderer.

Proponowany wybór: Slang 2026.13.1, przypięty pakiet i suma kontrolna,
bezpośrednia generacja SPIR-V dla Vulkan 1.3 / SPIR-V 1.6. Zainstalowany wariant
`2026.13.1-1-g84792eb15` jest kandydatem sprawdzonym diagnostyką; implementacja
przypina dokładny dystrybuowalny artefakt tej wersji, nie zależy od PATH/VulkanSDK.
Nie zakładamy kompilacji przez pośrednie GLSL ani automatycznej próby drugiego kompilatora.

Zakres końcowy tego planu:

- Zachować `.gdshader`, ShaderMaterial, StandardMaterial3D i VisualShader jako
  istniejące publiczne frontend/API. Przestrzenne shadery otrzymują natywny emitter
  Slang z istniejącego drzewa składniowego. Nie dodawać nowego publicznego języka,
  Resource, ustawienia wyboru kompilatora ani migracji projektów.
- Migrować aktywny rdzeń Forward+ geometry/surface, DI, przygotowanie NRD i kompozycję
  HDR oraz ich wymagane include'y. Włącza to istniejące warianty spatial używane
  przez głębię/cienie i edytor 3D, global/instance uniforms oraz deformację.
- Canvas/2D/UI, sky, fog, particles, pozostałe postprocess i niezależne upstream
  renderery zachowują ich odrębne GLSL shadery. To jawna granica pierwszej migracji;
  plan nie obiecuje usunięcia GLSL z całego silnika. Wspólne shadery wykorzystane przez
  oba zbiory wymagają zachowania odrębnej odpowiedzialności bez dwóch implementacji
  migrowanego cieniowania. Fog/HDR połączenie ma zachować dotychczasową energię.
- Nie dodawać DDGI, ReGIR, odbić, przezroczystości, transmisji, nowej obsługi custom
  shaderów w RT, GPU-driven sceny, DAG-u ani streamingu. Zachować aktualne ograniczenia
  i diagnostykę materiałów; pełna ewaluacja trafienia sondy jest późniejszym etapem DDGI.
- Nie dodawać ścieżki bez RT ani obsługi RT D3D12/Metal. Zachować upstream publiczne
  API kompilacji GLSL dla jego użytkowników. Współdzielone zmiany nie mogą zepsuć
  kompilacji pozostałych backendów; Slang wewnętrznego rdzenia jest targetem Vulkan.
- Zastępowane fork-local shadery, adaptery językowe i martwe zależności PT usuwać w
  kroku zastąpienia. Brak przełącznika stare/nowe lub powrotu do starego efektu.
- Bieżące cudze zmiany zostają. Bez worktree, stash, czyszczenia cache i testów
  automatycznych. Nie ma wymagania zielonej kompilacji między zależnymi krokami;
  finalny edytor i template muszą renderować na prawdziwym Vulkanie.

## Istniejące odpowiedzialności i overlap

Ścieżki względem repozytorium:

| Miejsce | Obecna odpowiedzialność | Decyzja |
|---|---|---|
| `servers/rendering/shader_compiler.{h,cpp}`: `compile`, `GeneratedCode` | Parser/emitter GLSL, metadata uniformów i tekstur | Rozszerzyć istniejący emitter o target Slang; wspólny frontend, brak drugiego parsera |
| `modules/glslang/register_types.cpp`: `compile_glslang_shader`; `rendering_device.cpp`: `shader_compile_spirv_from_source` | Publiczna kompilacja GLSL | Zachować dla nieprzenoszonych konsumentów; nowy wewnętrzny kompilator Slang |
| `shader_rd.{h,cpp}`: `_build_variant_code`, `_compile_variant`, `compile_stages` | Źródła, warianty, workery, cache | Rozszerzyć istniejący request o target/entrypoint/opcje/wersję; nie tworzyć równoległego cache |
| `glsl_builders.py`: `build_rd_header`; `SConstruct`, shader `SCsub` | Embedding i zależności include | Wspólna infrastruktura embeddingu z informacją o języku, bez translacji tokenami |
| `editor/export/shader_baker_export_plugin.cpp`: `WorkItem`, wywołania `compile_stages` | Prebake shaderów eksportu | Ten sam pełny request i identity co runtime |
| `rendering_shader_container.cpp`: refleksja; Vulkan container | SPIRV-Reflect, kompresja, downstream metadata | Zachować pojedynczą refleksję końcowego SPIR-V; bez konkurencyjnych layoutów z refleksji Slanga |
| `forward_clustered/scene_shader_forward_clustered.cpp`: `ShaderData::set_code`, `set_code_rt` | Spatial material generator/diagnostyka | Target Slang; zachować wspólne wyniki analizy i aktualną diagnostykę RT |
| `storage_rd/material_storage.cpp`: `shader_set_code`; canvas `set_code` | Wybór właściciela wg shader type | Wybór targetu per konsument; nie globalnie dla urządzenia |
| `rtxdi_application_bridge_inc.glsl` | BRDF, coverage, geometria, widoczność i dane SDK w jednym pliku | Wynieść semantykę renderera do wspólnej biblioteki; RAB pozostawia callbacki i historię SDK |
| `rtxdi_light_sampling_inc.glsl`, `raytracing_hit_inc.glsl` | Powielone index/UV/CLAS decode | Jedno dekodowanie z argumentami; builtiny/query pozostają w wrapperach etapów |
| `rtxdi_nrd_inc.glsl`, `effects/rtxdi_frame.glsl` | Ręczne odpowiedniki funkcji i pakowania NRD | Użyć przypiętego `NRD.hlsli` przy zachowaniu dokładnego istniejącego formatu i modulacji |
| `render_raytracing.cpp`: `build_light_registry` | Wspólna tożsamość świateł i snapshoty | Zachować, nie tworzyć drugiej tabeli dla przyszłego GI |
| `SceneShaderRaytracing::init`, `RenderRaytracing::initialize` | Nadal inicjalizują dawny bundle PT | Usunąć martwą zależność pipeline po oddzieleniu żywej obsługi materiałów |

Powyższe rendererowe skróty są pod `servers/rendering/renderer_rd`, shaderowe pod
`shaders/raytracing`, jeśli nie podano innej ścieżki. Publiczne kontrakty to również
`scene/resources/shader.cpp`, `doc/classes/Shader.xml`, `RDShaderSource` i
`modules/visual_shader/visual_shader.cpp`. Nie wymagają zmiany języka serializacji.

## Kolejność wykonania

### Krok 1 -> kompilator, request i dystrybucja [independent]

Co: dodać wewnętrzną integrację Slang (nowy `modules/slang/` z konfiguracją SCons i
rejestracją) oraz niezbędne wewnętrzne punkty wywołania w RD. Rozszerzyć istniejące
`shader_rd.{h,cpp}`, `glsl_builders.py`, `SConstruct`, odpowiednie shader `SCsub`
oraz `shader_baker_export_plugin.cpp` o język/target/entrypoint/opcje i tożsamość
kompilatora. Nowy moduł jest potrzebny do runtime kompilacji materiałów i hot reload,
nie jest nowym rendering subsystemem. Nazwy nowych prywatnych typów należą do autora.

Jak: jeden request używany w runtime, bakerze i cache; jednoznaczne entrypointy,
rzeczywiste include dependencies, source locations i diagnostyka. Używać API Slanga
w procesie, nie uruchamiać zewnętrznego procesu dla każdego materiału. Sesje
kompilatora mają jawnego właściciela i model współbieżności zgodny z SDK, są zwalniane
po zakończeniu pracy workerów. Brak współdzielonej mutowalnej sesji bez synchronizacji.
Tożsamość cache obejmuje kompilator/build, target, opcje, defines, include content i
entrypoint. Reuse starego cache ma być wykluczony przez klucz, bez kasowania katalogów.

Downstream pozostaje RD -> SPIRV-Reflect -> container -> Vulkan. Target nowego
wewnętrznego requestu nie zmienia domyślnych targetów starych publicznych konsumentów.
Sprawdzić actual SMOL-V/optimizer/reflection path dla NV cluster, deskryptorów,
push constants, specialization constants, buffer device address i 64-bitowych danych.
ABI zachowuje istniejące offsety CPU/GPU: jawne packing/matrix conventions,
location/binding i mapowanie resource arrays; nie polegać na defaultach kompilatora.

Domknąć dystrybucję zależności w edytorze i template, tools/no-tools, inicjalizację i
błąd braku kompilatora. Przypięte dependency dostarczać przez kontrolowany import/build;
nie edytować ręcznie thirdparty ani generated files. Publiczne GLSL API bez zmian.

Dlaczego teraz: każdy kolejny shader musi przejść tę samą pełną ścieżkę, także eksport.
Usunięcie starego efektu następuje z jego migracją, nie w kroku infrastruktury.

### Krok 2 -> spatial frontend i powierzchnie [independent]

Co: zmienić `shader_compiler.{h,cpp}`, spatial `ShaderData::set_code`/`set_code_rt`,
`scene_forward_clustered.glsl` oraz jego material/geometry include closure na target
Slang; zsynchronizować nagłówki i `SCsub`. Zastąpić źródła migrowanego efektu, nie
utrzymywać wyboru GLSL/Slang dla spatial Forward+. Pozostałe typy shaderów wybierają
dotychczasowy target przez swoich właścicieli.

Jak: prawdziwy emitter AST dla typów, konstruktorów, operatorów, texture/sampler,
varyingów i stage IO; bez makr udających GLSL. Zachować nazwy/hinty/offsety uniformów,
global/instance uniforms, tekstury/samplery, usage flags, depth/reverse-Z, motion i
camera-relative/double. Warianty depth/shadow/material export, vertex deformation,
skinning i editor 3D muszą używać tego samego wyniku generacji. `.gdshader` i
VisualShader zachowują dotychczasowe źródło i publiczne properties. Nie rozszerzać
nieobsługiwanych dziś cech RT; zachować ich diagnostykę.

Common surface oddziela pozycję/normalne/material/layers od screen-space historii
RTXDI. Raster wykonuje własną interpolację, derivatives i decals, eksportując dane
według jednego kontraktu. Nie przenosić screen-space instrukcji do ray-hit shaderów.
Shader/material hot reload musi odświeżać prawidłowe warianty i zależne materiały.

Dlaczego teraz: wspólne shading i RAB muszą otrzymać jednoznaczny kontrakt powierzchni.

### Krok 3 -> wspólne cieniowanie i zastąpienie DI [independent]

Co: zastąpić `rtxdi_di.glsl`, `rtxdi_application_bridge_inc.glsl`,
`rtxdi_light_sampling_inc.glsl`, `rtxdi_light_data_inc.glsl` i wymagane
`brdf_inc.glsl`/geometry include'y biblioteką Slang; zmienić `render_rtxdi.{h,cpp}`
i odpowiadające `SCsub`/include consumers. Usunąć stare fork-local odpowiedniki
zastępowanych funkcji, makra językowe i duplikaty flag geometrii.

Jak: wspólne material conversion, BRDF evaluation/sampling/PDF, światła analityczne,
area/emissive/environment, index/UV/CLAS decode i visibility/coverage. Jawny hit input
zawiera geometry/primitive/cluster/barycentrics/transform, bez builtinów konkretnego
etapu wewnątrz decode. Jawny footprint wejściowy pozwala zachować obecne zachowanie
kamery; przyszły DDGI dostarczy własny footprint, nie odziedziczy kamery przez global.
Nie tworzyć nieużywanego pełnego evaluatora trafień DDGI ani nowych reguł materiałów.

Zachować receiver/caster masks, shadows-only, single-sided facing, wszystkie obecne
samplery, vertex alpha, scissor/hash, normal orientation, emisję/solid-angle PDFs,
previous geometry/deformation i tożsamości świateł. Przy złączeniu decode zachować
zdefiniowane sprawdzanie zakresu/remapów. RAB przechowuje tylko wymagane callbacki,
sampling/reservoir/history metadata, a funkcje rendererowe działają bez RAB_Surface.
Istniejący host light registry, TLAS i snapshoty pozostają jedynym właścicielem.

Dlaczego teraz: NRD ma otrzymać to samo cieniowanie bez ręcznych kopii matematyki.

### Krok 4 -> NRD, HDR i usunięcie zależności PT [independent]

Co: zastąpić `rtxdi_nrd_inc.glsl` oraz `effects/rtxdi_frame.glsl`, zmienić
`effects/nrd_effect.{h,cpp}`, `forward_clustered/render_raytracing.{h,cpp}`,
`scene_shader_raytracing.{h,cpp}` i ich build/include consumers. Wspólne include'y
NRD pochodzą z przypiętego SDK. Nie przepisywać wewnętrznych algorytmów denoisera.

Jak: przygotowanie normal/roughness, material factors oraz RELAX signal packing
korzystają z SDK ze zgodnymi NRDConfig i formatami hosta. Zachować demodulację i
remodulację raz, raw radiance/emisję, exposure raz, guide-ordering przed PRE_OPAQUE,
fog/sky energy, historie i reset. Istniejące osadzone wewnętrzne shadery NRD zachowują
swój kontrolowany upstream proces generacji; nie przedstawiać ich jako własnego
drugiego implementowanego efektu. Runtime odbiera je przez ten sam SPIR-V contract.

Usunąć inicjalizację/budowanie dawnych raygen/hit pipeline bundles i ich martwe
templates/callers po zachowaniu potrzebnej material extraction/upload/classification
w aktualnym właścicielu. Domknąć wszystkie live refs, generated shader registration,
cache/pipeline/free paths. Nie portować martwego PT dla samego utrzymania startu.

Konkretne live sprzężenie: `build_tlas` wywołuje `ensure_pipeline_bundle`, a gałęzie
procedural/MultiMesh/mesh uzależniają obecność geometrii od `is_hg_ready_in_bundle`.
Zastąpić te bramki klasyfikacją obsługiwanej geometrii/materiałów; nie wystarczy usunąć
`init`. Zastąpić użycia `register_custom_shader`/`get_custom_shader_entry` w
`process_material`/odświeżeniu cache/TLAS istniejącą metadokumentacją materiału
(uniform offsets, texture hints, opacity) oddzieloną od SBT slots. Źródłowym seamem
są `_preprocess_shader` i `_finalize_uniforms_with_textures` w SceneShaderRaytracing;
analiza ma mieć jednego właściciela z kroku 2, bez drugiego generatora materiałów.
Usunąć `prepare_frame -> drain_completed_compiles`, końcowe
`build_tlas -> finalize_custom_shaders`, pipeline/SBT getters, bundle/worker/readiness
machinery i `rt_sbt_offset` jako indeks hit-group materiału. Argument wymagany przez
upstream API budowy AS nie upoważnia do zachowania kompilacji SBT. Procedural branch
zachowuje obecny zakres compute RT; brak nowego intersection execution.

RID zasobów i viewportów pozostaje w obecnych właścicielach; resize, partial failure,
shader reload i teardown respektują deferred frees i render-thread ownership.

Dlaczego teraz: zamyka ostatnie adaptacje SDK i ukrytą zależność od starego toolchainu
w migrowanym rdzeniu. Nie pozostawia funkcjonującej starej ścieżki PT.

### Krok 5 -> walidacja końcowa i dokumentacja [independent]

Build edytora i template_debug na Windows Vulkan (`accesskit=no d3d12=no` według
obecnej konfiguracji). Oddzielny `precision=double` build/uruchomienie dla dotkniętego
kontraktu. Sprawdzić tools/no-tools i kompilowalność dotkniętych wspólnych backend
interfejsów; nie twierdzić, że RT działa na D3D12/Metal. Sprawdzić dystrybucję runtime
zależności poza katalogiem VulkanSDK. Zweryfikować shader bake/export i uruchomienie
eksportowanego projektu, również cache warm/cold identity bez kasowania cudzych cache.

Na aktualnie zbudowanym zwykłym edytorze i template, real RTX 4090/Vulkan: istniejące
sceny `demos/rtxdi_manual` (surface, single-sided shadows, emissive/analytic/sky,
CLAS i zwykła geometria, animacja, camera cuts, resize, multi viewport, fog i FSR2).
Ręcznie sprawdzić spatial ShaderMaterial/VisualShader, global/instance uniform,
texture/sampler, vertex deformation oraz zmianę źródła/include i natychmiastowy
hot reload. Zweryfikować edytor 2D/UI i zachowane sky/fog/particle/post consumers.
Zachować istniejącą diagnostykę unsupported materials. Bez nowych automatycznych testów.

Odróżnić shader compilation/spirv-val od wykonania przez RD. Zapisać dokładny SHA,
binary provenance, konfigurację, obrazy, błędy i granice dowodu; porównać kontrolowane
sceny przed/po. Nie obiecywać zysku FPS; celem jest spójność i utrzymanie działania.
Zaktualizować canonical research/project-state/rendering-direction. Każdy ukończony
krok powyżej progu ma własny scoped commit i świeży hostile review; końcowe artefakty
wykonywalnej weryfikacji także proof audit. Komentarze: default NONE.

## Dowody i pozostałe ograniczenia

Źródłowa mapa pochodzi z dwóch niezależnych odczytów clangd/declaration/implementation,
ClassDB/XML/SCsub oraz historii. Jednorazowe refs mają ograniczone pokrycie; implementer
zamyka usunięcia przez rzeczywiste consumers i build, nie sam grep.

2026-09-07: lokalny Slang skompilował candidate/committed CLAS RayQuery; SPIR-V ma
`SPV_NV_cluster_acceleration_structure` i oba `OpRayQueryGetIntersectionClusterIdNV`.
`spirv-val --target-env vulkan1.3` PASS. Zainstalowany DXC 1.9 a107ba613 odrzuca te
intrinsics przy cs_6_5 (wymaga 6.10), a cs_6_10 odrzuca jako invalid module. To wynik
tej instalacji, nie dowód braku wsparcia we wszystkich wydaniach DXC.

RTXDI Reservoir/RandomSampler + NRD material factors/RELAX packing przeszły kompilację
i spirv-val w obu kompilatorach. Przypięty RTXGI-DDGI f33e496 shader ProbeBlendingCS
(radiance, 128 rays, 8x8 texels, shared memory) przeszedł Slang i spirv-val. SDK wymaga
przekazania HLSL=1 oraz __spirv__=1; bez drugiej definicji wybiera D3D register path.
Poprawny wariant ma osobne bindingi 0,1,2,4,5 i push constant. Pozostały ostrzeżenia
implicit conversion w źródle SDK. Nie zmieniano SDK. To diagnostyki kompilacji w TEMP,
nie integracja DDGI ani runtime proof pełnego rendererowego include closure.

Lokalne pliki diagnostyczne: `%TEMP%/godot-shader-unification-20260907`.
Pełne warianty i RD/reflection/optimizer/runtime są obowiązkiem kroków powyżej.
Brak pełnej kompilacji silnika lub nowych pomiarów GPU w researchu.
