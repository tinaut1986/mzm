# Carga de ROM en runtime para el port a 3DS

Decisión de arquitectura (confirmada con el usuario): el port a 3DS debe
poder publicarse algún día, así que sigue el modelo de `zelda-tmc-3ds` — el
binario distribuido no contiene assets con copyright; el usuario pone su
propia ROM en la SD y el port la lee en cada arranque.

## Por qué esto es viable y más simple que en TMC

`mzm` es una decompilación matching casi completa (99.89%). Confirmado por
inspección: hay una separación limpia entre:

- `src/*.c` (67 ficheros) — lógica de juego pura, sin datos.
- `src/data/*.c` (476 ficheros, ~209k líneas) — solo datos, compilados vía
  `#include "extracted/data/.../archivo.pal.inc"` etc., generados por
  `tools/extractor.py` a partir de la baserom del usuario en tiempo de
  compilación.

Esto significa que, para el build GBA normal, TODO el juego (código + datos)
se compila en un único binario autocontenido. Para el port a 3DS sin
distribuir assets, la idea es: **no compilar `src/data/*.c`**, y en su lugar
resolver esos mismos símbolos en tiempo de ejecución desde la ROM cargada,
usando las direcciones ROM exactas que ya conocemos gracias al build
matching (el `.map` del linker le da a cada símbolo su dirección GBA exacta,
sin necesidad de adivinar nada, a diferencia de TMC que trabajaba con un
decomp menos completo).

## Lo que se ha implementado y verificado

- `port/port_rom.h` / `port/port_rom.c`: `gRomData`/`gRomSize`, detección de
  región vía el game code en el offset 0xAC del header (`BMXE`=US,
  `BMXP`=EU, `BMXJ`=JP — confirmado contra `Makefile:10,27,44`),
  `Port_LoadRom(path)`, y `Port_ResolveRomData(gbaAddr)` para traducir una
  dirección GBA a un puntero nativo dentro del buffer cargado.
- `port/port_rom_selftest.c`: test standalone (no forma parte de ningún
  target de build todavía) que carga `mzm_eu_baserom.gba` y resuelve
  `sChozodiaEscapeShipHeatingUpPal` (dirección `0x085dc148`, sacada de
  `mzm_eu.map`). **Verificado**: los bytes resueltos desde la ROM
  (`00 00 01 04 05 14 07 1C...`) coinciden exactamente con
  `data/chozodia_escape/ship_heating_up.pal` (el binario ya extraído por
  `tools/extractor.py`). El mecanismo de resolución de direcciones es
  correcto.
- `port/port_gba_mem.h` ajustado para usar el `types.h` propio de `mzm` en
  vez del `port_types.h` de TMC (que no existe en este repo).

Cómo reproducir el test:
```
gcc -I. -Iinclude -Iport port/port_rom.c port/port_rom_selftest.c -o /tmp/t \
  && /tmp/t mzm_eu_baserom.gba
```

## Lo que falta (la parte grande, pendiente)

Verificado el mecanismo básico, pero todavía no hay forma automática de que
el código de gameplay (`src/*.c`) referencie los ~476 ficheros de datos sin
compilarlos. Cada declaración `extern const T simbolo[N];` en
`include/data/*.h` tiene que redirigirse a algo como:

```c
static const u16 (*p_sChozodiaEscapeShipHeatingUpPal)[8 * 16];
#define sChozodiaEscapeShipHeatingUpPal (*p_sChozodiaEscapeShipHeatingUpPal)
```

con `p_sChozodiaEscapeShipHeatingUpPal` apuntado en `Port_LoadRom()` (o justo
después) a `Port_ResolveRomData(offset_de_la_región_activa)`. Esto es
mecánicamente generable a partir de:

1. El `.map` de cada región (`mzm_us.map`, `mzm_eu.map`, `mzm_jp.map` —
   regenerar los tres con `make us eu jp`) para sacar la dirección GBA de
   cada símbolo por región.
2. Los headers `include/data/*.h` para sacar el tipo y las dimensiones de
   cada array/struct (necesita un parser de declaraciones C, aunque sea
   simple — no basta con regex ingenuo para tipos anidados/structs).

Siguiente paso concreto: escribir `tools/gen_port_rom_offsets.py` que haga
esto para un fichero de datos primero (p.ej. `chozodia_escape_data.h`) como
prueba de escala, antes de generarlo para los 476 ficheros. Los símbolos de
`src/*.c` (los 67 ficheros de lógica pura) no necesitan nada de esto — se
compilan de forma nativa sin cambios, tal cual ya hace el build GBA.

## Generador implementado y verificado: `tools/gen_port_rom_offsets.py`

Por cada `include/data/*.h` genera un par `.h`/`.c` en `port/generated/`:

- El `.h` declara, por cada símbolo `extern const T nombre[dims];` del
  header original, un puntero `extern const T (*p_nombre)[dims];` +
  `#define nombre (*p_nombre)`. Reproduce los `#include` que el header
  original necesitaba (para tipos como `struct FrameData` o macros como
  `OAM_DATA_SIZE`), pero **no incluye el header original** — si coexistieran
  en la misma unidad de compilación, la macro corrompería la declaración
  `extern` original.
- El `.c` define esos punteros (sin `static`, con enlace externo real, para
  que cualquier fichero que incluya el `.h` generado vea el mismo símbolo) y
  una función `PortGen_<base>_Init(void)` que los apunta a
  `Port_ResolveRomData(offset)` según `gRomRegion`, con los offsets sacados
  del `.map` de cada región.
- Arrays multidimensionales (`T[8][2]`), arrays de punteros
  (`struct X* const[5]`) y arrays sin tamaño (`T[]`, que decae a puntero
  plano) están soportados. Los bloques `#ifdef REGION_EU`/etc. del header
  original se preservan tal cual en el generado; el include guard del propio
  header (`#ifndef X_H`/`#define X_H`) se detecta y se ignora para no
  confundirlo con una condición real.

**Verificado**:
- Cobertura de parseo: **35/35 headers de `include/data/`, 0 líneas sin
  reconocer, 2153 símbolos generados en total** (`for h in include/data/*.h;
  do python3 tools/gen_port_rom_offsets.py "$h" ...; done`).
- Test end-to-end (`port/generated/chozodia_escape_data_rom_selftest.c`,
  no forma parte de ningún build todavía): carga la baserom EU real, llama a
  `PortGen_chozodia_escape_data_Init()`, y lee
  `sChozodiaEscapeShipHeatingUpPal[0..3]` **a través de la macro generada**
  (tal cual lo haría `src/chozodia_escape.c:705`) — devuelve
  `0000 0401 1405 1C07`, coincidiendo exactamente con los valores conocidos.
  También verificado indexado 2D (`sChozodiaEscape_5ca0d8[0][0..1]`) y
  resolución de un array de `struct FrameData`.
- Un puñado de símbolos (24/36 en `io_transfer_data.h`, 4/89 en
  `ending_and_gallery_data.h`, 1/3 en `cable_link_data.h`) no aparecen en
  `mzm_eu.map` — parecen strings/datos de debug o de idiomas no enlazados en
  este build concreto. El generador los reporta explícitamente y
  `Init()` simplemente no los resuelve para esa región (quedan `NULL`),
  que es el comportamiento correcto por ahora; pendiente de investigar caso
  a caso si hace falta usarlos.

Reproducir:
```
python3 tools/gen_port_rom_offsets.py include/data/chozodia_escape_data.h \
  --map eu=mzm_eu.map \
  --out port/generated/chozodia_escape_data_rom.c \
  --out-header port/generated/chozodia_escape_data_rom.h

gcc -I. -Iinclude -Iinclude/data -Iport -Iport/generated \
  port/port_rom.c port/generated/chozodia_escape_data_rom.c \
  port/generated/chozodia_escape_data_rom_selftest.c -o /tmp/t
/tmp/t mzm_eu_baserom.gba
```

## Lo que falta ahora

1. Generar para los 35 headers reales (solo se ha commiteado el de
   `chozodia_escape_data.h` como prueba) y añadirlos a un target de build.
2. Generar `mzm_us.map`/`mzm_jp.map` (requiere baseroms US/JP que aún no
   tenemos) para completar la tabla multi-región — de momento solo hay
   offsets EU.
3. Decidir cómo hacer que el código de gameplay (`src/*.c`) use estos
   headers generados en vez de los originales para el target 3DS, sin tocar
   línea a línea cada `#include "data/*.h"`. La opción más limpia (usada
   habitualmente en ports de decomps): generar los ficheros con el **mismo
   nombre** que el original y anteponer su directorio en el `-I` del
   compilador para el target 3DS, de forma que sombreen transparentemente a
   los de `include/data/` sin tocar `src/`. Pendiente de probar.
4. Los 67 ficheros de `src/*.c` (lógica pura) sí se compilan sin cambios en
   ambos targets.
