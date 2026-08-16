# Auditoría de modos de PPU (DISPCNT) para el port a 3DS

Contexto: `docs/metroid-3ds-port-context.md` del repo de referencia `zelda-tmc-3ds`
pedía auditar qué modos de display GBA usa Zero Mission antes de asumir que
`port/ppu` (renderizador PPU en software, podado a Modo 0 + Modo 1 en ese repo)
sirve tal cual. Resultado de la auditoría (grep sobre `DISPCNT`/`DCNT_MODE_*`
en todo `src/`, `include/` y `nes_metroid/`):

## Resultado

- **Modo 0** (todos los fondos tile-based, sin afín): usado en todo el juego —
  intro, menús, todas las salas, pausa, game over, créditos, y el emulador de
  NES Metroid embebido (`nes_metroid/`, que solo toca bits de enable BG3/OBJ,
  nunca cambia de modo).
- **Modo 1** (BG2 afín con rotación/escala + BG0/BG1 normales): usado en **un
  único punto de todo el juego**, `src/tourian_escape.c:1195`
  (`TOURIAN_ESCAPE_DATA.dispcnt = DCNT_MODE_1 | DCNT_BG0 | DCNT_BG2 | DCNT_OBJ`),
  durante la secuencia de autodestrucción de Tourian, con BG2 de 512x256 y
  wraparound activado. Es el único sitio donde aparecen escrituras a
  `REG_BG2PA/PB/PC/PD`.
- **Modos 2, 3, 4, 5**: no se usan en ningún punto del juego.

## Implicación para el port

El `port/ppu` de `zelda-tmc-3ds` (que solo implementa Modo 0 y Modo 1, podado
deliberadamente porque TMC tampoco usa más) es directamente aplicable a Zero
Mission sin necesidad de reimplementar más modos. Punto de atención: probar a
fondo la ruta de Modo 1 específicamente contra la secuencia de escape de
Tourian, ya que es el único caso de uso en todo el juego y por tanto el menos
"ejercitado" por el resto del port.
