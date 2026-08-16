# Esqueleto de port a 3DS importado desde zelda-tmc-3ds

Siguiendo `docs/metroid-3ds-port-context.md` (sección 6, paso 3): se ha copiado
la parte de `zelda-tmc-3ds` marcada allí como genérica ("cualquier juego GBA
mostrado en 3DS vía Citro2D/Citro3D"), sin tocar nada específico de TMC salvo
lo que aparece listado abajo.

## Qué se ha copiado

- `platform/3ds/` completo (fuente: `zelda-tmc-3ds/platform/3ds/`) — init de
  servicios 3DS, GPU, audio NDSP, frame pacer Old3DS, empaquetado CIA/3DSX.
- `port/ppu/` completo — renderizador PPU en software (Modo 0 + Modo 1). Según
  la auditoría en `docs/3ds-port-ppu-audit.md`, Zero Mission solo necesita
  exactamente estos dos modos, así que no hace falta ampliarlo.
- `port/port_gba_mem.{c,h}` + test — mapa de memoria GBA (EWRAM/IWRAM/IO/VRAM/ROM),
  sin ninguna estructura de TMC.
- `port/port_m4a_backend.{cpp,h}` + `port/port_m4a_stubs.c` — copiados
  inicialmente, pero **descartados tras verificar** (ver sección siguiente):
  Zero Mission NO usa M4A/Sappy. Quedan en el árbol como referencia de diseño
  del backend de audio 3DS (cómo alimentar NDSP con buffers), pero no se deben
  enlazar contra símbolos M4A que no existen en `mzm`.

## Qué NO se ha copiado (deliberadamente, del `port/` de referencia)

Todo lo demás de `port/` en `zelda-tmc-3ds` (unas 150 archivos): sistemas de
pantalla inferior específicos de TMC (`port_second_screen_dungeonmap`,
`_worldmap`, `_quest`, `_theme`...), el randomizer Picori (`port/rando/`),
`port_rom.c`/`port_rom_tables.c`/`port_linked_stubs.c` (símbolos exactos de
TMC), figurines/kinstones, asset pipeline de TMC, etc. Se dejan fuera hasta
diseñar el equivalente para Metroid (mapa de área, tanques de misiles/energía,
etc. en vez de kinstones/quest log).

## Referencias a TMC/Zelda pendientes de sustituir

Estos archivos copiados SÍ contienen texto o símbolos específicos de TMC y
necesitan edición antes de compilar contra `mzm`:

| Archivo | Qué hay que cambiar |
|---|---|
| `platform/3ds/build.sh` | Nombres de output / rutas del build |
| `platform/3ds/cia/tmc3ds.rsf` | Renombrar, y todo el bloque de metadatos (título, product code, banner) del `.rsf` de `makerom` |
| `platform/3ds/CMakeLists.txt` | Nombre del target, rutas a `src/`/`include/` (aquí son las de mzm, con layout distinto al de tmc) |
| `platform/3ds/README.md` | Reescribir para Metroid |
| `platform/3ds/source/main_3ds.c` | Según el doc de contexto, solo 2 referencias cosméticas (carpeta SD, string de versión) — el resto del arranque es genérico |
| `platform/3ds/source/bottom_idle_3ds.{c,h}` | Lógica de idle de pantalla inferior — probablemente reescribir contenido, pero la mecánica de "evitar redibujados redundantes" es reutilizable |
| `platform/3ds/source/port_config_3ds.c` | Claves de configuración específicas de TMC (probable) |
| `platform/3ds/source/port_ppu_3ds.c` | Puente entre `port/ppu` y Citro2D — revisar qué structs de TMC referencia |
| `platform/3ds/source/port_second_screen_3ds.c` | Diseño de pantalla inferior de TMC — a rediseñar para Metroid (mapa de área/inventario) |
| `port/ppu/include/assets.h`, `cpu/mode1.h`, `modes_impl.h`, `ppu_memory.h`, `virtuappu.h`, `src/mode1.c`, `src/virtuappu.c`, `README.md` | Revisar: es probable que sean solo comentarios/nombres, dado que el motor PPU en sí es genérico de hardware GBA (confirmar con `grep -n` puntual antes de tocar) |

## Hallazgo: motor de audio propio, no M4A/Sappy

`docs/metroid-3ds-port-context.md` asumía (sin confirmar) que Metroid usaría
M4A/Sappy "como la inmensa mayoría de juegos GBA de Nintendo". **No es el
caso.** `grep` sobre `src/`, `include/` y `sound/` no encuentra ninguna firma
de Sappy (`m4aSoundMain`, `SoundMainRAM`, `MPlayJumpTableCopy`...). En su
lugar, `src/audio.c` implementa un motor propio con `UpdateMusic()` y las
estructuras `TrackVariables`/`TrackData`/`PsgSoundData` — sonido por software
propio de la saga Metroid en GBA, distinto del engine estándar de Nintendo.

**Implicación**: `port_m4a_backend.cpp`/`port_m4a_stubs.c` de `zelda-tmc-3ds`
no son reutilizables tal cual (enlazan contra símbolos M4A que no existen
aquí). Lo reutilizable de esos ficheros es solo el patrón de "cómo alimentar
NDSP con buffers PCM desde un hilo de audio" — el backend real habrá que
escribirlo enganchando `UpdateMusic()`/`TrackVariables` de `mzm` en vez de la
API M4A. Confirmar si `metroidret/mf` (Fusion) comparte este mismo motor de
audio antes de asumir que el trabajo se puede compartir entre ambos ports.

## Progreso de sustitución (2026-08-16)

- `main_3ds.c`: adaptado — `APP_DIR`, game codes de ROM soportados
  (`BMXE`/`BMXP` en vez de `BZME`/`BZMP`; confirmados contra `Makefile:10,27`
  de este repo: US=`BMXE`, EU=`BMXP`, JP=`BMXJ`), hashes SHA-1 esperados
  (de `mzm_us.sha1`/`mzm_eu.sha1`/`mzm_jp.sha1`), texto de versión.
- `port_ppu_3ds.c`: solo el string cosmético del volcado de diagnóstico
  (`quick dump`) — el resto del fichero es lógica de PPU sin tocar todavía.

## Bloqueado: `CMakeLists.txt` necesita más que sustitución de texto

Inspeccionado a fondo. No es una sustitución cosmética — depende de piezas
que **no existen todavía**:

1. `list(GLOB ...)` sobre `src/playerItem/*.c`, `src/enemy/*.c`,
   `src/worldEvent/*.c`, etc. — layout de `src/` propio de la decomp de TMC.
   Hay que mapearlo contra la estructura real de `src/` de `mzm` (distinta).
2. `libs/agbplay_core` enlazado como motor de audio — no aplica, ver hallazgo
   de motor de audio propio arriba. Bloqueado hasta decidir el diseño del
   backend de audio real.
3. Fuentes de pantalla inferior (`port_second_screen_render.c`,
   `_worldmap.c`, `_dungeonmap.c`, `_quest.c`, `_theme.c`) — deliberadamente
   no copiadas (diseño de UI específico de TMC), así que el CMake no puede
   apuntar a ellas hasta tener el diseño equivalente para Metroid.
4. Flags `PC_PORT`, `NON_MATCHING`, `USE_HDMA` — específicos del sistema de
   build de TMC; hay que revisar qué necesita `mzm` realmente (su Makefile
   usa `REGION`/`DEBUG`, no estos).
5. `port_rom.c`/`port_linked_stubs.c` — no existen aún para `mzm` (ver
   sección 4 de `docs/metroid-3ds-port-context.md`, "no hay atajo aquí").

**No reescribir este archivo hasta que existan (1) el layout de fuentes
mapeado, (2) el diseño de audio, y (3) al menos un `port_rom.c` mínimo.**
Intentar producir un CMakeLists "completo" ahora daría un build que parece
terminado pero no compila contra símbolos inexistentes.

## Siguiente paso

Con `main_3ds.c` ya adaptado como plantilla de sustitución, el trabajo de
más impacto ahora es arrancar `port_rom.c`/`port_rom_tables.c` contra los
símbolos reales de `mzm` — es la pieza de la que depende todo lo demás
(incluido poder escribir un `CMakeLists.txt` real).
