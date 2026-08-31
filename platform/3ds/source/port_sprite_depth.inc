/* Overrides de profundidad estereoscópica por TIPO de sprite, para el port 3DS.
 *
 * PORT_SPRITE_DEPTH(spriteId, esSecundario, code)
 *
 *   spriteId       constante PSPRITE_* (o SSPRITE_* si esSecundario = 1)
 *   esSecundario   0 = sprite primario, 1 = secundario
 *   code           PORT_SPRITE_DEPTH_BG_COPLANAR  -> al plano del BG cuya
 *                      prioridad coincide con la prioridad OAM del sprite
 *                      (el sprite ya la fija a propósito para fundirse con él)
 *                  PORT_SPRITE_DEPTH_NONE         -> sin override (plano único)
 *                  >= 0                           -> un PORT_TIER_* explícito
 *
 * El renderer solo mira estos tags en gameplay. La clave es (spriteId,
 * esSecundario); una segunda línea con la misma clave gana a la anterior.
 *
 * Pensado para que lo escriba una herramienta (tools/sprite-workbench, aún por
 * hacer); de momento se edita a mano, igual que port_layer_fixes.inc.
 */

/* Estatuas de Kraid / Ridley de la sala previa a Tourian: el sprite de la cara
 * fija bgPriority = prioridad de BG1 para componer con el BG que dibuja la
 * parte alta de la cabeza. En el plano único de sprites la cara se despegaba
 * de la cabeza. */
PORT_SPRITE_DEPTH(PSPRITE_KRAID_STATUE,  0, PORT_SPRITE_DEPTH_BG_COPLANAR)
PORT_SPRITE_DEPTH(PSPRITE_RIDLEY_STATUE, 0, PORT_SPRITE_DEPTH_BG_COPLANAR)
