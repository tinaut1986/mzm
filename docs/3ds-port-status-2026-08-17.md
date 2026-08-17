# Estado del port a 3DS — resumen y siguientes pasos (2026-08-17)

Documento de continuidad: qué se ha hecho, en qué estado real está el
proyecto ahora mismo, y por dónde seguir. El trabajo se ha hecho en dos
frentes en paralelo — esta sesión (Claude Code) y otra sesión con
"antigravity" — sobre el mismo fork `tinaut1986/mzm`
(rama `wip/3ds-port-no-audio-ppu`), y este documento cubre ambos.

## 0. Punto de partida

Objetivo: portar **Metroid Zero Mission** a Nintendo 3DS, siguiendo el
mismo patrón que `zelda-tmc-3ds` (port de Minish Cap a 3DS): partir de la
**decompilación matching** del juego (`metroidret/mzm`, no de ingeniería
inversa propia) y construir encima una capa de "port" que emule el hardware
GBA necesario sobre libctru/citro2d/citro3d.

## 1. Decompilación base — verificada

- Fork `tinaut1986/mzm` de `metroidret/mzm` (~99.89% decompilado, build
  matching).
- Compilación GBA verificada: el `.gba` resultante coincide byte a byte
  (SHA-1 `0fd107445a42e6f3a3e5ce8c865f412583179903`, versión EU) con la ROM
  original, usando tu ROM legal como baserom.
- Auditoría de modos de PPU (`docs/3ds-port-ppu-audit.md`): Zero Mission
  usa Modo 0 en casi todo el juego y Modo 1 (afín) solo en la secuencia de
  escape de Tourian. Confirma que el renderizador de `zelda-tmc-3ds` (que
  solo soporta Modo 0+1) es aplicable sin ampliar.

## 2. Arquitectura de assets: carga de ROM en runtime (sin copyright)

Decisión de arquitectura clave, tomada porque **quieres poder publicar el
port algún día**: el binario distribuido no debe llevar assets con
copyright horneados dentro. El usuario final pone su propia ROM en la SD y
el port la lee en cada arranque, traduciendo direcciones GBA a punteros
nativos — igual que hace `zelda-tmc-3ds`.

Esto es viable de forma más sencilla que en TMC porque `mzm` es un decomp
matching casi completo: hay una separación limpia entre `src/*.c` (67
ficheros de lógica pura) y `src/data/*.c` (~476 ficheros, solo datos,
compilados desde `extracted/*.inc` generados por `tools/extractor.py` a
partir de tu baserom). El plan: no compilar `src/data/*.c` para 3DS: en su
lugar, resolver esos símbolos en runtime desde la ROM cargada, usando las
direcciones GBA exactas que ya conocemos gracias al build matching (el
`.map` del linker da la dirección exacta de cada símbolo).

### Piezas implementadas

- **`port/port_rom.c` / `port/port_rom.h`**: carga de ROM, detección de
  región vía el game code del header (`BMXE`=US, `BMXP`=EU, `BMXJ`=JP,
  confirmado contra el `Makefile` de `mzm`), y `Port_ResolveRomData()` para
  traducir una dirección GBA a puntero nativo. Verificado contra la ROM EU
  real: los bytes resueltos coinciden con los datos ya extraídos.

- **`tools/gen_port_rom_offsets.py`**: generador que, por cada
  `include/data/*.h`, produce un `.h`/`.c` en `port/generated/` con:
  - un puntero `extern` + macro `#define nombre (*p_nombre)` que redirige
    cada símbolo de datos a `Port_ResolveRomData(offset)`,
  - una función `PortGen_<base>_Init()` que resuelve todos los punteros
    según la región activa, usando offsets sacados del `.map`.
  - **Cobertura verificada**: los 35 headers de `include/data/` parsean con
    0 líneas sin reconocer (2153 símbolos). Verificado end-to-end contra
    datos reales de la ROM, y compilado con el compilador ARM real contra
    los 178 ficheros de gameplay sin errores de tipos/símbolos.
  - **Bugs reales encontrados y corregidos durante el desarrollo** (todos
    solo visibles al probar a escala, no con un símbolo suelto):
    1. `#if defined()/#elif/#else` no se interpretaban como excluyentes →
       símbolos duplicados con tamaños distintos (p.ej. `sCredits` por
       región). Corregido con una pila de condiciones propia.
    2. Variables RAM mutables sin `const` (p.ej. `sNonGameplayRamPointer`
       en `shortcut_pointers.h`) se trataban como datos ROM, duplicando el
       nivel de puntero (`T*` → `T**`). Corregido: solo se redirige lo
       `const`-calificado; el resto se reproduce tal cual.
    3. Confusión entre "escalar sin dimensiones" (necesita desreferenciar)
       y "array sin tamaño" (no debe desreferenciar) — rompía
       `&sDemo0_Ram` en `src/demo.c`. Corregido con una clasificación
       explícita de tres vías (`scalar`/`flat`/`array`).
    4. Símbolos legítimamente declarados `extern` en dos headers distintos
       con una única definición real (p.ej. `sDoorTransitionTilemap`) —
       generaban doble definición real → error de enlazado. Corregido
       marcando la definición como `__attribute__((weak))`.

- **Sombreado de includes**: los headers generados se escriben en
  `port/generated/shadow/data/*.h`, con la **misma ruta relativa** que los
  originales (`data/*.h`). Anteponiendo ese directorio en el `-I` del
  compilador, cualquier `#include "data/*.h"` de `src/*.c` se resuelve
  hacia el generado **sin tocar ni una línea de la decompilación**.
  Verificado con la salida del preprocesador, no solo asumido.

- **`include/syscalls.h` también se sombrea** (`port/generated/shadow/syscalls.h`):
  solo cambia la macro `SYSCALL(num)`, ver sección 3.

## 3. BIOS de GBA reimplementada en C limpio

Hallazgo importante: `SYSCALL(num)` es literalmente `asm("svc " #num)` — una
llamada real a la BIOS de GBA. En 3DS esa misma instrucción `svc` no falla
al compilar, pero en hardware real dispara una syscall de Horizon OS
completamente distinta: **no es un error de compilación, es un
crash/corrupción silenciosa en la consola**.

Investigando el código real: casi todas las funciones "BIOS" (`Div`,
`Sqrt`, `CpuSet`, `LZ77UncompVram`, etc.) son funciones normales (`bl`), NO
`swi` directas — solo `asm/syscalls.s` las implementa con `swi`. El único
uso directo de `SYSCALL()` en todo el código está en 2 sitios
(`agbmain.c`, `pause_screen_sub_menus.c`), ambos como "esperar al siguiente
VBlank".

**`port/port_bios.c`**: reimplementación limpia (no copiada, algoritmos
públicos documentados en GBATek) de `DivarmDiv/Mod`, `Sqrt`, `CpuSet`,
`CpuFastSet`, `LZ77UncompVram/Wram`, `Multiboot` (stub), `SoundBias0/200`
(stub), `MidiKey2Freq` (aproximación placeholder), y `Port_Bios_Halt()`
(sustituye el `SYSCALL` real: espera al VBlank real vía `gspWaitForVBlank()`
+ invoca `CallbackCallVblank()`, el cuerpo real de la ISR de VBlank del
juego). `asm/syscalls.s` se excluye del build 3DS.

**Verificado con tests unitarios reales** (`port/port_bios_selftest.c`):
casos literal, back-reference solapada (LZSS) y de longitud mínima del
LZ77 — los tres pasan.

## 4. Copyright — hallazgo importante, corregido

Al revisar de cara a una futura publicación: `platform/3ds/assets/`
contenía **arte real de Samus** (banner + icono, con tipografía oficial del
logo "METROID") y, más sorprendente, un `icon.png`/`splash.png` que
resultaron ser **arte de Link/Zelda** — resto de cuando el proyecto se
inició copiando la estructura de `zelda-tmc-3ds`, ni siquiera del IP
correcto.

- **No hay ningún binario `.cia`/`.gba`/`.elf` commiteado en git** (el
  `.gitignore` los excluye explícitamente) — el código en sí no redistribuye
  la ROM ni assets extraídos, gracias a la arquitectura de la sección 2.
- **Corregido**: `banner.png` e `icon-48.png` (los dos realmente usados)
  sustituidos por arte genérico propio (tipografía simple, sin personajes).
  `icon.png`, `icon-24.png`, `splash.png` y `romfs/splash.rgb565`
  eliminados (no se usaban en el build actual, y tenían copyright).
- `banner.wav` se dejó tal cual: parece generado sintéticamente
  (metadata `libav`, 1 segundo), no extraído del juego.

## 5. El Makefile — dos intentos, mismo destino final

### Intento propio (esta sesión): plantilla clásica de devkitARM

Escribí un Makefile siguiendo el patrón recursivo de dos pasadas estándar
de devkitPro. **Bug encontrado y corregido en el propio mecanismo**: el
`VPATH` duplicaba el prefijo `$(CURDIR)/` sobre rutas que ya eran
absolutas, produciendo rutas rotas tipo `/a//b` — corregido.

Aun así, el build fallaba de forma extrañísima: solo 4 de 227 ficheros se
compilaban, siempre los mismos, sin importar el orden de `SOURCES`, tamaño
del build, ni paralelismo. Tras horas de aislar el problema con Makefiles
mínimos de prueba (que SÍ funcionaban a la misma escala), until: usando
`make --debug=v`, el log reveló la causa real:

```
Prerequisite '.../src/samus.c' is older than target 'samus.o'.
No need to remake target 'samus.o'; using VPATH name '.../src/samus.o'.
```

**Causa real**: quedaban **654 ficheros `.o` residuales** en `src/` y
`port/` de cuando compilé la ROM GBA al principio de la sesión (artefactos
de build, en `.gitignore`, pero físicamente presentes en disco). `VPATH`
los encontraba, los consideraba "ya actualizados", y no los recompilaba
para ARM11 — pero el linker (que no entiende `VPATH`) no los encontraba en
el directorio de build, porque físicamente estaban en `src/`. Solución:
`find src port -name "*.o" -delete`.

### Segunda versión (en paralelo, "antigravity"): Makefile propio desde cero

En paralelo, otra sesión reescribió el Makefile completo con reglas
explícitas (no la plantilla recursiva de devkitARM), evitando el problema
de `VPATH` de raíz. Añadió también:

- Target nativo Linux (`platform/linux/`) — útil para depurar sin
  necesitar hardware real.
- `platform/3ds/cia/mzm3ds.rsf` — configuración de `makerom` para generar
  el `.cia` (sin nada con copyright dentro, solo permisos/metadata
  estándar de homebrew).
- Resolución de varios punteros de motor en runtime (`port_gba_symbols.c`,
  ampliación de `gen_port_rom_offsets.py` para más categorías de datos:
  sprites, tilesets, cutscenes).
- Compila directamente 3 ficheros de `src/data/` (`shortcut_pointers.c`,
  `music_track_data.c`, `audio.c`) — revisado: **no contienen bytes
  extraídos de la ROM** (`extracted/`), solo punteros a RAM y metadatos de
  estructura, así que no reintroducen el problema de copyright.
- Modifica directamente varios ficheros de `src/*.c` (bajo
  `#ifdef TMC_3DS`, sin afectar al build GBA matching) para añadir tablas
  de punteros gráficos/paleta necesarias en 3DS que no existían ya como
  tales en el decomp.

**Este es el Makefile que se usa ahora mismo** (`platform/3ds/Makefile`,
reescrito). Necesita `makerom`/`bannertool` instalados aparte (no vienen en
devkitPro oficial) — instalados en `~/tools/bin/`.

## 6. Estado actual: compila e instala, pero se cuelga en el arranque real

El CIA se genera correctamente y se instala en la New 3DS. Al abrirlo:
llega a imprimir "Starting engine (no video/audio yet)..." (o sea,
`main_3ds.c` corre entero: init de servicios, carga de ROM, detección de
región) y luego **se queda completamente colgado — ni siquiera responde al
botón Start**.

### Diagnóstico en curso

1. **Primera hipótesis descartada por instrumentación, no por lógica**:
   sospechaba de `src/audio_wrappers.c:205-206`:
   ```c
   while (READ_8(REG_VCOUNT) == (SCREEN_SIZE_Y - 1)) {}
   while (READ_8(REG_VCOUNT) != (SCREEN_SIZE_Y - 1)) {}
   ```
   Un busy-wait sobre el registro hardware `VCOUNT` (línea de escaneo
   actual), que en GBA real avanza solo, 228 líneas por frame. Sin una PPU
   real actualizándolo, se queda fijo para siempre.

2. **Implementado `port/port_gba_timing.c`**: un hilo de 3DS dedicado
   (creado desde `platform_3ds_minimal.c`, que sí puede incluir `<3ds.h>`
   con seguridad) que actualiza `REG_VCOUNT`/`REG_DISPSTAT` a ritmo real de
   GBA (~73.35 µs por línea, 160 líneas visibles + 68 de vblank). **No es
   la PPU real** (no dibuja píxeles), solo desbloquea los busy-waits de
   hardware.

3. **El cuelgue persistió igual**, así que se necesitaba visibilidad real
   en vez de seguir adivinando. El usuario señaló acertadamente: si antes
   había *crashes* con volcado (probablemente generado por Luma3DS ante una
   excepción real de CPU), pero ahora es un *cuelgue* (bucle infinito sin
   excepción), Luma no genera volcado — hace falta instrumentación propia.

4. **Implementado `port/port_debug_log.c`/`.h`**: logger a fichero
   (`sdmc:/3ds/mzm-debug.log`), con `fopen`/`fflush`/`fclose` en cada
   línea para que sobreviva a un cuelgue. Añadidos puntos de control en
   `main_3ds.c` y (temporalmente, bajo `#ifdef TMC_3DS`, en
   `src/agbmain.c`) en el bucle principal del juego.

5. **Primera captura de log** (antes de añadir más puntos de control):
   ```
   main: start
   timing thread: started
   main: Platform3DS_Init done
   main: before Port_LoadRom
   main: Port_LoadRom done
   main: before agbmain()
   agbmain: before InitializeGame()
   agbmain: InitializeGame() done, entering main loop
   agbmain: loop iteration start
   timing thread: heartbeat (still running)   [x10, ~15s]
   ```
   **Conclusiones de este log**:
   - `InitializeGame()` termina bien.
   - Entra en el bucle principal, arranca la **primera** iteración... y
     nunca llega a la segunda (el checkpoint de "loop iteration start"
     solo aparece una vez, no las 5 veces que el código permite).
   - El hilo de VCOUNT **está vivo y funcionando** (los heartbeats siguen
     llegando mientras el hilo principal está colgado) — descarta que el
     busy-wait de VCOUNT sea la causa actual (el hilo de timing ya lo
     desbloquearía).
   - **Conclusión**: el cuelgue está en algún punto DENTRO de la primera
     iteración del bucle, después de "loop iteration start" pero antes de
     completarla.

6. **Instrumentación adicional ya desplegada** (commit pendiente de subir
   a git): puntos de control bisecando la primera iteración —
   antes/después de `UpdateAudio()`, `UpdateInput()`, `SoftResetCheck()`,
   el `switch(gMainGameMode)`, y el `Halt` final. CIA recompilado y subido
   a `sdmc:/cias/mzm-zm.cia`. **Pendiente**: que el usuario lo abra, lo
   deje colgado unos segundos, y se lea `sdmc:/3ds/mzm-debug.log` de nuevo
   para ver exactamente cuál es la última línea antes del cuelgue.

### Sospecha principal para la siguiente sesión

`UpdateAudio()` es la sospechosa más probable: el motor de audio real de
`mzm` (`UpdateMusic`/`TrackVariables`, distinto de M4A/Sappy — ver sección
7) no está conectado a NDSP todavía, y es exactamente donde vive el
busy-wait de VCOUNT que investigamos primero. Aunque el hilo de timing ya
debería desbloquear ESE busy-wait concreto, es muy posible que haya *otro*
punto de esa misma zona (acceso a hardware de sonido, DMA, u otro
busy-wait no identificado aún) que sí bloquee.

## 7. Motor de audio — hallazgo pendiente de abordar

`mzm` **no usa M4A/Sappy** (el motor estándar de Nintendo, que sí usa
`zelda-tmc-3ds`/TMC) — tiene su propio motor propio
(`UpdateMusic()`/`TrackVariables`/`TrackData` en `src/audio.c`). Los
ficheros `port_m4a_backend.cpp`/`port_m4a_stubs.c` copiados al principio de
`zelda-tmc-3ds` **no son reutilizables tal cual** — quedan excluidos del
build actual. El backend de audio real hay que escribirlo desde cero contra
el motor propio de Metroid. No bloqueante para ver algo en pantalla, pero
sí puede estar relacionado con el cuelgue actual si `UpdateAudio()`
resulta ser el punto exacto.

## 8. Lo que NO se ha tocado todavía (grande, pendiente)

- **Renderizado real (PPU→GPU)**: `port_ppu_3ds.c` de `zelda-tmc-3ds` sigue
  sin adaptar — está profundamente acoplado a subsistemas propios de TMC
  (pantalla inferior, HDMA, `gMain`/`gRoomControls` de TMC). Ahora mismo
  `platform_3ds_minimal.c` no dibuja nada, solo texto de consola.
- **Audio real**: ver sección 7.
- **Multi-región**: solo hay offsets EU generados (`mzm_eu.map`). Hacen
  falta baseroms US/JP para generar `mzm_us.map`/`mzm_jp.map` y completar
  la tabla.
- **`nes_metroid/`** (el modo NES Metroid embebido): es ensamblador GBA real
  a mano, nunca decompilado — fuera de alcance hasta tener un plan para
  traducir/ejecutar asm GBA real en ARM11.

## 9. Siguientes pasos concretos, en orden

1. **Leer `sdmc:/3ds/mzm-debug.log` tras el último despliegue** (el CIA con
   los puntos de control bisecando `UpdateAudio()`/`UpdateInput()`/
   `SoftResetCheck()`/switch/Halt ya está en `sdmc:/cias/mzm-zm.cia`).
   Identificar la función exacta donde se cuelga.
2. Una vez identificada la función: mirar qué hace exactamente (otro
   busy-wait de hardware no cubierto por `port_gba_timing.c`, una llamada
   bloqueante a un servicio 3DS no inicializado, un acceso a memoria fuera
   de rango que debería fallar pero en su lugar cuelga, etc.) y corregirla
   puntualmente.
3. **Quitar la instrumentación temporal** (`Port_DebugLog` en
   `src/agbmain.c`, bajo `#ifdef TMC_3DS`) una vez confirmado que el
   arranque pasa de ese punto — no debe quedarse permanentemente.
4. Conseguir que el bucle principal corra de forma sostenida (aunque sea
   sin vídeo/audio) — confirmar que no hay OTRO cuelgue más adelante.
5. Empezar a conectar salida de vídeo real: diseñar la versión mínima de
   `port_ppu_3ds.c` para mzm (bridging `port/ppu` software renderer →
   citro2d/citro3d), sin todo el peso de pantalla inferior/HDMA de TMC.
6. Diseñar el backend de audio real contra `UpdateMusic`/`TrackVariables`.
7. Conseguir baseroms US/JP y generar los `.map` que faltan para
   multi-región.
8. Antes de cualquier publicación: revisar de nuevo TODOS los assets
   nuevos que se vayan añadiendo (banner/iconos/splash si se reintroduce)
   para que sigan sin arte con copyright.

## 10. Dónde está todo

- Fork: `tinaut1986/mzm`, rama `wip/3ds-port-no-audio-ppu` (push pendiente
  de los últimos commits de instrumentación en el momento de escribir este
  documento).
- Máquina de build/pruebas: esta misma (`~/mzm`), con `agbcc`,
  `binutils-arm-none-eabi`, devkitPro/devkitARM, `makerom`/`bannertool`
  (`~/tools/bin/`) ya instalados.
- New 3DS de pruebas: FTP en `192.168.1.138:5000` (FBI), CIA de prueba en
  `sdmc:/cias/mzm-zm.cia`, log de diagnóstico en
  `sdmc:/3ds/mzm-debug.log`.
