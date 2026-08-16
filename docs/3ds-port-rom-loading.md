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
