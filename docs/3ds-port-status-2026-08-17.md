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

## 7. Pendientes y Siguientes Pasos

**Siguientes tareas recomendadas:**

1. **Audio nativo:**
   - Implementar backend de audio para 3DS usando el sintetizador de sonido / pistas de MZM (actualmente stubeado en `port_audio_stubs.c`).
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
