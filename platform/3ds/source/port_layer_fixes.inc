/* Correcciones de capa para el port 3DS.
 * Generado por tools/layer-workbench -- vuelve a cargarlo ahí para editarlo.
 *
 * PORT_LAYER_FIX(area, room, capaOrigen, blockX, blockY, block, capaDestino)
 *
 * blockX/blockY son la posición del BLOQUE en la sala, la misma que
 * usa pDecomp en RoomUpdate*Tilemap. No son coordenadas de pantalla ni
 * del screenmap: ese da la vuelta cada 32x16 bloques y más de la mitad
 * de las salas son mayores que eso.
 *
 * El bloque se dibuja al nivel de la capa destino, ENCIMA de ella.
 * Para dejarlo detrás de una capa, mándalo a la capa anterior.
 * `block` es el valor del bloque en el tilemap de la sala y actúa de
 * checksum: si los datos cambian, la corrección falla en voz alta en
 * vez de mover otro bloque.
 */
PORT_LAYER_FIX(0, 12, 1, 45, 8, 0x008A, 2)  /* BG1 -> BG2 */
PORT_LAYER_FIX(1, 2, 2, 7, 47, 0x0082, 1)  /* BG2 -> BG1 */
PORT_LAYER_FIX(1, 2, 2, 7, 51, 0x0082, 1)  /* BG2 -> BG1 */
PORT_LAYER_FIX(1, 2, 2, 8, 56, 0x0082, 1)  /* BG2 -> BG1 */
PORT_LAYER_FIX(1, 2, 2, 9, 43, 0x0082, 1)  /* BG2 -> BG1 */