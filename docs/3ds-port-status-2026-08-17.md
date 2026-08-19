# Estado del port a 3DS — resumen y siguientes pasos (2026-08-17)

Documento de continuidad: qué se ha hecho, en qué estado real está el
proyecto ahora mismo, y por dónde seguir. Reescrito de arriba a abajo al
final de una sesión larga que avanzó mucho el estado real del port — léelo
entero antes de tocar código, y no asumas nada del historial de versiones
anteriores de este mismo fichero (`git log -- docs/3ds-port-status-2026-08-17.md`
si hace falta arqueología).

## 0. Punto de partida

Objetivo: portar **Metroid Zero Mission** a Nintendo 3DS, siguiendo el
mismo patrón que `zelda-tmc-3ds` (port de Minish Cap a 3DS, que el usuario
también tiene localmente en `~/zelda-tmc-3ds` — hay ficheros de este port
copiados literalmente de ahí, ver sección 4): partir de la **decompilación
matching** del juego (`metroidret/mzm`, no de ingeniería inversa propia) y
construir encima una capa de "port" que emule el hardware GBA necesario
sobre libctru/citro2d/citro3d.

Fork: `tinaut1986/mzm`, rama `wip/3ds-port-no-audio-ppu`.

## 1. Estado actual en una frase (actualizado, 2026-08-18 madrugada)

**El juego es plenamente jugable en 3DS real a 60 FPS: pasa selección de idioma, intro, título, creación y selección de partida, gameplay completo (movimiento, disparo, morfosfera, proyectiles abriendo puertas, bloques destructibles, plataformas desmoronables), estatuas Chozo visibles y sincronizadas, menú de estado y wireframe alineados, animación y guardado persistente en SD (`mzm.sav`), efectos de calor/niebla (`Haze`) sin crasheos y marcadores de objetivo en el mapa Chozo funcionando.**

El audio real sigue desactivado (stubeado, ver 6a) — no es la causa de nada de lo anterior. Rendimiento: 60 FPS estables sin logs pesados. Ver secciones 6e a 6h para el detalle de todo lo resuelto y sección 7 para los siguientes pasos.

## 2. Arquitectura del port — lo que ya estaba antes de hoy

- Decompilación base verificada (~99.89%, build GBA matching byte a byte
  contra la ROM EU real).
- Arquitectura de assets sin copyright: el usuario pone su ROM legal en la
  SD/disco, el port la carga en runtime y traduce direcciones GBA a
  punteros nativos (`port/port_rom.c`, `tools/gen_port_rom_offsets.py`,
  headers "sombreados" en `port/generated/shadow/`).
- BIOS de GBA reimplementada en C limpio (`port/port_bios.c`): `Div`,
  `Sqrt`, `CpuSet`, `LZ77Uncomp*`, `Multiboot` (stub), etc. Documentado con
  GBATek, no copiado.
- Target nativo Linux (`platform/linux/`) además del target 3DS
  (`platform/3ds/`).
- `port/ppu/` (`virtuappu.c`, `mode1.c`): renderizador PPU en software,
  vendido desde el port de Minish Cap — **agnóstico de TMC**, reutilizable
  tal cual.
- `platform/3ds/source/platform_gpu_3ds.c`: capa de presentación
  citro2d/citro3d (subida de textura, doble pantalla), copiada de
  `zelda-tmc-3ds` — también reutilizable tal cual, sin acoplamiento a TMC.
- `platform/3ds/source/port_ppu_3ds.c`: el bridge PPU→GPU **original** de
  TMC, copiado sin adaptar. Profundamente acoplado a subsistemas propios de
  TMC (pantalla inferior, HDMA, widescreen, save states, HUD de
  rendimiento). **No se ha tocado ni se usa** — se dejó como referencia y en
  su lugar se escribió uno nuevo y mínimo, ver sección 4.

## 3. El bucle de depuración local (`platform/linux/`) — la pieza que lo cambió todo

Antes de hoy, cada iteración para depurar un cuelgue en 3DS implicaba:
compilar, generar CIA, instalarlo por FTP en la consola, abrirlo, esperar,
y leer un log por FTP — varios minutos por vuelta. Hoy se arregló
`platform/linux/` para que sea un **repro completo y rápido en local**:

- `port/port_debug_log.c` tenía la ruta de log hardcodeada a
  `sdmc:/3ds/mzm-debug.log` (solo válida en 3DS). Ahora usa
  `/tmp/mzm-debug.log` bajo `#ifdef PLATFORM_LINUX`.
- Nada avanzaba `REG_VCOUNT` en Linux (solo existía un hilo para eso en
  3DS, `port/port_gba_timing.c`, arrancado desde `platform_3ds_minimal.c`).
  Cualquier busy-wait sobre `REG_VCOUNT` (hay varios en el motor, el más
  relevante en `src/audio_wrappers.c:205-206`) se quedaba colgado para
  siempre. Añadido un hilo equivalente con pthreads en
  `platform/linux/source/platform_linux.c`
  (`Platform_Linux_StartTimingThread`).
- Faltaban ficheros en `platform/linux/Makefile` (`port_debug_log.c`,
  luego `port_constructor_init.c`).

Uso: `cd platform/linux && make && ./mzm-linux --rom ../../mzm_eu.gba --test 3000`
corre 3000 frames (~50s simulados) headless y dice si crashea. Con
`CFLAGS=... -fsanitize=address` (ver `make test` o construir a mano, mirar
commits para la línea exacta) da backtraces exactos de cualquier crash de
memoria, sin necesitar la 3DS para nada.

**Antes de perseguir un bug en 3DS real, comprueba primero si se reproduce
en `platform/linux/`.** Es muchísimo más rápido, y varios de los bugs de
hoy (ver sección 5) se depuraron y verificaron ahí antes de tocar hardware.

Nota: no todos los bugs se reproducen en Linux — el bug de doble resolución
de direcciones (sección 5.4) y el que queda por resolver (sección 6) son
específicos de 3DS, porque dependen de dónde `malloc()` decide poner la
memoria, que difiere entre x86_64 y ARM11.

## 4. Renderizado — primera imagen real en pantalla

`platform/3ds/source/port_ppu_mzm.c` (nuevo, ~110 líneas): bridge mínimo
entre `port/ppu` (renderizador software, reutilizado tal cual) y
`platform_gpu_3ds.c` (presentación citro2d/citro3d, reutilizado tal cual).
Deliberadamente **no** es una adaptación de `port_ppu_3ds.c` (ver sección
2) — sin pantalla inferior real, sin HDMA, sin widescreen, sin HUD de
rendimiento. Solo lo justo para ver contenido real en pantalla.

- `Port_PPU_Init()`: ata `gIoMem`/`gVram`/`gBgPltt`/`gObjPltt`/`gOamMem`
  (memoria GBA emulada de `port_gba_mem.h`) al renderer vía
  `virtuappu_mode1_bind_gba_memory()`.
- `Port_PPU_PresentFrame()`: llamado una vez por frame emulado desde
  `port/port_bios.c`'s `Port_Bios_Halt()` (justo después de
  `CallbackCallVblank()`). Lee `DISPCNT` de `gIoMem`, llama
  `virtuappu_mode1_render_frame()`, sube el resultado a la pantalla
  superior vía `PlatformGpu3DS_BeginTop()`/`PlatformGpu3DS_EndBottom()`.
- Overlay de texto "F ###" en pantalla (esquina inferior izquierda,
  reutilizando el HUD de FPS de `platform_gpu_3ds.c` con la etiqueta
  cambiada) mostrando el contador de frames presentados — útil para saber
  cuánto ha avanzado antes de parar la app a mano.

Bug encontrado y arreglado en este mismo bloque: `PlatformGpu3DS_EndBottom`
se llamaba con `changed=false` siempre, así que el buffer negro de la
pantalla inferior nunca se subía a la textura de la GPU de verdad — se veía
la basura que hubiera en esa VRAM desde antes (parecía un patrón diagonal
fijo). Arreglado pasando `changed=true` siempre (no hay contenido real en
la pantalla inferior todavía, así que no importa el coste).

## 5. Bugs reales encontrados y arreglados hoy (todos confirmados, no sospechas)

Cada uno se diagnosticó con evidencia directa (logs, ASan, volcados de
crash de Luma3DS parseados a mano) — no por conjetura. Ver mensajes de
commit para el detalle completo de cada uno.

### 5.1 — Dereferencia cruda de `VRAM_BASE`

`src/soft_reset.c`'s `LanguageSelectChangeHighlight` hacía aritmética de
punteros sobre `VRAM_BASE` (macro con la dirección GBA cruda `0x06000000`)
y la desreferenciaba directamente, saltándose la traducción de
`WRITE_16`/`READ_16`. Segfault inmediato. Arreglado resolviendo con
`gba_MemPtr()` en los dos puntos de uso. **Este patrón (tomar la dirección
de una macro `_BASE` y desreferenciar directo) puede repetirse en otros
sitios del motor que aún no se han ejercitado — si aparece un crash nuevo
con pinta de "dirección inválida" en algo que use `VRAM_BASE`/`EWRAM_BASE`/
`OAM_BASE`/etc., mirar aquí primero.**

### 5.2 — Inicialización de punteros a ROM antes de cargar la ROM

~60 funciones `Init_*` en 29 ficheros de `src/` estaban marcadas
`__attribute__((constructor))`, que el runtime de C ejecuta **antes de
`main()`** — antes de que `Port_LoadRom()` resuelva los punteros a datos de
ROM que esas funciones copian. Capturaban NULL para siempre.

Arreglado centralizando en `port/port_constructor_init.c`: se quitó el
atributo constructor de las 62 funciones, y se llaman todas (como símbolos
`weak`, porque algunas solo existen en ciertas combinaciones de
región/idioma) desde `Port_InitConstructorPointers()`, invocada una vez
justo después de `PortGen_All_Init()` dentro de `Port_LoadRom()`.

### 5.3 — El cuelgue de arranque en 3DS real (parcialmente resuelto — workaround, no fix)

`gspWaitForEvent(0, true)` en `Port_Bios_Halt()` nunca se desbloqueaba en
hardware real. Se descartaron dos hipótesis probándolas en la consola:

1. Colisión de prioridad entre el hilo de `REG_VCOUNT` (`port_gba_timing.c`,
   prioridad 0x18) y el hilo interno de GSP de libctru (`gspEventThreadMain`,
   confirmado a prioridad 0x1A desanclando `gspgpu.o` de `libctru.a`). Se
   bajó la prioridad de nuestro hilo a 0x20 (por debajo del de GSP) — sin
   cambio.
2. Falta de `aptMainLoop()`. Se añadió — sin cambio en el cuelgue, pero
   **se mantiene** porque sin ella la app no respondía nunca a HOME (solo
   se podía salir apagando la consola). Ahora si `aptMainLoop()` devuelve
   `false`, la app sale limpiamente (`gfxExit(); exit(0);`).

**La causa real de por qué `gspWaitForEvent` no se desbloquea sigue sin
saberse.** Workaround actual: `Port_Bios_Halt()` en 3DS usa
`svcSleepThread(16666667LL)` (60Hz por temporizador) en vez de esperar al
VBlank real. Esto es aceptable ahora porque el renderizado tampoco estaba
sincronizado a VBlank real todavía, pero **hay que revisitar esto en cuanto
la sincronía de vídeo/audio importe de verdad** (podría ser la causa de que
todo vaya lento, ver sección 6 de la versión anterior de este documento — ya
no aplica tal cual, pero la sospecha de fondo sigue siendo válida).

### 5.4 — Bucle infinito de auto-reset por no leer el mando real

`src/update_input.c`'s `UpdateInput()` lee `REG_KEY_INPUT` (activo en bajo:
bit=0 = pulsado). **Nada en el build de 3DS escribía nunca ese registro**
(a diferencia de Linux, que sí lo hacía desde el principio vía
`Platform_Linux_PollKeys()`). Con el registro parado en 0x0000, el juego
veía "todos los botones pulsados" permanentemente, incluida la combinación
de soft-reset (A+B+START+SELECT) — así que `SoftReset()` se
re-disparaba cada frame desde el arranque (parecía que la máquina de
estados del boot estaba atascada en `gSubGameMode1=7`, cuando en realidad
se reiniciaba sin parar).

Arreglado con `Platform3DS_PollKeysIntoGba()` (nueva, en
`platform_3ds_minimal.c`), llamada cada frame desde `Port_Bios_Halt()`. Los
bits de `hidKeysHeld()` de libctru coinciden exactamente en posición con
los de GBA (bits 0-9: A,B,SELECT,START,RIGHT,LEFT,UP,DOWN,R,L) — no hace
falta tabla de remapeo, solo máscara + inversión.

### 5.5 — Doble resolución de direcciones cuando `gRomData` cae en el rango "GBA"

El bug más sutil de la sesión. En 3DS, `malloc()` puede devolver (y de
hecho devuelve — confirmado en hardware, `gRomData=0x0800eb20`) direcciones
que caen dentro del mismo rango `0x02000000-0x0A000000` que
`port_resolve_addr()`/`port_resolve_write_addr()` (`port/port_gba_mem.c`)
tratan como "dirección GBA cruda, hay que traducirla". Cualquier puntero
YA resuelto hacia dentro de `gRomData` (p.ej. `sLanguageSelectGfx`, que es
`*p_sLanguageSelectGfx = gRomData + offset`) colisiona numéricamente con esa
heurística. Al pasarlo de nuevo por el resolutor, se traduce una SEGUNDA
vez como si fuera una dirección GBA cruda, aterrizando en bytes
completamente distintos de la ROM.

Esto causaba un crash real: `Lz77Uncomp` (`port_bios.c:139`) leía un
`decompressedSize` basura de un puntero mal traducido y escribía muy más
allá del buffer destino, hasta salirse de todo el BSS del programa y
petar. Confirmado con un volcado de crash de Luma3DS **parseado a mano**
contra la estructura real de `ExceptionDumpHeader`
(`k11_extension/include/fatalExceptionHandlers.h` del repo de Luma3DS) — no
hay que adivinar el formato la próxima vez, está en ese header, campos:
`magic[2]`, `versionMinor/Major` (u16), `processor/core` (u16), `type`,
`totalSize`, `registerDumpSize` (=92, 23 words: r0-r12, sp, lr, pc, cpsr +
3 extra), `codeDumpSize` (=96), luego stack dump y 16 bytes de datos
adicionales (nombre de proceso + title ID).

No se reproduce en Linux/x86_64 (`malloc()` nunca da direcciones en ese
rango ahí), así que el bucle de depuración local no lo habría pillado —
hubo que verificar primero que la ROM de la SD era byte-idéntica a la local
(lo era, mismo SHA-1) antes de sospechar de la resolución de direcciones en
vez de un fichero corrupto.

Arreglado en `port_gba_mem.c`: `port_resolve_addr()` y
`port_resolve_write_addr()` comprueban primero si el valor cae dentro de
`[gRomData, gRomData+gRomSize)` — si es así, es inequívocamente un puntero
ya resuelto (ningún código construye una dirección GBA cruda a partir del
valor de puntero host de `gRomData`), y se devuelve tal cual.

**Confirmado en hardware**: la pantalla de selección de idioma (que usa
exactamente estos datos) pasó de crashear siempre a renderizarse
correctamente.

## 6. Corrupción de `TrackVariables` en el motor de audio — root cause encontrado y arreglado

**Actualización: root cause identificado y arreglado (sin verificar aún en
hardware real — pendiente de que el usuario pruebe el CIA).**

La causa era el mismo patrón de "dirección de ROM cruda sin traducir" de las
secciones 5.1/5.2/5.5, pero en un sitio nuevo: `InitTrack` (la función real
que arranca una pista de música/sonido) **no está en C**, es asm Thumb sin
tocar del decomp (`asm/audio_internal.s`, no aparece en
`docs/non_matching_functions.md` como pendiente de descompilar — simplemente
nunca se ha portado a C). Esa asm lee dos campos directamente de la cabecera
de sonido en ROM y los copia tal cual, sin pasar por `port_resolve_addr()`:

1. `pTrack->pVoice` (línea ~57-60 del fichero): puntero a la tabla de voces/
   instrumentos, usado luego en `AudioCommand_Voice` (`src/audio.c:1545`,
   `pVoice = &pTrack->pVoice[*pVariables->pRawData]`) — con el puntero sin
   traducir, esto indexa memoria arbitraria del host.
2. `pVariables->pRawData` (línea ~96-99, dentro del bucle que inicializa
   cada `TrackVariables` de la pista): puntero a los datos crudos de la
   partitura, exactamente el campo que `UpdateTrack()` desreferenciaba
   cuando crasheaba con `0xfefefeff` (sección original de este apartado,
   ver más abajo el texto tal cual se dejó al final de la sesión anterior).

Ambos se arreglaron insertando un `bl port_resolve_addr` en la asm justo
después de leer el valor crudo de la cabecera y antes de guardarlo en el
struct, preservando a mano los registros caller-saved (`r0`-`r3`) que
seguían haciendo falta después de la llamada (`push`/`pop` alrededor del
`bl`; `r4`-`r7` son callee-saved y sobreviven solos). `port_resolve_addr()`
ya es idempotente (ver comentario en `port_gba_mem.c` sobre
`IsWithinRomDataBuffer`), así que es seguro llamarlo aunque el valor
resultara no ser una dirección GBA cruda.

Compila limpio tanto en `platform/linux` (sigue sin reproducir el bug ahí,
como ya se sabía — `PORT_NATIVE_AUDIO_STUBS` stubea `InitTrack` a un no-op
en Linux, así que esta asm ni se ejercita en ese target) como en
`platform/3ds` (genera `mzm-3ds.cia` sin warnings nuevos). **Falta
confirmar en hardware real** que la pantalla de selección de idioma ya no
crashea al llegar al primer `VOICE`/nota con `pRawData`.

Si el crash persiste tras esto, revisar `asm/soundcode.s`
(`SoundCodeA/B/C`, mezclador PCM de bajo nivel) — no se tocó porque
parece código muerto por ahora (nada en el motor engancha DMA/timer real
todavía, ver sección 7), pero si en algún momento se conecta a NDSP habrá
que auditarlo con la misma lupa.

### Texto original de esta sección (contexto de cómo se encontró)

Bug encontrado (ya resuelto arriba), descripción original de cuando se
detectó, sin resolver, al final de la sesión previa.

**Síntoma**: tras ver la pantalla de selección de idioma unos segundos, la
app crashea dentro de `UpdateTrack()` (`src/audio.c:750`,
`var_0 = *pVariables->pRawData;`) leyendo un puntero basura
(`0xfefefeff`, confirmado con otro volcado de crash parseado igual que en
5.5).

**Ya descartado**: no es un problema de inicialización. Se añadió un
checkpoint justo después de `InitializeAudio()` en `src/init_game.c` (ver
commit `a09da7f6`) que confirma `gTrack0Variables[0].pRawData == 0x0`
correctamente nada más inicializar. La corrupción a `0xfefefeff` pasa
**después**, durante el procesamiento normal de la pantalla de selección de
idioma (antes de que el usuario llegue a pulsar nada útil — las teclas de
dirección no hacen nada visible ahí, lo cual es esperable).

**Dato curioso sin explorar**: `gTrack0Variables` vive en una sección de
enlazado personalizada `iwram_data` (macro `IWRAM_DATA` en
`include/macros.h`, activa porque `MODERN` no está definido en ningún
Makefile) en vez del `.bss` estándar — confirmado con `readelf -S` que la
sección existe y es `PROGBITS`/`WA` (con contenido real, no NOBITS). Esto
en sí no parece ser el bug (el checkpoint de arriba confirma que arranca en
cero correctamente), pero es una pista de que **este proyecto no define
`-DMODERN` en ningún Makefile** — si aparecen más rarezas de memoria en
variables `IWRAM_DATA`/`EWRAM_DATA`, revisar si conviene definir `MODERN` y
dejar que esas macros sean no-ops, moviendo esas variables al `.bss`
estándar (más simple, probablemente más seguro en este port).

**Siguiente paso recomendado**: la misma técnica de bisección con
`Port_DebugLog()` que resolvió todos los bugs de la sección 5 — añadir
checkpoints que impriman `gTrack0Variables[0].pRawData` en más puntos entre
"justo después de `InitializeAudio()`" (ya confirmado en 0) y el momento
del crash, para encontrar exactamente qué función lo pone a
`0xfefefeff`. Buenos candidatos donde mirar primero (por orden de
sospecha, sin confirmar ninguno todavía):

1. Cualquier llamada a `InitTrack()`/`QueueSound()` que se dispare durante
   la animación de selección de idioma (`LanguageSelectUpdateHighlightAnimation`,
   llamada cada frame desde `case 3`/`case 4` de `SoftResetHandler` en
   `src/soft_reset.c`) — si alguna reproduce un sonido con una cabecera
   (`pHeader`) mal resuelta, encaja con el patrón de "dirección de ROM sin
   traducir" ya visto dos veces hoy (secciones 5.1 y 5.5).
2. Un desbordamiento de buffer genuino en otra estructura que viva
   adyacente a `gTrack0Variables` en la sección `iwram_data` — comprobar con
   `readelf -S`/`nm -n mzm-3ds.elf` qué símbolo está justo antes en memoria
   y si algo podría estar escribiendo de más ahí.
3. Revisar si `UpdateAudio()`/`InitializeAudio()` se llaman más de una vez
   de forma inesperada (p.ej. re-inicializando parcialmente sin limpiar del
   todo) — el propio `InitializeAudio()` tiene guardas de reentrancia
   (`gMusicInfo.occupied`) que podrían estar enmascarando una llamada
   solapada.

## 6b. Barrido sistémico: "BASE macro cruda + desreferencia directa" (2026-08-17, sesión posterior)

Tras arreglar el audio (6a) se encontró el mismo patrón de 5.1 (puntero
`VRAM_BASE`/`PALRAM_BASE`/`EWRAM_BASE`/`OAM_BASE` + offset, desreferenciado
directamente sin pasar por `WRITE_16`/`READ_16`) repetido en **más de 40
sitios en 12+ ficheros** — no era un caso aislado, es sistémico en el motor.
Se añadió `GBA_RESOLVE(p)` a `port_gba_mem.h` (envuelve `gba_MemPtr`) y se
aplicó en cada sitio con desreferencia directa confirmada:
`in_game_cutscene.c`, `text.c` (`TextDrawMessageCharacter`/`TextDrawCharacter`,
un único punto de traducción reusado por varios call sites),
`menus/game_over.c`, `menus/title_screen.c`, `block.c`, `room.c`,
`menus/status_screen.c` (solo el sitio fuera de `#ifdef DEBUG`, el resto de
esa pantalla de debug es código muerto en este build), `menus/file_select.c`,
`menus/pause_screen_map.c`, `menus/pause_screen.c`.

**Descartados tras comprobar que NO hace falta tocarlos** (ya se traducen
solos): cualquier sitio donde el puntero crudo solo se pasa a
`DmaTransfer`/`BitFill`/`LZ77Uncomp*`/`DMA3_*` (estas ya resuelven
internamente vía `port_resolve_write_addr`/`port_resolve_copy_src`) —
`data/menus/pause_screen_data.c`'s `sPauseScreen_IgtAndTanksVramAddresses`,
la mayoría de `haze.c` (usa `DMA_SET`, registro de hardware, no memoria
directa), y varios sitios de `file_select.c`/`text.c` que solo hacen
aritmética de punteros sin desreferenciar. `data/shortcut_pointers.c`'s
`sSramEwramPointer`/`sBgPalramPointer` YA estaban bien: tienen una rama
`#if defined(TMC_3DS) || defined(PORT_NATIVE)` que los inicializa
directamente contra `gEwram`/`gBgPltt` (los buffers reales), no contra la
macro cruda — patrón a copiar si aparecen más globales de este tipo.
`menus/boot_debug.c` entero está bajo `#ifdef DEBUG` (no compilado, código
muerto en este build) — no se tocó.

**Compilado y verificado**: `platform/3ds` linka limpio, `platform/linux`
sigue sin regresiones (`--test 500` pasa). **Pendiente confirmar en
hardware real** que esto desbloquea pasar de la animación de intro al menú
principal sin nuevos crashes de este tipo.

## 6c. Silenciado el logging por frame + capturado el mensaje FATAL de `gba_MemPtr` (2026-08-17, sesión posterior)

Con los fixes de 6/6b el arranque llegaba establemente hasta la animación de
intro (confirmado en hardware, se ve bien). Pero iba muy lento: `agbmain.c`
y `Port_Bios_Halt()` hacían ~10-18 `Port_DebugLog()` por frame (cada uno una
escritura a fichero en la SD con flush inmediato). Todos esos checkpoints
(diagnóstico del cuelgue de arranque, ya resuelto) se pusieron detrás de
`PORT_VERBOSE_FRAME_LOG` (apagado por defecto) — reactivar añadiendo
`-DPORT_VERBOSE_FRAME_LOG` al Makefile de 3DS si hace falta bisecar un
cuelgue de arranque nuevo. Mejora notable de velocidad confirmada en
hardware.

También se detectó que `gba_MemPtr()` (el traductor de direcciones) hacía
`abort()` con el mensaje de error solo por `stderr` — invisible en hardware
real (pantalla "Fatal error" genérica de homebrew, sin detalle). Ahora
también lo manda a `Port_DebugLog()` (siempre activo, no depende de
`PORT_VERBOSE_FRAME_LOG`, dispara como mucho una vez antes de abortar). Esto
permitió cazar el siguiente bug real sin necesidad de parsear un volcado de
crash.

**Bug encontrado con esto**: `ProcessMenuOam`/`ProcessComplexMenuOam` en
`src/menus/pause_screen.c` (usado también por el menú de título, que reusa
el renderizador OAM genérico del pause) leían
`pFrame = pOamData[pOam->oamId].pOam;` — otro caso más del patrón de 5.1:
`.pOam` es un puntero crudo de ROM embebido en la tabla de datos
`OamArray`, no traducido. Se repetía **15 veces en el mismo fichero**
(`sed` para las 15 de golpe, todas con `GBA_RESOLVE(pFrame)` justo después
de la asignación). No confundir con `gCurrentSprite.pOam`/`struct Sprite`'s
`.pOam` usado en `src/sprites_ai/*.c` — es un campo de runtime distinto,
ya resuelto por el pipeline de sprites normal, no tocado.

**Pendiente de confirmar en hardware**: si esto arregla el crash al pulsar
un botón en el menú de título. El menú desordenado (título/"Pulse Start"
con tiles mal colocados) sigue sin explicación — puede ser este mismo tipo
de bug en otra tabla `OamArray` de datos del título, o un problema distinto
de descompresión/carga de gráficos. Investigar después de confirmar que el
crash al pulsar botón ya no ocurre.

## 6d. Siguiente crash sin resolver: `pText` corrupto en `TextProcessCurrentMessage` al elegir partida

Encontrado justo al final de esta sesión, **sin diagnosticar, sin
arreglar**. Flujo para reproducir: arrancar → selección de idioma → animación
intro → menú de título (pulsar un botón) → menú de selección de partida (3
slots) → elegir un slot → **crash**.

**Volcado de crash** (`crash_dump_00000015.dmp`, parseado con la técnica de
la sección 5.5/8 — estructura `ExceptionDumpHeader` de Luma3DS):

```
pc  = 0x0013b72c  ->  TextProcessCurrentMessage, src/text.c:1396
r1  = 0x55555555
lr  = 0x55555555
r12 = 0x0847b420  (pinta a dirección de ROM cruda sin traducir, 0x08xxxxxx)
r4  = 0x06005800  (pinta a dirección de VRAM cruda sin traducir, 0x06xxxxxx)
extra2 (DFAR) = 0x55555555
```

`0x55555555` repetido en varios registros (no es una dirección plausible en
ningún espacio de direcciones del port) — **mismo tipo de patrón sospechoso
que el `0xfefefeff` de la sección 6 (audio)**: no parece un puntero de ROM
mal traducido (eso daría algo con pinta de `0x08xxxxxx`/`0x06xxxxxx`, como sí
tienen `r12`/`r4` aquí, curiosamente — puede que esos dos SÍ sean el patrón
de siempre y `r1`/`lr` sean colaterales), sino más bien memoria realmente sin
inicializar o un valor "centinela" de algún sitio.

Código donde crashea (`src/text.c:1370-1396`, función
`TextProcessCurrentMessage`):

```c
    if (state < TEXT_STATE_NOTHING)
    {
        pMessage->delay = 0;
        pMessage->timer = 0;
        pText += pMessage->textIndex;      // <- pText llega corrupto aquí
        while (state < TEXT_STATE_NOTHING)
        {
            width = 0;
            switch (*pText & CHAR_MASK)    // <- crash: *pText, src/text.c:1396
```

`pText` es un parámetro de la función (`const u16* pText`), viene de la
tabla de punteros de texto del idioma actual (`sMessageTextPointers[gLanguage][...]`
o similar, ver otros call-sites de `TextProcessCurrentMessage` en la
sección 6b) — candidato sospechoso número uno dado el historial de hoy:
otro caso más del patrón "puntero de ROM embebido en datos, sin traducir"
(secciones 5.1, 5.5, 6b, 6c) en la tabla de punteros de texto del menú de
selección de partida, o en algo que la precede (nombre de fichero
guardado, texto de la hora de juego, etc. — el menú de selección de
partida dibuja bastante texto dinámico).

**Por dónde seguir (no hecho todavía):**

1. Igual que en 6c: el mensaje `FATAL: gba_MemPtr: ...` ahora se manda a
   `Port_DebugLog` (si esto fuera un caso de "no pasa por `gba_MemPtr` en
   absoluto", como parece — no hay línea `FATAL` en el log de esta prueba,
   así que **no** pasó por el resolutor de direcciones antes de crashear;
   el bug está en un sitio que ni siquiera lo intenta).
2. Localizar qué llama a `TextProcessCurrentMessage` desde el flujo de
   selección de partida (`src/menus/file_select.c`, buscar
   `TextProcessCurrentMessage(` — hay varias llamadas, ver sección 6b)
   y qué le pasa como `pText`/`dst`.
3. Comprobar si esa tabla de punteros de texto (o el string concreto que
   se intenta dibujar al confirmar un slot) es un `CAST_TO_ARRAY`/macro de
   direcciones crudas sin arreglar (como en 6b/6c), o un puntero embebido
   en otra estructura de datos (como en 6c con `.pOam`).
4. Si no es eso: mirar si `pMessage->textIndex` está corrupto o fuera de
   rango (el `+=` podría estar desplazando un puntero por lo demás válido
   fuera de los límites del buffer real).

## 6e. Avance: Entrada a Gameplay y Selección de Partida

1. **Crash en selección de partida (`TextProcessCurrentMessage`):**
   - Resuelto el puntero de texto `pText` en `src/text.c` con `GBA_RESOLVE(pText)`.
2. **Entrada a Gameplay / Animación de Samus:**
   - Resueltos punteros de OAM, paletas y frames de Samus (`gSamusData`, `sSamusOamDataPointers`, etc.) con `GBA_RESOLVE`.

## 6f. Avance: Memory Layout EWRAM, Bloques Destructibles y Puertas

1. **Disparo y apertura de puertas / bloques destructibles:**
   - La ROM original de GBA asume que los buffers de tilemaps (`gTilemap`, `gCommonTilemap`, `gClipdataCollisionTypes`, `gBgPointersAndDimensions`, etc.) residen de forma contigua en EWRAM (`0x02000000..0x02040000`).
   - Creado `platform/3ds/ewram_symbols.ld` mapeando todos los símbolos de EWRAM directamente a sus desplazamientos exactos dentro del array global `gEwram`.
   - Con esto, los proyectiles impactan, las puertas se abren y muestran espacio vacío, los bloques destructibles se rompen con gráficos correctos y el suelo que se desmorona funciona como en GBA.

## 6g. Avance: Renderizado de Estatuas Chozo, Sprites Multi-Parte y Wireframe

1. **Estatuas Chozo y Sprites Multi-Parte:**
   - El collider de la estatua Chozo estaba presente pero el sprite no se dibujaba porque `ChozoStatueSyncSubSprites`, `UnknownItemChozoStatueSyncSubSprites` y `SpriteUtilSyncCurrentSpritePositionWithSubSprite*` (`src/sprite_util.c`) leían tablas de sub-sprites de la ROM sin resolver (`pData`).
   - Añadido `GBA_RESOLVE(pData)` en todas las funciones de sincronización de sub-sprites.
2. **Alineación de Wireframe y Menú de Pausa:**
   - La imagen wireframe de Samus al obtener la Morfosfera aparecía desplazada hacia una esquina debido a que el compilador de 3DS generaba `sizeof(struct PauseScreenWireframeData) == 14` en lugar de 16 bytes.
   - Añadido padding de 2 bytes (`u8 _pad[2];`) a `struct PauseScreenWireframeData`, `struct WorldMapData`, `struct MinimapAreaName` y `struct ChozoStatueTarget`.

## 6h. Avance: Persistencia de Guardado (SRAM), Crasheo de Niebla (Haze) y Marcadores Chozo

1. **Persistencia del Guardado (SRAM -> SD):**
   - Implementadas funciones `Port_LoadSram()` y `Port_SaveSram()` en `src/sram/sram.c` vinculadas a `sdmc:/3ds/Metroid Zero Mission 3DS/mzm.sav`.
   - `main_3ds.c` carga el archivo `.sav` al arrancar el juego y `SramWrite`/`SramWriteUnchecked` sincronizan los 64 KB de SRAM a la SD cada vez que se guarda partida.
2. **Paleta de Samus al Guardar:**
   - Añadido `src = GBA_RESOLVE(src);` en `SamusCopyPalette` (`src/samus.c`) para resolver los punteros de paleta de la ROM (`sSamusPal_PowerSuit_SavingPointers`), evitando que Samus desaparezca durante la animación de guardado.
3. **Crasheo en salas con efecto de calor/niebla (`HazeSetupCode`):**
   - En GBA original, `HazeSetupCode` copiaba funciones a la RAM (`gNonGameplayRam.inGame.hazeCode`) y saltaba a ejecutarlas. En 3DS, saltar a buffers de datos sin permisos de ejecución producía abortos de instrucción (`PC: 0x0025CCDC`).
   - Reemplazada la ejecución en RAM por asignación directa de punteros a funciones C nativas (`Haze_Bg3`, `Haze_Bg3StrongWeak`, etc.).
4. **Marcadores de Objetivo de Estatuas Chozo:**
   - `sChozoStatueTargetPathPointers` se inicializaba estáticamente en el constructor antes de que la ROM estuviera cargada (apuntando a `NULL`).
   - Sustituido por `GetChozoStatueTargetPath(area)` en `src/menus/pause_screen_sub_menus.c` para resolver dinámicamente las rutas y coordenadas en tiempo de ejecución.

## 6i. Bug de guardado: el `.sav` se persiste todo a ceros y la partida no aparece en selección — RESUELTO y confirmado en hardware (2026-08-18)

Síntoma reportado por el usuario: al guardar y volver a la pantalla de selección de partida, la partida no aparece (ni en la misma sesión ni tras reiniciar).

**Evidencia dura de hardware (vía FTP):**
- `sdmc:/3ds/Metroid Zero Mission 3DS/mzm.sav` se crea y se reescribe a **todo ceros** (65536 bytes, `0x00` en todas las posiciones). El tamaño coincide con `sizeof(gSramMem)` (=`0x10000`), luego `Port_SaveSram()` (en `src/sram/sram.c`) sí escribe el buffer `gSramMem`, pero ese buffer está a ceros cuando se escribe.
- Instrumentado `SramTestFlash()` (con checkpoint en `Port_DebugLog`): `SramTestFlash: flags=4 corrupt=4`. El flag 4 = la verificación final del test de escritura/lectura falla. **`gSramCorruptFlag` queda a 1** (no nulo).
- La consecuencia directa está en `unk_fbc()` (`src/sram_misc.c`): su `switch` de escritura a flash está envuelto en `if (!gSramCorruptFlag)`, así que **con el flag de corrupción puesto, TODAS las escrituras de guardado se saltan**. La partida nunca llega a `gSramMem` → `.sav` a ceros → en el siguiente arranque no hay partida. Por eso "guarda" (la animación confirma) pero luego no aparece nada.

**Flujo de `SramTestFlash()` que falla (`src/save_file.c:880`):**
1. `SramWriteChecked(sMetZeroSramCheck_Text, SRAM_BASE+OFFSET, 16)` → escribe el texto de test en flash (gSramMem), verifica. OK (sin flag 1).
2. `SramWriteUnchecked(SRAM_BASE+OFFSET, text, 16)` → lee flash a buffer local de pila `text`. OK.
3. `text[i]++` → texto incrementado.
4. `SramWriteChecked(text, SRAM_BASE+OFFSET, 16)` → escribe el texto modificado, verifica. Sin flag 2 (su propio `SramCheck` "pasa", comparando contra el MISMO origen corrompido).
5. `SramWriteUnchecked(SRAM_BASE+OFFSET, text, 16)` → relee flash a `text`.
6. Verificación final `text[i] == sMetZeroSramCheck_Text[i]+1` → **falla** (flag 4).

**Sospecha principal (clase de bug "doble resolución de direcciones", sección 5.5):** el buffer local de pila `text` tiene una dirección que numéricamente cae en un rango que `port_resolve_copy_src()`/`port_resolve_write_addr()` (`port/port_gba_mem.c`) tratan como "dirección GBA cruda" y la traducen como si fuera EWRAM/IWRAM/etc. En el paso 4, `port_resolve_copy_src(text)` traduce mal el puntero de pila → copia basura a gSramMem → el `SramCheck` interno "pasa" (compara basura vs. el mismo origen mal traducido) → el flag 4 salta al final.

Notas de memoria concretas de hardware:
- `gRomData=0x14189c00` (heap, FUERA de `0x02..0x0A` y de `0x0E...`; sin colisión de ROM en esta sesión).
- Los buffers estáticos `gEwram`/`gSramMem`/`gVram`/etc. están en `0x0026xxxx-0x002dxxxx` (del `nm` del ELF 3DS), por debajo de `0x02000000` → no colisionan.
- `Platform3DS_IsActiveStackAddress()` (`platform/3ds/source/platform_3ds.c:331`) solo detecta pila en el rango `0x08..0x0A` y solo dentro de `±64KB` del `sp` actual — si la pila está en OTRO rango GBA, no se cubre.

**Diagnóstico que se está añadiendo ahora** (para confirmar el rango de pila y el puntero traducido):
- En `SramTestFlash`: log de `text=%p` (dirección del buffer de pila) y los 16 bytes de `MetZeroSramCheck_Text` en hex.
- En `SramWrite()`: log de `src=%p->%p dest=%p->%p` (punteros crudos vs. resueltos) para ver exactamente qué puntero se traduce mal.

**Root cause CONFIRMADO con el siguiente log de hardware (2026-08-18):**

```
SramWrite: src=0x8007b28->0x14191728  dest=0xe007f50->0x268284  size=16
SramTestFlash: flags=4 corrupt=4 text=0x8007b28 testBytes=2c7b0008118880200840002802d00220
```

- El buffer local de pila `text` de `SramTestFlash` vive en `0x8007b28` — **dentro del rango ROM GBA** `[0x08000000, 0x08000000+gRomSize)`, que se solapa con la pila del hilo principal de la 3DS.
- `port_resolve_copy_src(text)` lo traduce MAL como dirección ROM: `0x8007b28 -> 0x14191728` (= `gRomData + 0x7b28`). En el paso 4 de `SramTestFlash` copia basura de la ROM a `gSramMem+0x7f50`, su `SramCheck` interno "pasa" (compara basura vs. el mismo origen mal traducido), y la verificación final (flag 4) salta → `gSramCorruptFlag=4`.
- El `dest` sí se resuelve bien (`0xe007f50 -> 0x268284` = `gSramMem + 0x7f50`), por lo que solo el ORIGEN de pila es el problema.
- **Por qué pasaba:** el build 3DS enlaza `platform/3ds/source/platform_3ds_minimal.c`, cuyo `Platform3DS_IsActiveStackAddress()` era un **stub que devolvía `0` siempre** (líneas 180-183). El mecanismo anti-doble-resolución de pila existía en `port_resolve_copy_src` pero se apoyaba en esa función, que en el build mínimo nunca detectaba la pila. (La implementación real solo existe en `platform/3ds/source/platform_3ds.c`, que NO se enlaza en este build.)

**Fix aplicado (2026-08-18):**
1. `platform/3ds/source/platform_3ds_minimal.c`: `Platform3DS_IsActiveStackAddress()` implementado de verdad (mismo criterio SP ± 64 KB que la versión de `platform_3ds.c`).
2. `port/port_gba_mem.c`: los resolvers ahora usan la comprobación de pila de forma consistente:
   - `port_resolve_addr()` y `port_resolve_write_addr()`: si el valor está en un rango GBA numérico Y además es una dirección de pila activa → se devuelve tal cual (host pointer), antes de intentar traducirlo como GBA.
   - `port_resolve_copy_src()`: mismo chequeo, aplicado a TODOS los rangos GBA ambiguos (antes solo `0x08..0x0A` y solo vía el stub roto).
   - Añadidos helpers `InGbaNumericRange()`/`IsActiveStackPtr()`.

**Confirmado en hardware (2026-08-18):** el usuario reinstaló el CIA y ahora **el guardado persiste y carga correctamente** (guarda partida, aparece en selección y la retoma). El fix de pila era correcto.

**Efecto secundario positivo confirmado:** las **indicaciones de las estatuas Chozo ahora se ven bien** (bug 4 / sección 6h). Causa raíz compartida: los indicadores Chozo se leían desde datos del guardado (`gSramMem` / `gSram`, items obtenidos y tiles visitadas), que estaban a ceros por la corrupción de SRAM; al arreglar la resolución de `gSramMem` (y su contenido ahora persistente), los marcadores se muestran correctamente. Refuerza que la corrupción de SRAM afectaba a varios subsistemas a la vez.

## 6j. Bug del tanque de misiles no recogible — RESUELTO, y nuevo problema del jefe que no aparece (2026-08-18)

**Bug del tanque de misiles (bug 2) — RESUELTO y confirmado en hardware:**

Síntoma: en una partida nueva, Samus atravesaba el tanque de misiles sin recogerlo (sin banner ni animación). Tras instrumentar `BgClipCheckTouchingTransitionOrTank` (`src/bg_clip.c`), el log de hardware mostró:

```
TankTouch: behavior=57 idx=5 itemType=0 explored=65536
```

- `behavior=57`, `idx=5` → el tanque de misiles (`CLIP_BEHAVIOR_MISSILE_TANK`) se **detecta correctamente**.
- `explored=65536` (≠0) → el gate de exploración (`MinimapCheckIsTileExplored`, línea 511) **pasa**, no era el problema.
- `itemType=0` (`ITEM_TYPE_NONE`) → **el fallo**: `sTankBehaviors[5].itemType` devolvía 0 en vez de `ITEM_TYPE_MISSILE` (1), por lo que `if (i != ITEM_TYPE_NONE)` (línea 508) saltaba la recolección y Samus pasaba de largo.

**Root cause:** `sTankBehaviors` se lee de la ROM (`Port_ResolveRomData(0x083468d4)`, tabla EU). Al comparar los bytes reales de la ROM EU (`mzm_eu.gba`, cabecera `BMXP`) con el array C del decomp (`src/data/block_data.c:863`):
- Las entradas de la tabla en la ROM son de **8 bytes**, pero `struct TankBehavior` (`include/structs/block.h`) estaba definido con **6 bytes** (`u8 itemType + u8 underwater + u8 messageId + u16 revealedClipdata`).
- Con stride de 8 bytes TODAS las 12 entradas coinciden con el array C (campos en los mismos offsets: itemType@0, underwater@1, messageId@2, revealedClipdata@4-5; bytes 6-7 siempre `00`).
- Como el port indexa la tabla con `sizeof(struct TankBehavior)` (6), todo a partir del índice 1 queda desalineado; en el índice 5 (missile) leía `itemType=0`.

Por qué no se había detectado antes: el decomp de referencia compila el array desde `tools/extractor` (bytes `.inc`), que es auto-consistente y nunca necesita que el struct coincida con la ROM; pero el port nativo lee la tabla de la ROM en runtime, así que `sizeof` DEBE ser 8.

**Fix aplicado:** en `include/structs/block.h`, `struct TankBehavior` ahora tiene un campo final `u16 padding;` (bytes 6-7 de la ROM, siempre 0), haciendo el struct de 8 bytes y alineándolo con la ROM.

**Confirmado en hardware:** tras reinstalar el CIA, el usuario puede recoger los misiles (log `TankTouch: ... itemType=1 ...`) y se dispara correctamente el evento del jefe.

**Nuevo problema relacionado — CORREGIDO (2026-08-18, sesión posterior):** justo después de coger los misiles, **el enemigo de la sala no aparece** y Samus queda atrapada. El log inicial mostraba: `TankTouch itemType=1` → `ModeChange GM 0x05` (banner) → vuelta a `GM 0x04` → `RoomLoad` → sin más actividad. Con esos datos se sospechó inicialmente de un "jefe Ruins Test" que no spawneaba por culpa de `gPauseScreenFlag`.

**⚠️ CORRECCIÓN IMPORTANTE (invalida el diagnóstico de abajo marcado como "provisional"):** la sala 12 de Brinstar (donde está el primer tanque de misiles) **no es la sala de "Ruins Test"**. Esa sala existe en el juego pero es de Chozodia, mucho más adelante (gateada por `EVENT_FULLY_POWERED_SUIT_OBTAINED`, ver `connection.c:1083` `ConnectionCheckPlayCutsceneDuringTransition`, room `0x2A`/`0x2B`). Se confirmó verificando `sBrinstarRoomEntries[12]` en `src/data/rooms_data.c`: `defaultSpriteset = 32`, `firstSpritesetEvent = EVENT_NONE`, `pFirstSpriteData = sEnemyRoomData_Empty` — sin gating por evento, sin variante "jefe".

**El enemigo real es Deorem** (`PSPRITE_DEOREM_FIRST_LOCATION`, spriteId=66 en el log `SpriteInitPrimary`), que **ya está cargado en la sala desde la entrada** (se ve en el log mucho antes de tocar el tanque), colgado del techo esperando. Por tanto la sala **no necesita ninguna recarga** (`RoomLoadEntry`/`SpriteLoadAllData`) para que aparezca — el análisis de `gPauseScreenFlag` bloqueando esas funciones es correcto como observación (así se comporta el juego original en cada recogida de ítem, código decomp sin modificar) pero **irrelevante para este bug**, ya que Deorem no depende de recarga de sala.

El disparador real está en `DeoremWaitingForFight` (`src/sprites_ai/deorem.c:539`):
- Mientras `gEquipment.maxMissiles == 0` (antes del tanque), Deorem solo hace temblores de ambiente decorativos.
- En cuanto `maxMissiles != 0` (justo tras coger el tanque) y Samus camina bajo su posición X central, se dispara la secuencia real de ataque: bloquea la cámara (`gLockScreen`), vuelve sólidas las paredes laterales de la sala (`DeoremChangeLeftWallClipdata`/`DeoremChangeRightWallClipdata` con `CAA_MAKE_SOLID_GRIPPABLE` — esto es lo que probablemente hace que Samus "no pueda salir", es la mecánica normal de la pelea, no un bloqueo de movimiento) y hace bajar al bicho por el techo con nube de polvo (`ParticleSet(..., PE_TWO_MEDIUM_DUST)` — la "nube de humo" que describe el usuario).

**Diagnóstico en hardware confirmado (2026-08-18, misma sesión, ~10 iteraciones de CIA vía FTP): el sprite se corrompe/desaparece 1-6 frames después de empezar a bajar — SIN CRASH y SIN CUELGUE. SIN RESOLVER, root cause aún desconocida.**

Secuencia observada de forma consistente y reproducible en varias partidas:
1. `DeoremWaitingForFight` (`deorem.c:539`) detecta correctamente `maxMissiles != 0` + Samus bajo el rango X y dispara el ataque: `gLockScreen`, `DeoremChangeLeftWallClipdata`/`RightWallClipdata` (`CAA_MAKE_SOLID_GRIPPABLE`), 6× `SpriteSpawnSecondary(SSPRITE_DEOREM_SEGMENT, ...)` (todas con éxito, slots libres encontrados sin problema), `pose = DEOREM_POSE_SPAWN_GOING_DOWN`.
2. En el primer frame de `DeoremSpawnGoingDown` (`deorem.c:637`) se ejecutan `ParticleSet`, 2× `SoundPlay` y `PlayMusic(MUSIC_WORMS_BATTLE, 0)` — **todas completan sin error** (confirmado con logs antes/después de cada llamada).
3. El sprite de Deorem (slot 0 en `gSpriteData`) sigue con `status=0x0003` (`EXISTS|ONSCREEN`), visible (`notdrawn=0`) y con `yPosition` avanzando con normalidad durante los primeros 1-5 frames tras entrar en `DEOREM_POSE_SPAWN_GOING_DOWN`.
4. **En algún punto entre esos primeros frames (varía: se ha visto desaparecer tan pronto como el 3er frame y tan tarde como más allá del 5º), el sprite deja de existir por completo**: ya no aparece en ningún escaneo de `gSpriteData` buscando `spriteId == PSPRITE_DEOREM_FIRST_LOCATION && SPRITE_STATUS_EXISTS`, y su función de IA (`Deorem()`) deja de ser invocada — sin pasar nunca por ninguna pose de "muerte" (`SPRITE_POSE_DESTROYED`, `DEOREM_POSE_DYING_GOING_DOWN`, etc. — el rastreador de cambios de pose no registra ninguna transición posterior a `9`).
5. **El resto del juego sigue funcionando con total normalidad** después de la desaparición: `SpriteUpdate` se seguía ejecutando cada frame (`stopSprites=0`), Samus se movía y su `pose` cambiaba con normalidad, sin ningún cuelgue ni caída de FPS perceptible. Tampoco hay volcado nuevo en `sdmc:/luma/dumps/arm11/` ni línea `FATAL: gba_MemPtr` en el log (el mecanismo de `abort()` de `port_gba_mem.h:69` para direcciones GBA no resueltas no se disparó).

**Hipótesis descartadas con evidencia directa de hardware:**
- ❌ `gPauseScreenFlag` bloqueando `RoomLoadEntry`/`SpriteLoadAllData` — irrelevante, Deorem no depende de recarga de sala (ver arriba).
- ❌ `gPreventMovementTimer` congelando la IA de sprites (`SpriteUtilCheckStopSpritesPose`, `sprite_util.c:2328`) — confirmado con log que en el momento exacto del disparo del ataque, `gPreventMovementTimer=0` y `stopSprites=0`.
- ❌ Cuelgue/deadlock del hilo principal — descartado: `SpriteUpdate` sigue corriendo con normalidad (logs `stopSprites=...` continuos) y Samus se sigue moviendo bien después de la desaparición.
- ❌ Audio/música (`PlayMusic(MUSIC_WORMS_BATTLE, ...)`, primera vez que se cambia de pista en toda la partida; sospecha reforzada porque `sSoundDataEntries` se resuelve desde ROM vía `Port_ResolveRomData(0x0808ff4cu)` en `port/generated/audio_rom.c:18`, la misma clase de mecanismo que causó el bug de `TankBehavior`, y porque el port tiene un hilo de audio real (`port_audio_3ds.c` + `port_m4a_backend.cpp`) corriendo en paralelo) — **descartado con un build de diagnóstico** que desactivaba las 2 llamadas a `SoundPlay` y la llamada a `PlayMusic` en `DeoremSpawnGoingDown` (macro `TMC_3DS_DIAG_DISABLE_DEOREM_AUDIO`, ver más abajo): el sprite desapareció igualmente (`VANISHED at tick=1515`, tras solo 3 frames visibles). El audio queda descartado como causa directa.

**Dato más fuerte a favor de una condición de carrera (no confirmado, principal sospecha para retomar mañana):** el frame exacto en el que desaparece **no es constante** entre partidas (se ha visto desde el 3er hasta más allá del 5º-8º frame de la caída), lo cual apunta a algo dependiente del *timing*/orden de ejecución entre hilos más que a un bug determinista de lógica del juego. El port lanza 2 hilos nativos aparte del principal (`platform_3ds_minimal.c:66` hilo de timing GBA, `port_audio_3ds.c:151` hilo de audio) — aunque el hilo de audio ya se ha descartado como causa directa, **no se ha revisado si el hilo de timing (`Port_GbaTiming_ThreadMain`) toca alguna memoria compartida con `gSpriteData`/`gCurrentSprite`**, ni se ha comprobado si hay algún otro punto de escritura concurrente sin proteger.

**Instrumentación de debug añadida esta sesión (queda en el código, guardada tras `#ifdef TMC_3DS`, no afecta al build normal):**
- `src/sprites_ai/deorem.c`: rastreador de cambios de pose en `Deorem()` (línea ~2798) + log periódico cada 5 llamadas mientras `pose==DEOREM_POSE_SPAWN_GOING_DOWN` (status/posición/flags) + logs antes/después de cada llamada de audio en `DeoremSpawnGoingDown` + log único al final del primer dispatch en pose 9. Bloque de audio protegido tras `#if defined(TMC_3DS) && defined(TMC_3DS_DIAG_DISABLE_DEOREM_AUDIO)` — para repetir el experimento de descartar audio, añadir `-DTMC_3DS_DIAG_DISABLE_DEOREM_AUDIO \` como línea propia dentro de `CFLAGS` en `platform/3ds/Makefile` (después de la línea `-DPORT_NATIVE_AUDIO_STUBS \`, con su propio `\` de continuación) y recompilar.
- `src/sprite.c`: en `SpriteSpawnSecondary` (línea ~2696), log de cada spawn (slot/id/gfx/posición) o de fallo por falta de slot. En `SpriteUpdate` (línea ~1628), log periódico (`stopSprites`, `gPreventMovementTimer`, pose de Samus) + escaneo cada 3 ticks de `gSpriteData` buscando a Deorem vivo, con log explícito `"Deorem VANISHED at tick=N"` en el tick exacto en que deja de encontrarse + 3 checkpoints alrededor de la llamada a la IA de cada sprite (`post-AI`, `post-checks`) que confirmaron que, al menos en el frame en que se dispara el ataque, el sprite sigue sano justo antes y después del post-procesado genérico (animación/colisión) de ese mismo frame.
- Marca manual: pulsar **SELECT** durante la partida escribe `USER MARK: tick=N` en el log, para poder correlacionar acciones del usuario con el tick exacto sin ambigüedad.

**Próximos pasos recomendados para retomar mañana:**
1. Añadir el mismo tipo de log de "tick" (cada 1-3 frames, no cada 30) desde el momento en que empieza `DEOREM_POSE_SPAWN_GOING_DOWN` para acotar el frame exacto de desaparición con precisión de 1 frame en varias partidas seguidas, y ver si el frame exacto correlaciona con algo (nº de sprites activos en pantalla, posición de Samus, si Samus está disparando, etc.).
2. Revisar `Port_GbaTiming_ThreadMain` (`platform/3ds/source/platform_3ds_minimal.c`) para confirmar si toca memoria compartida con el estado de sprites sin lock.
3. Probar el mismo experimento de aislar-por-desactivación con `DeoremSpriteDebrisSpawn`/`ParticleSet`/`SpriteDebrisInit` (los únicos efectos que quedan sin descartar dentro del primer frame de `DeoremSpawnGoingDown`), ya que el audio queda descartado pero esas llamadas no se han aislado todavía.
4. Considerar log/verificación de `SpriteUtilSamusAndSpriteCollision` (colisión Samus-sprites, se llama una vez por frame en `SpriteUpdate` antes del bucle principal) como sospechoso de "matar" a Deorem por una colisión mal calculada, aunque no se ha visto pasar por ninguna pose de muerte — comprobar si existe algún camino que ponga `status = 0` directamente sin pasar por el pose de destrucción.

## 6k. Sesión de audio 2026-08-19: seis bugs reales encontrados y arreglados, petardeo reducido pero persiste, nueva regresión de FPS

Punto de partida: CIA de la rama `wip/3ds-port-audio` instalable, pipeline NDSP conectado (sección 6j previa) pero solo silencio (`wholePeak=0`, `pRawData=0x0` siempre). Sesión larga de ida y vuelta con el usuario probando en 3DS real (New3DS) y en Azahar, con seis bugs reales encontrados y arreglados en orden, cada uno confirmado con evidencia dura (volcados de crash parseados a mano con la estructura `ExceptionDumpHeader` de sección 5.5/8, o capturas de PCM analizadas con espectrogramas/histogramas). Todos los commits están en `wip/3ds-port-audio` (`54305f29`..`91ea2156`), pusheados.

**Metodología nueva de esta sesión — verificación de audio sin poder "escuchar":**
- El puente NDSP ya volcaba el PCM crudo mezclado por el motor a `sdmc:/3ds/mzm_audio_dump.bin` (diagnóstico `DumpRaw()` en `port/port_mzm_audio_glue.c`, existente de sección 6j). Se recupera por FTP, se convierte a WAV, y se genera un espectrograma (`ffmpeg -lavfi showspectrumpic`) — imagen que sí se puede inspeccionar visualmente aunque no se pueda "oír": música real se ve como bandas tonales estructuradas que varían en el tiempo; ruido/petardeo se ve como banda ancha casi constante en todo el rango de frecuencias.
- Referencia de comparación: se grabó la ROM real (`mzm_eu.gba`) corriendo en `mgba-qt` en este mismo PC, capturando la salida de audio real del sistema vía PulseAudio/PipeWire (`parecord` sobre el `.monitor` del sink activo) — funciona en este entorno aunque el compositor gráfico de la sesión remota esté roto (capturas de pantalla salen en blanco; el audio no depende de eso).
- Un histograma de bytes crudos del volcado (qué valores de byte son más frecuentes) fue la pista clave que destapó el bug de signo (ver más abajo): picos masivos en `0x00` y cerca de `0xFF`, cero muestras cerca de `128`, es la firma de PCM con signo centrado en 0, no sin signo centrado en 128.
- **Trampa a evitar la próxima vez:** `mzm_audio_dump.bin` se abre en modo `"ab"` (append) y nunca se trunca entre sesiones — un volcado "nuevo" puede contener audio mezclado de builds anteriores, mucho más rotos. Borrar el fichero por FTP (`DELE 3ds/mzm_audio_dump.bin`) antes de cada prueba nueva si se va a analizar.
- Nueva herramienta `tools/audit_rom_pointer_fields.py`: heurístico basado en la convención de nombres `pNombre` (Hungarian notation) del proyecto, sin dependencias. Automatiza el barrido manual de "campo puntero copiado de un struct y desreferenciado sin `GBA_RESOLVE`" que ya se había repetido varias veces a mano (secciones 5.1/6b/6c/6d). Encontró dos de los bugs de audio de esta sesión, y de paso uno nuevo sin arreglar fuera del audio (ver "Pendientes" más abajo).
- Nuevo `make test-unit` en `platform/linux/Makefile`: reactiva `port/port_gba_mem_test.c` (importado de `zelda-tmc-3ds` en la sección 2 pero roto/nunca adaptado — dependía de `port_asset_loader.h`, que no existe en mzm). Ahora prueba de verdad `port_resolve_addr`/`port_resolve_write_addr`/`port_resolve_copy_src`, con un caso de regresión específico para el bug de sección 6i (doble resolución de pila) usando el valor real observado en hardware (`0x08007b28`) y un doble de test para `Platform3DS_IsActiveStackAddress`. Corre en Linux, sin ROM ni 3DS, en segundos.

**Los seis bugs (orden cronológico del hallazgo):**

1. **`InitTrack` no resolvía `pHeader` en sí mismo** (`asm/audio_internal.s`, commit `54305f29`) — solo resolvía `pVoice` y cada `pRawData` de pista, pero el propio puntero de cabecera (`sSoundDataEntries[].pHeader`, leído crudo de la ROM) se desreferenciaba sin traducir varias veces dentro de la función. No crasheaba (la dirección cruda cae dentro del linear heap de la 3DS) pero leía basura — por eso `pRawData` se quedaba a 0 y el motor "arrancaba" sin datos reales. Se resuelve a un registro local para las desreferencias internas, conservando el valor crudo original en pila para el campo `TrackData.pHeader` (comparado por identidad en varios sitios de `music_wrappers.c`/`audio_wrappers.c` contra relecturas frescas igual de crudas).

2. **`AudioCommand_Voice` no resolvía `pVoice->pSample`** (`src/audio.c`, commit `50601ea1`) — mismo patrón, un nivel más adentro: la tabla de voces ya estaba resuelta, pero el campo `pSample` dentro de cada entrada (dirección cruda de ROM apuntando a los datos PCM/onda reales) se copiaba sin traducir a `pVariables->pSample1`/`pSample2`, desreferenciados después por el mezclador (`unk_4e10`, `unk_4f8c`). Confirmado con el espectrograma del volcado: ruido de banda ancha sin ninguna estructura tonal. Dos ramas que empaquetan un valor pequeño en los bits crudos del puntero (no una dirección real) se dejan sin resolver a propósito.

3. **`unk_2030` desreferenciaba `pSound->pVariables` sin comprobar NULL** (`src/audio.c`, mismo commit `50601ea1`) — al robar un slot de voz PSG. En GBA real, escribir a través de NULL en ese offset cae en BIOS ROM y el bus lo descarta en silencio (por eso el decomp "matching" original nunca necesitó el guard — `ResetTrack`/`StopMusicOrSound` sí lo tienen para la misma operación). En el port es una desreferencia real de host y revienta. Confirmado con volcado de crash: PC dentro de `unk_2030`, `r3` (=`pSound->pVariables`) = 0. Pasaba de forma determinista con el primer efecto de sonido PSG de la partida (coincide con "crashea al pulsar Start").

4. **`QueueSound`/`unk_2f00` leían `pHeader[0]`/`pHeader[2]` directamente** (`src/audio_wrappers.c`, `src/music_wrappers.c`, commit `de6933ef`) — encontrado por el nuevo script de auditoría. Mismo `pHeader` crudo de las funciones 1, pero desreferenciado directamente como bytes (campos "cantidad de pistas"/"prioridad" de la cabecera) antes incluso de llegar a `InitTrack`. Arreglado envolviendo solo esas dos lecturas con `GBA_RESOLVE`, sin tocar el valor almacenado (debe seguir crudo para las comparaciones de identidad).

5. **`gUnk_300376C` declarado `[1]` pero indexado `& 7` (0-7)** (`src/globals1.c`, commit `2d7694e2`) — bug distinto a los anteriores, no es de traducción de direcciones. En la ROM real (`mzm_eu.map`), este array de 1 elemento está pegado en memoria justo antes de `gPsgSounds[4]`, así que los índices 1-4 alias-ean silenciosamente sobre `gPsgSounds` — los dos globals se comportan como un array de 5 slots por la disposición fija del linker de GBA, algo que nadie documentó al nombrar la variable (de ahí el `Unk`). El linker nativo de 3DS no reproduce esa adyacencia (confirmado con `arm-none-eabi-nm`: están en direcciones completamente distintas del ELF compilado), así que el mismo acceso fuera de rango caía en memoria no relacionada. Arreglado dimensionándolo a 8 (todo el rango realmente usado); no reproduce el aliasing accidental de la ROM, pero nada en el código nombrado/decompilado parece depender de él a propósito.

6. **El puente NDSP trataba PCM con signo como si fuera sin signo** (`port/port_mzm_audio_glue.c`, commit `de327f2a`) — `soundRawData` se escribe directamente en `REG_DMA1_SRC`/`REG_DMA2_SRC`, es decir, alimenta el FIFO de Direct Sound real de GBA, que consume PCM **con signo** de 8 bits (hecho de hardware bien documentado). El puente restaba 128 como si fuera sin signo, convirtiendo una señal ya correcta en amplitud básicamente descontrolada. Confirmado con histograma de bytes del volcado (ver metodología arriba). Arreglado reinterpretando cada byte como signed directamente, sin sesgo.

**Regresión de rendimiento encontrada y arreglada a medias (commit `9eea72a9`):** con el motor de audio ya haciendo trabajo real cada frame, el hilo consumidor de NDSP (`platform/3ds/source/port_mzm_audio_3ds.c`) competía por CPU con el bucle principal del juego y el hilo de timing de GBA (que se despierta cada ~73µs) porque los tres estaban fijados al Core 0. Se movió el hilo de audio al Core 1 cuando `Platform3DS_CanUseCore1()` es cierto (New3DS con presupuesto de CPU concedido vía `APT_SetAppCpuTimeLimit`), quedándose en Core 0 en Old3DS. **Esto rompió el audio por completo la primera vez**: `platform/3ds/cia/mzm3ds.rsf` tenía `AffinityMask: 1` (máscara de capacidades del exheader, a nivel de kernel, que se aplica ANTES e independientemente de `APT_SetAppCpuTimeLimit`) — solo permitía el Core 0, así que `threadCreate` para el Core 1 fallaba en silencio, y como nada llamaba nunca a la función de respaldo ya existente `Port_MzmAudio_Pump()`, el resultado era silencio total sin ningún error visible. Arreglado: `AffinityMask: 3` (Core 0+1) y `Port_MzmAudio_Pump()` conectada de verdad al bucle por frame (`src/agbmain.c`) como red de seguridad real.

De paso (commit `fa6cebbe`): el contador "F ###" en pantalla era un acumulado total de frames presentados, no unos FPS reales — pedido explícito del usuario. Ahora `port_ppu_mzm.c` calcula una tasa real con ventana móvil de 1 segundo (`osGetTime()`), etiqueta cambiada a "FPS ###".

**Estado tras todo lo anterior (2026-08-19, sin resolver):**
- El petardeo **mejora notablemente** tras el fix de signo (se intuyen sonidos de fondo del juego) pero **persiste** — no es limpio todavía. Sin diagnosticar en detalle en esta sesión: pendiente repetir el ciclo volcado→espectrograma con un CIA que ya incluya el placement de Core 1, para descartar que parte del petardeo restante sea underrun de la ring (`ndsp: toRead=0/256` aparecía ~29% de las veces en el log previo al fix de núcleo) en vez de datos todavía corruptos en el motor.
- **Nueva regresión: ~20 FPS de media** (objetivo 60) tras el movimiento a Core 1 — contraintuitivo, ya que el propósito del cambio era liberar el Core 0. Sin diagnosticar todavía. Sospechas a revisar primero la próxima sesión, ninguna confirmada: (a) puede que `APT_SetAppCpuTimeLimit`/`AffinityMask: 3` tenga algún coste de arranque o interacción con `osSetSpeedupEnable`/`ReleaseKernelMajor/Minor` del `.rsf` no contemplada; (b) puede que el propio motor de audio decompilado (`asm/audio_internal.s` + `asm/soundcode.s`, corriendo de verdad por primera vez con datos reales) sea, sencillamente, caro por frame y esto ya estuviera pasando antes de moverlo de núcleo, enmascarado por el contador de FPS roto (ahora arreglado, así que es la primera vez que se ve un número real); (c) posible interacción entre el hilo de audio en Core 1 y el hilo de timing de GBA / GSP en Core 0 (sección 5.3) que no se ha revisado. Medir con logs de timing (`timing thread: heartbeat`) con marca de tiempo real, no solo contadores, sería el siguiente paso más útil.

## 6l. Sesión de continuación 2026-08-19: falsa pista de la FPS (build incremental obsoleto), nuevo crash real en la intro, y automatización de pruebas en Azahar

Punto de partida: la sesión anterior (6k) se quedó sin commitear, iterando manualmente contra Azahar. Se revisó ese estado (movido a la rama `antigravity/azahar-iteration`, sin mergear) y se retomó el trabajo limpio desde `wip/3ds-port-audio` (`d9ff3f8f`).

**Herramienta nueva: `tools/run_azahar_test.sh`** (sin commitear todavía). Automatiza el ciclo completo build→instalar→ejecutar→recoger evidencia contra Azahar (flatpak `org.azahar_emu.Azahar`) en esta máquina de desarrollo, sin depender de la 3DS real por FTP: compila, mata cualquier instancia previa, limpia `mzm-debug.log`/`mzm_audio_dump.bin` (se abren en modo append), instala el CIA en silencio (`azahar -i`, sin diálogo) y lo lanza directamente sobre el `.app` ya instalado bajo la SD virtual de Azahar (lanzar el `.cia` en crudo con `-f` sí dispara un diálogo interactivo de instalación aunque ya esté instalado — hay que evitarlo). Al final imprime el log, hace captura de pantalla y genera WAV+espectrograma del volcado de audio si `DumpRaw()` está activo.

**Se probó primero (y se descartó) el tuning de buffers NDSP de la sesión 6k** (`BUFFER_FRAMES` 256→224, `BUFFER_COUNT` 12→4, prebuffer de arranque, mantener última muestra en underrun, tasa 16364→13379 Hz) aplicándolo sobre `wip/3ds-port-audio` sin el resto de cambios de esa sesión (se dejó fuera el revertido del signo del PCM y el logging nuevo). Resultado confirmado por el usuario en su propia máquina: el petardeo seguía exactamente igual (incluso los "pitidos" de cada letra apareciendo en el texto de intro, que en el espectrograma se ven como ráfagas tonales limpias pero en realidad también son petardeo — fácil de confundir sin escuchar) y los FPS bajaron más (~10 en vez de los ~20 ya malos de antes). Revertido: quitar el colchón `RING_FLOOR` y reducir a un tercio el total de audio en buffer (de ~3072 a ~896 frames) empeora las cosas, no las mejora. Se mantiene la configuración original de buffers/tasa.

**Hallazgo importante — la "regresión de FPS a ~20" de la sección 6k estaba contaminada por un build incremental obsoleto:** al reconstruir `wip/3ds-port-audio` con los cambios seguros (ver más abajo) sin `make clean`, el binario seguía mostrando el mismo comportamiento lento (~10-11 FPS en Azahar) pese a que el Makefile ya NO llevaba `-DPORT_VERBOSE_FRAME_LOG=1` (esa definición sólo estuvo puesta durante la sesión 6k, nunca se trajo a este commit). Motivo: `port_bios.c`/`agbmain.c` no habían cambiado de fuente entre builds, así que `make` no los recompiló y sus `.o` seguían siendo los de cuando el flag SÍ estaba activo — con eso, `Port_Bios_Halt()` seguía escribiendo 3 líneas de log por frame a la SD virtual (`port/port_bios.c:232,255,259`, gateadas por `PORT_VERBOSE_FRAME_LOG`, ver sección 6k), y ese I/O de fichero por frame es muy caro bajo emulación. Un `make clean && make` limpio hizo que los FPS subieran de ~10-11 a **~29** en Azahar, en el mismo commit, sin ningún otro cambio. Esto también explica lo que recordaba el usuario de la sesión anterior ("quitar las funciones de logueo devolvió 60 FPS" — no era que el logging en sí fuera necesariamente el problema real en hardware, sino que borrar físicamente las líneas de log fuerza la recompilación de esos ficheros y así sí se libra del `.o` obsoleto). **Conclusión: la cifra de "~20 FPS" documentada en 6k para hardware real puede estar limpia (el usuario probaba con CIAs recién compilados por FTP, no con builds incrementales locales), pero cualquier medición de FPS en este entorno de desarrollo local a partir de ahora debe hacerse SIEMPRE con `make clean` de por medio para no arrastrar este artefacto.** Sigue sin confirmarse si, con un build limpio de verdad, la regresión de FPS en HARDWARE REAL (no Azahar) sigue existiendo o era en parte el mismo espejismo.

**Nuevo crash real encontrado (sin diagnosticar del todo):** dejando correr la intro más tiempo de lo que había durado ninguna prueba anterior (las pruebas previas cortaban a los 15-40s), Azahar mostró una excepción antes de que terminara la intro:
```
r00=00000000 r01=00267458 r02=00000001 r03=00000000
r04=000003A2 r05=00261188 r06=00265ACC r07=00263994
r08=00000000 r09=0025B800 r10=00000004 r11=00000000
r12=FFFFFF80 r13(sp)=08007CD8 r14(lr)=001AA9E0 r15(pc)=0025B478
ExceptionRaised(exception = UndefinedInstruction, pc = 0025B478)
```
- `sp=0x08007CD8` cae otra vez dentro del rango de direcciones GBA ROM `[0x08000000, ...)` — el mismo fenómeno de la pila nativa de la 3DS solapando ese rango numérico ya documentado en la sección 6i (ahí era `0x08007b28`, aquí `0x08007CD8`, ambos en la misma región de pila del hilo principal).
- `pc=0x0025B478` **cae dentro de `.rodata` (0x00256000–0x0025DF20 según `readelf -S`), no dentro de `.text` (que empieza en 0x00100000)** — es decir, la ejecución saltó a datos de solo lectura y la CPU los rechazó como instrucción inválida. `addr2line` lo resuelve como `dtoa.c` (una tabla/símbolo de las rutinas de conversión de punto flotante de newlib) pero eso es solo el símbolo de depuración más cercano en esa zona de datos — casi seguro NO se estaba ejecutando dtoa de verdad, sino que algo saltó a esa dirección por error.
- `lr=0x001AA9E0` resuelve a `port_resolve_addr` (`port/port_gba_mem.c:322`, dentro del chequeo `IsActiveStackPtr`) — encaja con la teoría de que algo llamó a través de un puntero devuelto por `port_resolve_addr()`/similar que apuntaba, por la razón que sea, a `.rodata` en vez de a código real. Podría ser un puntero a función leído de una tabla de datos sin traducir (mismo patrón que los bugs de audio de la sección 6k, pero en otra parte del motor, todavía sin localizar qué tabla/campo es) o corrupción de pila cerca de ese `sp` aliasado con GBA ROM.
- **Sin diagnosticar más a fondo esta sesión** por tiempo. Próximo paso: reproducir con `tools/run_azahar_test.sh` dejando correr más tiempo (60s+), capturar el `mzm-debug.log` completo hasta el crash para ver el último evento normal antes de la excepción, y buscar con el script de auditoría `tools/audit_rom_pointer_fields.py` posibles punteros a función sin resolver cerca del código de intro (`src/intro.c`, ya que el crash pasa "antes de acabar la intro").

## 6m. Root cause real del crash de la intro Y de gran parte de la regresión de FPS: `lr` pisado por `bl port_resolve_addr` (2026-08-19)

Siguiendo la pista de la sección 6l con logging temporal (añadido y luego quitado, no queda en el código): se instrumentó cada lectura de byte de comando de música (`var_0 = *pVariables->pRawData` en `UpdateTrack`, `src/audio.c`) y cada llamada a través de `sMusicCommandFunctionPointers[]`. El crash es 100% determinista — mismos registros exactos en cada repetición. La secuencia final antes de crashear, para la pista de música que crashea: comando `0xB4` (PEND, sin patrón activo que recuperar — no-op válido) seguido inmediatamente de `0xB3` (PATT, `AudioCommand_PatternPlay`, dirección de función confirmada válida `0x100731`). Justo después de esa llamada, Azahar reporta `ExceptionRaised(exception = NoExecuteFault, pc = 00000000)` — la CPU intentó EJECUTAR en la dirección 0.

**Causa encontrada en `asm/audio_internal.s`:** tanto `AudioCommand_Goto` (comando GOTO) como `AudioCommand_PatternPlay` (comando PATT) son funciones hoja del motor original de GBA — se llaman con `bl` (lo que pone la dirección de retorno en `lr`) y vuelven con un simple `bx lr` al final, porque el código GBA original nunca llamaba a nada internamente (nunca ponía en riesgo `lr`). El fix de traducción de punteros del port (documentado en secciones anteriores) añadió una llamada `bl port_resolve_addr` en medio de cada una de estas dos funciones para traducir la dirección cruda de GBA leída de los datos de la pista (el destino del GOTO/patrón) — **pero nunca guardó el `lr` de entrada antes de esa llamada**. Toda instrucción `bl` sobrescribe `lr` como efecto colateral (apuntando de vuelta a sí misma, no al llamador real). Como resultado, el `bx lr` final de estas dos funciones no vuelve a quien las llamó (normalmente a través de `sMusicCommandFunctionPointers[]` en `UpdateTrack`), sino a un punto intermedio de sí mismas o a lo que quedara en `lr` de una llamada GOTO/PATT anterior — comportamiento indefinido, que eventualmente salta a una dirección de basura (incluida `0x00000000`, produciendo el `NoExecuteFault` observado).

**Fix:** guardar/restaurar `lr` alrededor de la llamada `bl port_resolve_addr` en ambas funciones. No se puede hacer `pop {r0,lr}` directamente en la codificación Thumb usada en este fichero (el ensamblador da error `cannot honor width suffix`), así que se pasa por un registro callee-saved libre (`r4`, sin uso en ninguna de las dos funciones) como intermediario.

**Verificado con `tools/run_azahar_test.sh`:** antes del fix, crash determinista al poco de empezar la intro en el 100% de las repeticiones (confirmado en decenas de runs). Después del fix: **120+ segundos sin crashear, pasando de largo la intro hasta gameplay real** (captura confirmada: Samus en una sala de cuevas, HUD correcto). Los FPS en Azahar subieron de los ~20-29 de las secciones 6k/6l a **~55 estables** — indicio fuerte de que la "regresión de FPS" documentada en 6k no era (solo) por mover el hilo de audio a Core 1, sino en gran parte este mismo bug corrompiendo el flujo de control cada vez que una pista de música ejecutaba GOTO o PATT (algo que pasa constantemente en música real con patrones/bucles, coincidiendo justo con cuando el audio dejó de sonar en silencio en la sección 6k).

**El petardeo NO se ha arreglado con esto** — el espectrograma tras el fix sigue mostrando ruido de banda ancha continuo, igual que antes. Es un bug distinto, en los datos de audio en sí, no en el flujo de control. Sigue abierto.

**Todavía sin hacer en esta sesión:** auditar si hay más funciones con el mismo patrón (`bl port_resolve_addr`/`bl port_resolve_write_addr` insertado en una función hoja que vuelve con `bx lr` sin guardar `lr`) en el resto del motor de audio o en otras partes del port — de los 5 sitios totales que llaman a `port_resolve_addr` en `asm/audio_internal.s`, 3 están dentro de `InitTrack` (que sí guarda `lr` correctamente en su prólogo) y los 2 restantes eran exactamente los arreglados aquí, así que en `audio_internal.s` no queda ninguno pendiente — pero no se ha revisado `asm/soundcode.s` ni otros ficheros asm del proyecto con el mismo criterio.

## 6n. Continuando la caza del petardeo: auditoría de `soundcode.s` y giro importante en el diagnóstico (2026-08-19)

Tras el fix de la sección 6m (`git push` hecho, rama `wip/3ds-port-audio` actualizada en origin), se continuó sin el usuario delante (petición explícita: "continua tu solo").

**Dos punteros de muestra más resueltos** en `unk_4e10` (`asm/audio_internal.s`, procesamiento de inicio de nota): el mismo patrón de `pVoice->pSample` sin `GBA_RESOLVE` que ya se había arreglado en `AudioCommand_Voice` (sección 6k punto 2), pero alcanzado desde otro camino (nota-encendida en vez de selección de voz). `lr` no corría peligro aquí (esta función sí guarda `lr` correctamente en pila desde el principio, a diferencia del bug de 6m). Sin cambio audible/visible en una prueba de 90s — puede que esta ruta de código no se ejercite con la música probada.

**Auditoría de `asm/soundcode.s`** (el mezclador final — `CallSoundCodeA`/`B`/`C`): se revisó función por función buscando el mismo patrón de puntero sin resolver o `lr` corrompido.
- `sub_08004ad8`/`sub_08004adc`: manipula `REG_DMA1_CNT` directamente vía punteros crudos (`sDma1ControlPointer`/`sDma1ControlValue`, direcciones de hardware GBA literales `0x040000xx`, NUNCA traducidas a `gIoMem` como sí hace el resto del port vía las macros `WRITE_16`/`WRITE_32`) — sería un bug real si se ejecutara, pero **no lo llama nadie en todo el proyecto** (grep confirma cero referencias fuera de su propia definición). Código muerto, descartado como causa.
- `SoundCodeA` (el mezclador de canales Direct Sound con interpolación lineal de muestras): usa `pChannel->pData`/`pChannel->pSize`, que se calculan en `asm/audio_internal.s:713-718` como **aritmética de punteros sobre `pSample1` ya resuelto** (`pData = pSample1 + 0x10`, `pSize = pSample1 + 0xC`) — correcto, no necesitan resolución propia mientras `pSample1` ya lo esté (confirmado: solo hay un sitio que escribe `TrackVariables.pSample1` aparte del ya arreglado en 6k/aquí, y ambos ya resuelven).
- El copiado a IWRAM de `CallSoundCodeA/B/C` (optimización de velocidad de GBA real, `DMA3_COPY_16` en `InitializeAudio`, `src/audio_wrappers.c:38-50`) ya está correctamente deshabilitado para `TMC_3DS`/`PORT_NATIVE` desde antes de esta sesión — no es la causa.

**Giro importante: se generó una forma de onda (no solo espectrograma) de un fragmento de música real** (`ffmpeg -lavfi showwavespic`, 2s en torno al segundo 45 de una captura de gameplay) — **la señal se ve claramente periódica y musicalmente coherente** (una nota/acorde con lo que parece vibrato o batido entre voces, envolvente de amplitud normal), **no como ruido/basura de memoria corrupta**. Esto contradice la hipótesis de trabajo de las secciones 6k-6m ("hay punteros sin resolver leyendo memoria equivocada como si fueran muestras de audio") como explicación PRINCIPAL del petardeo — si esa fuera la causa dominante, se esperaría ver una forma de onda mucho más caótica/aleatoria, no esta.

**Hipótesis revisada para la siguiente sesión (ninguna confirmada todavía):**
1. Un bug sutil de aritmética/escala en el propio mezclador de interpolación (`SoundCodeA`/`SoundCodeB`) — posible desbordamiento o saturación incorrecta al sumar varios canales, en vez de un problema de punteros. Requeriría comparar salida muestra-a-muestra contra una referencia conocida (ej. `mgba` corriendo la ROM real), no solo inspección visual.
2. Diferencia de fidelidad inherente entre la salida analógica real de GBA (que filtra/suaviza el PCM crudo de 8 bits vía DMA a través de su DAC y altavoz) y la reproducción digital limpia vía NDSP en 3DS — un problema conocido en emuladores que reproducen el stream DMA crudo sin aplicar el mismo filtrado de reconstrucción que el hardware real. Si es esto, no sería un "bug" del port sino que necesitaría un filtro de suavizado deliberado en el puente NDSP.
3. Underruns/discontinuidades en los límites de buffer del puente NDSP — se descartó parcialmente en 6l (el log muestra `toRead=256/256` la gran mayoría del tiempo, pocos underruns), pero no se ha medido con precisión cuánto contribuyen los pocos que sí ocurren.

## 6o. Diagnóstico cuantitativo del petardeo: sí es real, y está anclado a los límites del bloque de mezcla de 432 muestras (2026-08-19)

Siguiendo el hallazgo de 6n (la forma de onda se ve musicalmente coherente), se hizo la comparación muestra-a-muestra recomendada contra una referencia real: `/tmp/mgba_ref.wav` (grabación de `mgba-qt`, emulador GBA preciso, de una sesión anterior — 23s, 48kHz estéreo s16) contra el volcado del port ya decodificado correctamente como `s8` (`/tmp/azahar_dump_s8.wav`, sección 6n). Un detector simple de "saltos" (diferencia absoluta entre muestras consecutivas del canal izquierdo, en % de la escala completa) da:

```
mgba (referencia):   0 saltos >50% de escala en 1.11M muestras (0.000%)
port (3DS, s8):     739 saltos >50% de escala en 1.05M muestras (0.070%)
```

**El petardeo es real y medible** — la referencia tiene CERO discontinuidades grandes en todo el clip; el port tiene 739. Esto confirma que no es (solo) un efecto de que "la música con muchos canales se ve ruidosa en espectrograma" (aunque esa parte de 6n sigue siendo cierta y explica por qué el espectrograma solo no es buen diagnóstico).

**Las posiciones de esos saltos NO son aleatorias:** al mirar el resto (posición módulo N) para varios tamaños de bloque candidatos, aparece un pico muy claro en **módulo 432** — exactamente el tamaño de bloque de mezcla del motor (`n = count*4 = 432`, visible en los logs `mzmAudio: rate=13379 n=432 ...`). Los restos 383 y 431 (es decir, justo en o cerca del ÚLTIMO byte de cada bloque de 432) concentran ~1355 de los 5724 saltos totales (>25% de escala) — muchas más veces de lo esperado por azar (~13 por resto). Esto apunta a una discontinuidad sistemática en el límite entre bloques de mezcla consecutivos, no a ruido disperso.

**Se instrumentó temporalmente `Port_MzmAudio_SoundCodeC` (`port/port_mzm_audio_glue.c`, revertido después, no queda en el código)** para comprobar si el motor escribe a direcciones `dest` diferentes/discontinuas entre llamadas consecutivas (lo que habría implicado un bug de "pegado" en el puente de captura). Resultado: **`dest` es SIEMPRE la misma dirección física en cada llamada** (`gap=0` en decenas de muestras consecutivas) — es el mismo buffer fijo y reutilizado en cada tick, tal como se espera del modelo de doble buffer de hardware real (DMA consume mientras la CPU rellena el mismo buffer una y otra vez). **Esto descarta que el bug esté en cómo el puente del port concatena los bloques capturados** (direcciones consistentes, sin huecos ni solapes de dirección) — la discontinuidad debe originarse o bien dentro del propio cálculo de mezcla del motor entre llamadas (algún estado de fase/oscilador no se traslada correctamente de una llamada a la siguiente, un bug de fidelidad de la decompilación) o bien en CUÁNDO se invoca `SoundCodeC` respecto a cuándo "debería" avanzar el audio en tiempo real (en hardware real, un DMA con reloj propio consume el buffer a ritmo constante independientemente de la CPU; en el port no hay tal reloj de DMA real — si el motor se invoca a una cadencia ligeramente distinta de la que su propio diseño de doble buffer asume, se perdería o repetiría contenido justo en cada límite de bloque).

**No se ha aplicado ningún cambio de código a partir de este hallazgo** — es un diagnóstico, no un fix. Sigue siendo la pista más prometedora para la siguiente sesión: revisar qué determina exactamente cuándo se llama a `SoundCodeC`/`UpdateAudio` cada frame (`src/agbmain.c`, cerca de `UpdateAudio()`) y si eso se corresponde con lo que el motor original asumía sobre el ritmo de consumo del DMA de hardware real.

## 6p. Crossfade probado y descartado; localización más precisa del salto dentro del bloque (2026-08-19)

Continuando desde 6o, se probó un fix de bajo riesgo: un crossfade lineal corto (8 muestras) en `Port_MzmAudio_SoundCodeC` (`port/port_mzm_audio_glue.c`) mezclando las últimas muestras del bloque de 432 anterior con las primeras del nuevo, sin tocar tamaños de buffer/tasa/motor. **Se probó y se descartó**: simulado offline en Python sobre un volcado real (para no repetir el error de medir con datos contaminados por I/O — ver más abajo), el conteo de saltos grandes NO mejora (739→740, prácticamente igual). El código del experimento se revirtió (`git checkout` sobre el fichero), no queda en el repo.

**Aviso para la próxima sesión — trampa de medición encontrada:** un primer intento de verificar el crossfade en vivo (volcando las muestras ya mezcladas a un segundo fichero en la SD, con `fopen`/`fclose` por cada muestra individual — 432 veces por frame) provocó **FPS 0** de inmediato. No es una regresión del propio fix, es que abrir/cerrar un fichero en la SD 432 veces por frame es brutalmente caro. Cualquier instrumentación de diagnóstico en el motor de audio debe agrupar la E/S (una sola apertura por llamada, como ya hace `DumpRaw()`), nunca por muestra.

**Localización más precisa del salto:** en vez de mirar solo "módulo 432", se miró en qué DESPLAZAMIENTO dentro de cada bloque de 432 muestras se concentran los saltos grandes. No es el límite del bloque (desplazamiento 431) el que domina — es el **desplazamiento 383** (283 de ~739 saltos totales, ~38%), muy por delante de cualquier otro desplazamiento incluido el 431 (145 veces). 431-383 = 48. Esto sugiere que la discontinuidad no está en el "pegado" entre bloques de 432 (ya descartado en 6o al confirmar que `dest` es constante), sino en un punto FIJO dentro del propio cálculo interno de `SoundCodeC` — posiblemente relacionado con `samplesPerFrame`/`gMusicInfo.unk_C`/`unk_E` (las constantes de `SetupSoundTransfer`, sección 6, que gobiernan cada cuántas muestras el motor original re-sincronizaría con el DMA/interrupción de hardware real cada vblank) en vez de con el tamaño del lote de esta llamada concreta. No se ha confirmado el valor exacto de estas constantes en tiempo de ejecución ni se ha localizado la línea de asm exacta responsable — siguiente paso recomendado: loguear `gMusicInfo.unk_C`/`unk_D`/`unk_E`/`unk_10` una vez al arrancar el audio, y comparar 384 (=432-48) y 48 contra esos valores; si coincide con `samplesPerFrame` o similar, buscar en `asm/soundcode.s` qué pasa exactamente en la muestra 383-384 de cada llamada a `SoundCodeC` (probablemente un punto donde el bucle de mezcla revisa/actualiza algo por-frame en medio de un lote que ahora es más grande que un frame).

## 6q. Causa raíz más probable encontrada: `DMA2IntrCode()` nunca se invoca en el port (2026-08-19)

Siguiendo la pista de 6p (salto concentrado en el desplazamiento 383/384 dentro de cada bloque, no en el límite de 432), se rastreó el origen exacto de `var_6`/`var_3`/`var_4` — los que determinan cuántas muestras mezcla cada llamada a `SoundCodeC`/`SoundCodeB` — hasta el principio de `UpdateMusic()` (`src/audio.c:32-116`).

**El mecanismo real (línea 66-116):** `var_8 = gMusicInfo.unk_10` (la posición de "lo que el hardware ya ha consumido", en unidades del ciclo del DMA) y `var_7` (derivado de `unk_10`/`unk_C`) definen, comparados contra `gMusicInfo.unk_11` (la posición de "lo que ya hemos producido"), cuánto hay que mezclar en esta llamada para "ponerse al día" con el consumo real. Si el hueco entre producción y consumo cruza el final físico del buffer, la mezcla se parte en dos llamadas: `var_6` muestras hasta el final del buffer + `var_3` muestras desde el principio (esto es exactamente el "wrap: segunda llamada con dest=soundRawData[0]" que ya documentaba el comentario del wrapper, confirmado ahora con el código fuente exacto, `src/audio.c:122-129` para `SoundCodeB` y `:283-290` para `SoundCodeC`).

**El problema:** `gMusicInfo.unk_10` solo se actualiza en un sitio de todo el código: `DMA2IntrCode()` (`src/music_wrappers.c:17`), la rutina de interrupción que en hardware real dispara el propio DMA2 cuando termina de transmitir un semi-buffer completo a FIFO_B (`REG_DMA2_CNT` se configura con `DMA_INTR_ENABLE` en `SetupSoundTransfer`, sección 6/6q). **Esta interrupción nunca se invoca en el port** — no hay hardware DMA real en 3DS que la dispare, y no hay ningún sustituto que la llame periódicamente (confirmado por grep: el único lugar que la referencia fuera de su propia definición es la tabla de vectores de interrupción `src/data/generic_data.c:352`, nunca despachada). Esto significa que `gMusicInfo.unk_10` se queda **congelado en su valor inicial** (`unk_E - 1`, calculado una vez en `SetupSoundTransfer`) durante toda la partida — el motor de audio del port cree permanentemente que "el hardware ha consumido X" cuando en realidad esa cifra nunca se mueve, y toda la aritmética de `UpdateMusic()` que decide cuánto mezclar cada llamada queda desacoplada de cuánto se ha reproducido realmente.

**La coincidencia que lo confirma:** `PCM_DMA_BUF_SIZE` (`include/constants/audio.h`) es 1536 bytes; el DMA real transfiere en palabras de 4 bytes (`DMA_32BIT`), así que un semi-buffer completo son **1536/4 = 384 transferencias** — el punto exacto en el que, en hardware real, `DMA2IntrCode()` debería dispararse y `unk_10` debería avanzar. Es precisamente el desplazamiento (383/384) donde se concentran los saltos grandes de audio encontrados en 6o/6p. No parece casualidad.

**No se ha aplicado ningún fix todavía** — arreglar esto bien requiere decidir CÓMO simular el avance de `unk_10` sin hardware DMA real: la opción más fiel sería invocar `DMA2IntrCode()` cada vez que se hayan consumido 384 muestras reales (ligado al progreso real del anillo NDSP, `sRingReadIndex`, no a un contador de tiempo simulado), pero esto toca el corazón del sistema de temporización de audio y un error aquí podría ser tan malo o peor que el intento de tuning de buffers de la sección 6l. Dado que no se puede verificar el resultado por oído en este entorno, **se documenta como el hallazgo más prometedor pero no se ha tocado código** — la relación exacta entre "muestras realmente reproducidas" y "cuántas veces incrementar `unk_10`" necesita confirmarse con cuidado (probablemente 1 incremento de `unk_10` por cada `unk_C`=14 muestras reales consumidas, o por cada semi-buffer de 384/`unk_C`≈27 incrementos — sin confirmar) antes de tocar `UpdateMusic()`/`DMA2IntrCode()`/el puente NDSP.

**Siguiente paso recomendado para quien continúe:** llamar a `DMA2IntrCode()` desde el hilo consumidor de NDSP (`platform/3ds/source/port_mzm_audio_3ds.c`) o desde `Port_MzmAudio_Pump()`, basado en cuántas muestras ha consumido REALMENTE el anillo desde la última llamada (no en tiempo simulado ni en cuántas se han producido), y verificar con el detector de saltos de 6o/6p si el conteo de discontinuidades baja significativamente. Probar SIEMPRE con `make clean` de por medio (ver 6l) y comparar con `tools/audit_rom_pointer_fields.py`/el script de clics, no solo de oído.

## 7. Pendientes y Siguientes Pasos

**Bugs abiertos pendientes de investigar (reportados por el usuario, aún SIN resolver):**
- **Audio: petardeo residual (rama `wip/3ds-port-audio`, 2026-08-19) — CAUSA RAÍZ PROBABLE ENCONTRADA, sin arreglar todavía.** Ver secciones 6k-6q. Seis bugs reales de audio ya arreglados y confirmados en hardware/Azahar (silencio→sonido real), pero el sonido sigue con petardeo. Descartado por el camino: tamaño/cantidad de buffers NDSP (6l), el bug de `lr` corrompido en GOTO/PATT (6m, arreglado pero no cambia el audio), formato de decodificación del script de diagnóstico (corregido a `s8`), varios puntos de `asm/soundcode.s` (6n), que el puente de captura concatene mal direcciones (6o), y un crossfade en los límites de bloque (6p). **Sección 6q: `gMusicInfo.unk_10` (la posición de "cuánto ha consumido ya el DMA real") solo se actualiza en `DMA2IntrCode()`, que nunca se invoca en el port (no hay hardware DMA2 real en 3DS) — se queda congelada toda la partida, desacoplando por completo la aritmética de `UpdateMusic()` (`src/audio.c:32-116`) de cuánto se ha reproducido de verdad.** `PCM_DMA_BUF_SIZE/4 = 384` coincide con precisión con el desplazamiento dominante de los saltos de audio encontrado en 6o/6p — probablemente no es casualidad. **Sin arreglar deliberadamente**: toca el corazón de la temporización de audio, con riesgo real de regresión si se hace mal (como pasó con el tuning de buffers de 6l) y sin forma de verificar por oído en este entorno. Siguiente paso recomendado: invocar `DMA2IntrCode()` desde el hilo/pump de NDSP basándose en cuánto ha consumido REALMENTE el anillo (`sRingReadIndex`), no en tiempo simulado, y verificar con el detector de saltos de 6o/6p si el conteo baja. Probar siempre con `make clean` de por medio (6l).
- **RESUELTO (2026-08-19, sección 6m): el crash determinista de la intro y gran parte de la regresión de FPS.** Era el bug de `lr` corrompido: `bl port_resolve_addr` insertado en `AudioCommand_Goto`/`AudioCommand_PatternPlay` (`asm/audio_internal.s`) sin guardar `lr`, así que su `bx lr` final volvía a un sitio erróneo cada vez que una pista de música ejecutaba GOTO o PATT. Con el fix: 120s+ sin crashear, llega a gameplay real, FPS en Azahar sube de ~20-29 a ~55. Sigue pendiente confirmar el número de FPS en hardware real (no solo Azahar) ahora que este bug está arreglado — sería el siguiente paso lógico antes de seguir optimizando rendimiento.
- **Bug fuera de audio encontrado por `tools/audit_rom_pointer_fields.py` sin arreglar:** `src/animated_graphics.c:307` copia `pData->pGraphics` de `sAnimatedGraphicsEntries` (confirmado resuelto desde ROM vía `port/generated/animated_graphics_data_rom.c`) sin `GBA_RESOLVE` — mismo patrón que los bugs de audio, afectaría a gráficos animados (agua, lava, tiles parpadeantes).
- **Deorem (el "monstruo del techo" de la sala del primer tanque de misiles, Brinstar sala 12) desaparece a los pocos frames de empezar a bajar del techo** (2026-08-18, EN INVESTIGACIÓN ACTIVA, sin resolver tras ~10 iteraciones de diagnóstico en hardware real). **No es un jefe "Ruins Test"** (esa sala es de Chozodia, mucho más tarde en el juego) — es el enemigo Deorem, ya presente en la sala desde el inicio. Su ataque se dispara correctamente (`DeoremWaitingForFight`, `deorem.c:539`, confirmado con `maxMissiles != 0` + posición de Samus) y comienza a bajar con normalidad (visible, sano, `SPRITE_STATUS_EXISTS`), pero entre el 3er y el 8º frame de la caída el sprite desaparece por completo del array de sprites SIN pasar por ninguna pose de muerte, SIN crash y SIN colgar el resto del juego. Se ha descartado `gPauseScreenFlag`, `gPreventMovementTimer`, un cuelgue del hilo principal, y el audio/música (con un build de diagnóstico que desactivaba `SoundPlay`/`PlayMusic` por completo — desapareció igualmente). El frame exacto de desaparición varía entre partidas, lo que apunta a una condición de carrera entre hilos en vez de un bug determinista. Ver diagnóstico completo, hipótesis descartadas e instrumentación dejada en el código en la sección 6j.
- **Franja vertical "gelatina" desincronizada a la derecha** (bug 3): franja del ancho de la vista previa del minimapa en el borde derecho que se desincroniza al subir/bajar la cámara. Pendiente de investigar (sospecha de scroll de un BG / window).

**Resueltos en esta sesión (2026-08-18):** guardado (6i), indicadores Chozo (6i, efecto secundario), tanque de misiles (6j).

**Audio nativo conectado (2026-08-18, rama `wip/3ds-port-audio`):** en esta rama se ha montado el sonido real del port usando el motor propio de mzm (NO M4A/TMC):
- El motor real de mzm (`asm/audio_internal.s` + `asm/soundcode.s`) se enlaza de nuevo en el build 3DS (`platform/3ds/Makefile`: SRCS_ASM), se quitó `PORT_NATIVE_AUDIO_STUBS` y `port/port_audio_stubs.c` quedó fuera del build 3DS. `UpdateAudio`/`UpdateMusic`/`CallSoundCodeA/B/C` vuelven a estar activos y mezclan en `gMusicInfo.soundRawData` cada frame.
- Se escribió el "pegamento" NDSP desde cero: `port/port_mzm_audio_glue.{c,h}` (productor, hilo principal: convierte el PCM mono u8 de `soundRawData` a s16 estéreo, resamplea a 16364 Hz y lo encola en un ring lock-free) + `platform/3ds/source/port_mzm_audio_3ds.c` (consumidor NDSP con hilo de audio que drena el ring al canal 0). El puente se invoca desde `src/agbmain.c` justo después de `UpdateAudio()` (guard `#if defined(TMC_3DS) && defined(__3DS__)`, no afecta a Linux) y el init desde `platform/3ds/source/main_3ds.c` antes de `agbmain()`.
- **Verificación de audio: se trasladó de hardware real a Azahar** (emulador 3DS en PC). Se copió `dspfirm.cdc` del FTP real a la SD virtual de Azahar para que `ndspInit()` funcione (`Port_MzmAudio_Init done`). El consumidor NDSP drena sin underruns (`toRead=256/256`, rate 13379).
- **Diagnóstico de esta entrada (2026-08-18) SUPERADO — ver sección 6k para la sesión completa que lo resolvió (2026-08-19).** El silencio total (`pRawData=0x0`, `wholePeak=0`) era el bug de `InitTrack` no resolviendo `pHeader` (sección 6k, punto 1). Tras arreglarlo y cinco bugs más, el audio ya suena (no silencio), pero queda petardeo sin terminar de limpiar y una regresión de rendimiento nueva — ambos sin resolver, ver el final de la sección 6k.
- Riesgos a validar más adelante: (a) tasa/desfase del resampleo (motor 13379 Hz vs NDSP 16364 Hz), (b) la lectura de `soundRawData` (el wrapper lee `dest` dentro de la propia llamada a `SoundCodeC`, por lo que la posición es exacta), (c) el crash de `TrackVariables` de la sección 6 (el fix de `port_resolve_addr` está en el asm pero sigue sin confirmarse), y (d) la sincronía de los buffers NDSP (subflow/underruns). Requisito previo en consola real: **Dump DSP firmware** en Luma3DS Rosalina para que `ndspInit()` funcione.

**Siguientes tareas recomendadas:**

1. **Audio nativo:**
   - **Corrección (2026-08-18): ya existe un backend de audio real**, no es solo el stub. `port_audio_stubs.c` stubea únicamente los hooks de bajo nivel del engine original (`InitTrack`, `CallSoundCodeA/B/C`, etc. — no hacen nada). Pero hay una implementación nativa completa aparte: `platform/3ds/source/port_audio_3ds.c` (hilo de audio real con NDSP, mutexes, gestión de buffers) + `port/port_m4a_backend.cpp` (motor M4A/MP2K en C++ con locks propios) + `port/port_m4a_stubs.c` (traduce las llamadas del juego al backend). Falta verificar en hardware si la música/sonido ya suena correctamente en general (no se ha comprobado en esta sesión más allá de descartarlo como causa del bug de Deorem); si no suena, el problema probablemente esté en el "pegamento" entre `music_wrappers.c`/`audio_wrappers.c` (que siguen llamando a los hooks stubeados) y este backend nuevo, no en el backend en sí.
2. **Opciones de pantalla / Aspect Ratio y Segunda Pantalla:**
   - Añadir en el menú opciones de presentación de pantalla (1:1 Pixel Perfect, 1.5x Linear Filtered, Pantalla Completa).
   - Aprovechar la pantalla táctil inferior para mostrar el minimapa, estado de Samus o controles táctiles.
3. **Soporte Multi-Región:**
   - Generar offsets y compatibilidad completa para ROMs US (BMXE) y JP además de la versión EU (BMXP).
4. **NES Metroid integrado:**
   - Portar el emulador de NES interno (`nes_metroid/`) a nativo.


## 8. Cómo desplegar y depurar en 3DS real (flujo que se usó toda la sesión)

1. Compilar:
   ```
   cd platform/3ds
   export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
   export PATH=$DEVKITARM/bin:$HOME/tools/bin:$PATH
   make
   ```
   (`~/tools/bin` tiene `makerom`/`bannertool`, no vienen con devkitPro
   oficial.)
2. Subir el CIA por FTP (FBI corriendo en la consola, puerto 5000 — la IP
   de la 3DS en la LAN del usuario cambia entre sesiones, se ha visto tanto
   `192.168.1.133` como `.138`; probar ambas si una falla):
   ```
   curl --ftp-method nocwd -T mzm-3ds.cia ftp://<IP>:5000/cias/mzm-zm.cia
   ```
3. Limpiar el log antes de la siguiente prueba (opcional pero recomendado,
   si no se acumula entre ejecuciones):
   ```
   curl "ftp://<IP>:5000/3ds/" -Q "DELE mzm-debug.log"
   ```
4. El usuario reinstala manualmente desde FBI (`sdmc:/cias/mzm-zm.cia`) y
   abre la app — esto no se puede automatizar desde aquí.
5. Leer el log de diagnóstico:
   ```
   curl "ftp://<IP>:5000/3ds/mzm-debug.log"
   ```
6. Si crashea con volcado de Luma3DS, están en
   `sdmc:/luma/dumps/arm11/crash_dump_NNNNNNNN.dmp` (numeración
   incremental, no se limpian solos — mirar el de número más alto tras cada
   prueba). Parsear con la estructura de la sección 5.5 (o el script
   Python ad-hoc usado en la sesión, no guardado como fichero pero
   reproducible a partir de esa estructura). `arm-none-eabi-addr2line -e
   mzm-3ds.elf -f -C <pc_o_lr>` resuelve la dirección a función/línea una
   vez extraída del volcado.
   `errdisp.txt` en `sdmc:/luma/errdisp.txt` da un resumen rápido (tipo de
   error, dirección) sin necesidad de parsear el binario, pero **esa
   "Address" es la dirección de fallo de datos (DFAR), no el PC** — para
   saber qué código estaba ejecutando hace falta el volcado completo.
