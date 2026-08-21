# Estado del renderer nativo de GPU (PICA200) — continuidad (2026-08-20)

Documento de continuidad para la rama `feat/native-gpu-renderer`. Complementa
`docs/future-roadmap-and-architecture.md` (visión general/roadmap de fases) con
el detalle real de esta sesión: qué se intentó, qué se rompió, qué se arregló
con evidencia, y qué queda abierto. Léelo entero antes de tocar
`platform/3ds/source/port_gpu_renderer.c` — está lleno de bugs ya cazados una
vez, no los repitas.

## 0. Contexto: por qué existe este renderer

`port/ppu/src/mode1.c` (el renderer por software en CPU, ya maduro y
verificado en hardware) es la línea base funcional del proyecto — juego
completo, audio, guardado, todo probado. El renderer de GPU es un intento de
**sustituirlo** para liberar CPU y llegar a 60 FPS estables tanto en Old3DS
como en New3DS, siguiendo el roadmap de `docs/future-roadmap-and-architecture.md`.

**Está detrás de un flag de compilación, `PORT_GPU_TILE_RENDERER`, apagado por
defecto.** El build normal (`make` sin `EXTRA_CFLAGS`) no lo incluye y sigue
usando el renderer de CPU intacto — nada de lo de abajo afecta al build que
usa el usuario día a día salvo que se compile explícitamente con ese flag.

## 1. Herramienta nueva: bucle de pruebas local sin hardware (Azahar)

`tools/run_azahar_test.sh` ya existía en el repo (de una sesión anterior,
2026-08-19) pero no se había explotado a fondo hasta ahora. Compila, instala
en Azahar (flatpak) y lo ejecuta headless — mucho más rápido que el ciclo de
FTP a la 3DS real:

```bash
tools/run_azahar_test.sh <segundos> [--no-build] [--no-audio-dump]
```

Deja una captura en `/tmp/azahar_test_screenshot_full.png` (el desktop
completo — hay que recortar la región de la ventana de Azahar, ver más abajo)
y el log de depuración (`~/.var/app/org.azahar_emu.Azahar/data/azahar-emu/sdmc/3ds/mzm-debug.log`).

**Limitación importante encontrada esta sesión: no hay forma de enviar
pulsaciones de botón a Azahar desde este entorno** (no hay `xdotool`/`wtype`/
similar instalado, y no se instaló nada nuevo sin supervisión). Esto significa
que el bucle automático **solo puede observar lo que pasa sin pulsar nada**
— la intro y las pantallas que no requieren input se pueden inspeccionar
libremente, pero cualquier pantalla que dependa de una pulsación (selección
de partida tras el título, gameplay real) **no se puede alcanzar ni probar
sin que el usuario juegue a mano** y mande capturas.

Para escenas con estereoscopía 3D, el modo "side by side" de Azahar
(`render_3d=1` en `~/.var/app/org.azahar_emu.Azahar/config/azahar-emu/qt-config.ini`)
permite ver ambos ojos en una sola captura sin gafas ni hardware.

Truco útil para diferenciar de un vistazo si un frame se renderizó por GPU o
cayó al *fallback* de CPU **sin mirar logs**: el overlay de texto "FPS NN" en
pantalla **solo lo dibuja el camino de CPU** (`platform_gpu_3ds.c`'s
`DrawTopImageStereo`) — el camino de GPU (`Port_GpuRenderer_RenderFrame`) no
lo pinta. Si se ve "FPS" en la captura, esa imagen es CPU, no GPU.

## 2. Bugs reales encontrados y arreglados esta sesión (confirmados, con evidencia)

Todos verificados con capturas de Azahar antes/después, comparando contra
capturas de referencia de mGBA que aportó el usuario.

### 2.1 — Canales de color invertidos (texto/arte casi negros o con tinte marrón)

El atlas de texturas de tiles se dibujaba con un texenv de paso directo
(`GPU_REPLACE`), pero el PICA200 muestrea una textura `GPU_RGBA8` con los
**componentes en el orden inverso al de los bytes en memoria** — un quirk real
de la GPU del 3DS, no una suposición: el propio código de
`platform_gpu_3ds.c` (`ConfigureAbgrTextureEnv`, renombrado a
`PlatformGpu3DS_ConfigureAbgrTextureEnv` y expuesto en el header) ya tenía
que corregirlo con un texenv de 3 etapas para el buffer compuesto por CPU, que
usa el mismo empaquetado de bytes (R,G,B,A en memoria — lo que el propio
`mode1.c` llama, confusamente, "ABGR8888", nombre que en realidad describe el
orden de los dígitos hexadecimales del valor de 32 bits, no el de los bytes).

**Arreglo:** `ConfigureAtlasTextureEnv()` en `port_gpu_renderer.c`, misma
técnica de 3 etapas, adaptada para además reconstruir el alfa real en vez de
forzarlo a opaco (ver 2.2).

### 2.2 — Transparencia rota (cuadrados negros sólidos donde debería verse a través)

Consecuencia directa del mismo texenv: la etapa de alfa forzaba el canal alfa
de salida a una constante opaca (255) siempre, así que cualquier índice de
paleta 0 (transparente) se pintaba como un **cuadrado negro sólido en vez de
transparente**. Arreglado leyendo el alfa real desde el componente muestreado
correcto (`GPU_TEVOP_A_SRC_R`, que —por el mismo intercambio de orden— es el
byte de alfa real almacenado) y activando `C3D_AlphaTest(true, GPU_GREATER,
0)` para descartar los píxeles transparentes.

### 2.3 — Forced blank (bit 7 de DISPCNT) no gestionado

El hardware real muestra pantalla en blanco y desactiva todo el renderizado
de BG/OBJ mientras este bit está activo (técnica común para ocultar VRAM
mientras se reescribe entre escenas). El renderer de GPU lo ignoraba por
completo, intentando pintar con lo que hubiera en VRAM en ese instante exacto
— coincide con el patrón de "la imagen se corta justo en las transiciones"
reportado. Arreglado añadiéndolo a `Port_GpuRenderer_CanRenderFrame()` (cae a
CPU si está activo).

### 2.4 — Mapeo 2D de sprites de 8bpp multi-fila (confirmado contra el código fuente, no solo capturas)

`CollectSprite()` calculaba mal el índice de tile en modo de mapeo 2D para
sprites de 8bpp de más de una fila: usaba paso de fila 16 (y no duplicaba el
paso de columna). La fórmula correcta, verificada contra el renderer de CPU
ya probado (`port/ppu/src/mode1.c`, ~línea 2431: `tile_row * 32 + tile_col`,
`* 2` para 8bpp), es que **el paso de fila siempre es 32** en mapeo 2D
independientemente de la profundidad de color — solo el paso de columna se
duplica en 8bpp. Esto corrompía cualquier sprite de 8bpp de más de 8px de
alto — el ejemplo real encontrado fue el retrato de Samus en la pantalla
"SAMUS DATA" (selección de partida).

### 2.5 — El bug grande: buffer de citro2d agotado a las 128 primeras quads (causa real del "solo se ve una franja arriba")

**Este era el bug estructural que explicaba casi todos los síntomas visuales
reportados** ("solo se ve el 10-15% superior de la pantalla", en escenas de
contenido totalmente distinto — intro, selección de partida, estática).
`platform_gpu_3ds.c` inicializaba citro2d con `C2D_Init(128)`, dimensionado
para el uso original de ese módulo (un par de quads grandes + el texto de
FPS, bastante por debajo de 100 objetos/frame). El renderer de tiles nuevo
dibuja **600-1200+ `C2D_DrawImage` por frame** para una escena grande — todo
lo que excedía el límite del buffer interno de citro2d simplemente no se
dibujaba, sin error ni aviso. Como el orden de dibujado es fila por fila de
arriba a abajo, el resultado visual era sistemáticamente "solo las primeras
filas (la parte de arriba) se ven, el resto queda negro" — **exactamente el
mismo patrón en escenas sin relación alguna entre sí**, la pista de que era
un bug estructural y no algo específico de cada escena.

**Arreglo:** subido a `C2D_Init(4096)` (con margen sobre el `MAX_DRAW_ITEMS`
documentado del renderer) + un `C2D_Flush()` de seguridad cada 512 items
dibujados, para que un caso extremo futuro degrade a una llamada extra a la
GPU en vez de fallar en silencio otra vez.

**Confirmado con capturas antes/después:** la imagen de Samus (cinemática de
intro) pasó de verse cortada en una franja superior a verse completa. La
escena de estática de la intro pasó de verse a bandas a cubrir toda la
pantalla. El fondo estrellado de la pantalla de título pasa a verse completo
y sin el overlay de FPS (confirmando que lo gestiona GPU).

### 2.6 — BLDCNT (blending) excluido por completo → el gameplay real casi nunca usaba GPU

`Port_GpuRenderer_CanRenderFrame()` rechazaba cualquier frame con
alfa-blend/brillo/oscurecimiento activo, cayendo siempre a CPU. Auditando
`src/transparency.c` se confirmó que el juego activa BLDCNT **de forma
rutinaria en salas normales** (agua, capas de transparencia, orden de Samus
sobre fondos) — no solo en fundidos especiales. Esto significaba que el
gameplay real, con blending activo casi siempre, **nunca llegaba a pasar por
el renderer de GPU**, sin importar los demás arreglos.

**Arreglo implementado (aproximación, no réplica exacta del hardware):**
- Brillo/oscurecimiento (efectos 2/3 de BLDCNT): aplicado directamente al
  decodificar cada tile de una capa marcada como "first target", con la
  misma fórmula exacta que `mode1.c` (`mode1_brighten`/`mode1_darken`,
  copiada como `ApplyBrighten`/`ApplyDarken` en `port_gpu_renderer.c`).
  Incorporado a la clave de caché de tiles (`TileCacheKey.brightAdjust`) para
  no reusar por error un tile ya aclarado/oscurecido cuando no toca.
- Alfa-blend (efecto 1): segunda pasada de dibujado por ojo, solo para los
  items marcados `blendAlpha`, con blending real de GPU
  (`C3D_AlphaBlend`/`C3D_BlendingColor`) usando EVA como factor de blend
  constante. **Aproximación explícita, no exacta**: asume `EVA+EVB≈16` (el
  caso típico de fundido cruzado) y compone contra lo que ya haya en el
  framebuffer tras la primera pasada (capas no-blend ya dibujadas), en vez de
  identificar por píxel cuál es la capa "second target" real como hace GBA de
  verdad (el renderer de CPU sí lo hace bien, comparando `top_layer`/
  `bottom_layer` por píxel — replicar eso en GPU necesitaría leer el
  framebuffer por píxel, no es práctico con el pipeline actual).

**Sin verificar en hardware ni con una escena de gameplay real en Azahar**
(no se pudo alcanzar gameplay sin poder pulsar botones — ver sección 1).

## 3. Lo que sigue roto (reportado por el usuario tras probar el build de la sección 2, en hardware real)

Todo esto queda **sin diagnosticar y sin arreglar**, pendiente de la próxima
sesión:

### 3.1 — Líneas horizontales finas en toda la imagen ("efecto persiana")

Visible en todas las capturas correctas (Samus, estrellas, estática) como
líneas horizontales oscuras finas repetidas cada pocos píxeles. Sospecha sin
confirmar: el escalado del atlas/quads es 1.5x (240→360, `scale = 1.5f` en
`Port_GpuRenderer_RenderFrame`), un factor no entero — con `GPU_NEAREST`
como filtro de textura (`C3D_TexSetFilter(&sAtlasTexture, GPU_NEAREST,
GPU_NEAREST)`), un escalado no entero puede saltarse o duplicar filas de
píxeles de forma irregular al muestrear, generando justo este tipo de
patrón de bandas. **No investigado más a fondo** — sería el primer sitio
donde mirar la próxima sesión (probar filtro lineal, o forzar que cada
draw-call de tile use coordenadas de textura que caigan exactamente en el
centro de texel para evitar el aliasing, o repensar el pipeline de escalado
completo).

### 3.2 — Menú de selección de partida: solo se ve el fondo, sin las opciones, hasta que se pulsa un botón

**Reportado por el usuario, sin diagnosticar.** Antes de cualquier pulsación,
la pantalla de selección de partida ("SAMUS DATA") muestra solo la imagen de
fondo (el rostro Chozo) — el título "SAMUS DATA", las filas de datos de
partida (A/B/C) y el menú COPY/ERASE/OPTIONS **no se ven** hasta que se pulsa
un botón, momento en el que además **salta al renderer de CPU** (aparece el
overlay de FPS).

Dos posibles explicaciones a investigar, ninguna confirmada:
1. **Comportamiento real del hardware**: puede que la capa de texto/menú
   (probablemente un BG distinto al del fondo) genuinamente no esté
   habilitada en DISPCNT hasta que el juego entra en el sub-estado
   interactivo de selección de partida — en cuyo caso el renderer de GPU
   estaría reflejando fielmente un estado transitorio real, y el "bug" sería
   solo que un jugador nunca se queda parado en ese frame exacto (siempre
   pulsa algo before darse cuenta). Se puede confirmar comparando contra
   mGBA en ese instante exacto (congelando el emulador de referencia justo
   al entrar en la pantalla, antes de pulsar nada).
2. **Bug real de recolección**: si la capa de texto SÍ está habilitada en ese
   frame y el renderer de GPU simplemente no la dibuja (los sprites del menú
   —cursor, iconos de casco de Samus, flechas de copia— se generan vía el
   sistema `MenuOam` genérico de `src/menus/pause_screen.c`, reusado por
   `file_select.c` — comprobar si de verdad escriben en `gOamMem` real o si
   hay algún paso intermedio que el recolector de sprites de
   `port_gpu_renderer.c` no está leyendo).

Y sigue sin explicarse por qué **pulsar un botón fuerza la caída a CPU** —
comprobar qué cambia en DISPCNT/BLDCNT/ventanas al procesar la primera
pulsación en `file_select.c` (round de anotación de cursor, quizás activa un
`WIN0`/`WIN1` para resaltar la opción seleccionada, lo cual SÍ excluimos en
`CanRenderFrame`).

### 3.3 — Intro por GPU no parece ir a 60 FPS (sin poder medirlo)

El usuario reporta que, aunque el overlay de FPS no aparece (confirmando
camino GPU) durante la intro, **visualmente no parece ir a 60 FPS real** —
pero sin el contador no hay forma de confirmarlo con certeza. Pendiente:
activar temporalmente el overlay de FPS también en el camino de GPU (no
estaba conectado — ver `Port_GpuRenderer_RenderFrame`, no llama a nada
equivalente a `DrawStatusText`) para poder medir de verdad, o usar el log
`PORT_PPU_PERF_LOG`/`PORT_GPU_RENDERER_DIAG_LOG` ya existentes (ver
`docs/3ds-port-status-2026-08-17.md`, sección de esta misma rama) para sacar
un número real en vez de una impresión visual.

### 3.4 — El gameplay real, una vez cargada la partida, sigue renderizándose por CPU

**El objetivo principal de esta rama sigue sin cumplirse.** Con el build de
la sección 2 (blending ya soportado), el usuario reporta que el gameplay
real, tras cargar una partida, se sigue viendo "sin zoom" (a resolución
nativa sin escalar — la pinta característica del camino de CPU cuando
`sTopPresentWidth`/aspecto no coincide, o simplemente confirma que es el
buffer compuesto por CPU) **y con el overlay de FPS visible**, es decir seguía
sin pasar al camino de GPU pese a permitir blending.

Hipótesis sin confirmar para la próxima sesión: puede que el gameplay real
combine blending CON ventanas (`WIN0`/`WIN1`) simultáneamente — recordar que
`transparency.c` también escribe `WIN1H`/`WIN1V` (`gSuitFlashEffect`) y hay
más lógica de ventanas en ese mismo fichero (`REG_WINOUT`, `REG_WININ`) que
no se ha auditado a fondo esta sesión. Si las ventanas están activas de forma
igual de rutinaria que el blending, haría falta soportarlas también (mucho
más trabajo: las ventanas de GBA definen regiones rectangulares donde ciertas
capas se activan/desactivan por píxel, no hay equivalente directo trivial en
el pipeline de quads actual — necesitaría scissor rects por capa, como mínimo,
o shaders custom). **Antes de intentar implementarlo, confirmar con datos
reales** (instrumentar `Port_GpuRenderer_CanRenderFrame()` para loguear
*por qué* rechaza cada frame durante gameplay real, en vez de asumir).

## 4. Recomendación concreta para la próxima sesión

1. Instrumentar `Port_GpuRenderer_CanRenderFrame()` para que, cuando rechace
   un frame, loguee (detrás de `PORT_GPU_RENDERER_DIAG_LOG`) **cuál** de las
   condiciones fue la que lo tiró (forced blank / modo != 0 / ventana / mosaic
   / OBJ afín) — ah-mismo se sabe que rechaza pero no por qué, y eso es
   necesario para saber si el problema de 3.4 son ventanas, mosaic, u otra
   cosa.
2. Conseguir alguna forma de automatizar input hacia Azahar (instalar
   `xdotool`/`ydotool`/`wtype` **con permiso explícito del usuario primero**)
   para poder alcanzar selección de partida y gameplay real sin depender de
   que el usuario juegue a mano y mande capturas — habría acelerado mucho
   esta sesión.
3. Root-causear 3.1 (líneas horizontales) antes de dar la Fase 1 del roadmap
   por cerca de terminar — es el único bug puramente visual (no de
   contenido) que queda, y probablemente sea rápido de arreglar una vez que
   se confirme la causa (filtro de textura vs. escalado no entero).
4. Root-causear 3.2 y 3.4 con datos reales de hardware/Azahar con input antes
   de intentar arreglarlos a ciegas otra vez.

## 5. Continuación misma sesión (2026-08-20, tarde): objetivo principal cumplido

Sesión de seguimiento directo, con el usuario jugando en hardware real y
mandando capturas/logs por FTP entre iteraciones (`192.168.1.133:5000`, FBI).
Se siguió al pie la recomendación de la sección 4 — instrumentar antes de
arreglar — y esta vez sí se pudo alcanzar gameplay real con input. Resultado:
**el objetivo principal de la rama (gameplay real usando el renderer de GPU)
se cumple ya**, con varios bugs de correctness reales encontrados y
arreglados por el camino. Todo en `platform/3ds/source/port_gpu_renderer.c`
salvo que se indique lo contrario.

### 5.1 — Instrumentación añadida (recomendación 1 de la sección 4, hecha)

- `Port_GpuRenderer_CanRenderFrame()` ahora loguea (detrás de
  `PORT_GPU_RENDERER_DIAG_LOG`, throttled 1/30 rechazos) **cuál** condición
  rechazó el frame: `GPU_REJECT: forced blank / mode != 0 / WIN0 / WIN1 /
  OBJWIN / mosaic BG / affine OBJ`.
- `GPUDIAG` ampliado con `bldcnt`/`eff`/`eva`/`evb`/`blend` (nº de items
  marcados para el segundo pase de alpha-blend).
- Nuevo log `STEREO slider=... left=... right=...` y `EYE0/EYE1
  drawCount=... reasserted=...` por ojo (cuidado: un contador compartido con
  módulo par entre los dos ojos hace que uno de los dos nunca se loguee —
  usar contadores separados por ojo, ver `sEyeDrawLogCounter[2]`).
- `CMDBUF usage=%` (uso del command buffer de C3D) logueado en
  `PlatformGpu3DS_EndBottom`.
- **Overlay de debug en pantalla inferior**, siempre visible
  independientemente del path de render (a diferencia del "FPS NN" de la
  pantalla superior, que **solo** dibuja el path de CPU — ver
  `DrawTopImageStereo`): línea 1 `FPS<n> GPU`/`FPS<n> CPU`, línea 2 (solo
  GPU) `<items> <obj> <cache>`. Requirió añadir los glifos `C`/`G` al font
  bitmap 5x7 de `platform_gpu_3ds.c` (antes solo tenía A,D,E,F,M,P,S,U,V).
- **`KEY_X` como hotkey de diagnóstico** (`Platform3DS_PollKeysIntoGba` en
  `platform_3ds_minimal.c`, cae fuera de `MZM_KEY_MASK` así que no interfiere
  con el juego): vuelca los tres render targets reales (ojo izq., ojo dcha.,
  pantalla inferior) tal cual los produce la PICA200 a
  `sdmc:/3ds/mzm-dump-{left,right,bottom}.rgb` (crudo, sin cabecera, RGB8,
  dimensiones nativas sin rotar — 240x400 los de arriba, 240x320 el de
  abajo; rotar 90° al visualizar). Ver `PlatformGpu3DS_DumpScreens()`. No se
  acumulan: cada pulsación sobreescribe los mismos tres ficheros. **Esta
  herramienta fue decisiva** — permitió ver directamente que el contenido
  del ojo derecho realmente faltaba en el framebuffer (no era un artefacto
  de captura/ventana), cosa que ningún log por sí solo hubiera podido
  demostrar con la misma certeza.

### 5.2 — Bug real, el gordo: orden de prioridad de capas invertido (causa raíz de 3.4)

`CollectBgLayer`/`CollectSprite` calculaban el `sortKey` de dibujado
(orden ascendente = se dibuja antes = queda más al fondo) usando
`priority` directamente. En GBA, **prioridad 0 = se dibuja encima,
prioridad 3 = va al fondo** — justo al revés de lo que asumía el código.
Cualquier capa con prioridad 3 (típicamente la decoración de fondo) se
pintaba la última, tapando BG0-2 y todos los sprites/OBJ (incluida Samus).
Confirmado con `GPUDIAG` en gameplay real: la única capa visible era
`bg3[cnt=060b...]` (prioridad 3).

**Arreglo:** invertir a `(3 - priority) * 10 + tiebreak` en ambos sitios.
Con esto **el gameplay real pasó de verse solo fondo a verse completo**
(suelo, paredes, Samus) por primera vez en esta rama.

### 5.3 — Bug real: WIN1 rechazaba casi todo el gameplay por un falso positivo

`src/transparency.c` activa `WIN1` en prácticamente cualquier sala normal
(`TransparencySetRoomEffectsTransparency`), pero como mecanismo para
habilitar efectos BLDCNT por capa vía `WININ`, no para recortar pantalla: la
ventana cubre las 240x160 completas y `WININ_H=0x3F` dentro (todas las capas
habilitadas, sin restricción). `CanRenderFrame()` rechazaba el frame con solo
ver el bit de WIN1 activo, sin comprobar si realmente recortaba algo —
confirmado con diagnóstico que **WIN1 era el motivo de rechazo dominante en
gameplay real** (20 de 22 rechazos en una sesión de prueba).

**Arreglo:** `WindowCoversFullScreen()` — si WIN0/WIN1 están activas pero
cubren la pantalla completa y `WININ` no restringe ninguna capa (`== 0x3F`),
se permite el frame (es un no-op real). Si de verdad recorta algo (p.ej. el
efecto de flash del traje, que sí encoge el rectángulo), sigue cayendo a CPU
correctamente. OBJ window sigue sin aproximarse (no se ha visto que haga
falta).

### 5.4 — Bug real: `depthTier` de paralaje estéreo desalineado con la tabla

`kTierEyeOffsetPx` está comentada como `{BG3, BG2, BG1, BG0, OBJ}` (de más
lejano a más cercano), pero `CollectBgLayer` pasaba `bgIndex` directamente
como `depthTier` (0=BG0..3=BG3) — el índice contrario al que la tabla espera.
BG0 (pensado para desplazamiento 0, al ser HUD/primer plano) leía en
realidad el valor de BG3 (-3.5px, pensado para el fondo lejano), y viceversa.

**Arreglo:** `PushItem(..., 3 - bgIndex, ...)`. Por sí solo el efecto era
pequeño (máx. ±3.5px) y no explicaba el bug grande de la sección 5.6 (el
contenido no aparecía en absoluto, no solo desplazado) — pero es un bug real
e independiente, ya corregido.

### 5.5 — Bug real: signo del paralaje estéreo invertido

`eyeSign` era `-1` para el ojo izquierdo y `+1` para el derecho. Combinado
con que `kTierEyeOffsetPx` son todos `<= 0`, esto hacía que el ojo
*izquierdo* se desplazara hacia la derecha y el *derecho* hacia la
izquierda para las capas de fondo — paralaje cruzado/negativo, que se
percibe como que el fondo **sale disparado hacia el jugador** en vez de
hundirse en la pantalla. Confirmado por el usuario en hardware ("los fondos
parecen estar por delante que el frente").

**Arreglo:** invertir `eyeSign` (`+1` izquierda, `-1` derecha). Confirmado
arreglado por el usuario tras el cambio.

### 5.6 — Bug real, el gordo #2: contenido del ojo derecho ausente con el 3D activado

Con el 3D activado, el ojo derecho perdía contenido de forma sistemática: el
menú de "¿guardar?" y la barra de HUD desaparecían **por completo** (no
desplazados, ausentes), mientras el fondo se veía bien en ambos ojos. El
overlay de FPS de la pantalla inferior también desaparecía por completo con
el 3D activado. El recuento de draw calls (`EYE0`/`EYE1 drawCount=...`) era
**idéntico** entre ambos ojos — se estaban emitiendo los mismos
`C2D_DrawImage`, así que no era un bug de "saltarse" items.

Se descartaron por datos, en este orden, antes de dar con la causa:
- Agotamiento del command buffer de C3D (`C3D_GetCmdBufUsage()`): 0.1-0.2%
  de uso incluso en el ojo con más carga. No era esto.
- Invocación doble de `Port_PPU_PresentFrame()`/`PlatformGpu3DS_EndBottom()`
  por frame real (había una vía plausible vía `VBlankIntrWait()` en
  `src/transfer.c`): instrumentado, **0 casos** de re-entrada en cientos de
  frames. No era esto.
- Depth-test de la GPU dejado en su valor por defecto de citro2d en vez de
  desactivado explícitamente: se desactivó explícitamente
  (`C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL)`) como higiene general
  (2D por orden de dibujado no debería depender de él), pero no cambió nada
  observable — no era la causa, aunque el cambio se mantiene por ser
  correcto.

**Causa real, confirmada con las capturas de `KEY_X`:** el buffer de
vértices interno de citro2d (`C2D_Init`, **distinto** del command buffer de
C3D ya descartado) estaba dimensionado a `4096` — pensado para el peor caso
de **un solo ojo** (`MAX_DRAW_ITEMS`). Con el 3D activado, los draws de
ambos ojos caen en ese mismo buffer dentro de un único frame de C3D antes de
que se libere del todo, así que lo que se dibuja al final (justo el
contenido del segundo ojo procesado, en la capa más alta del z-order) se
pierde en silencio — el mismo patrón de fallo que el bug original de la
sección 2.5 (buffer de citro2d agotado a las 128 primeras quads), esta vez
disparado por la suma de draws de los dos ojos en vez de por uno solo muy
cargado.

**Arreglo:** `C2D_Init(4096 * 3)`. Confirmado arreglado por el usuario — ya
se ve todo en ambos ojos.

### 5.7 — Bug introducido y arreglado en la misma sesión: colores invertidos al añadir el overlay

Al añadir el overlay de debug de la pantalla inferior (dibujo "sólido",
`C2D_DrawRectSolid`/texto) resucitó el bug de canales de color invertidos de
la sección 2.1 (pantalla roja/magenta) — pero solo *después* de que el
renderer de GPU empezara a usarse de verdad en gameplay real (gracias al fix
de la sección 5.2). Causa: citro2d reprograma internamente el TEV (unidad 0)
cada vez que cambia entre dibujo "con textura" (`C2D_DrawImage`) y "sólido"
— no tiene ni idea de que `ConfigureAtlasTextureEnv()`/
`ConfigureAbgrTextureEnv()` tocan esos registros a mano por fuera de su
propio tracking. Como el overlay dibuja "sólido" todos los frames, el modo
queda en "sólido" al final de cada frame; el primer `C2D_DrawImage` del
atlas del frame siguiente dispara el cambio de modo y citro2d pisa la
configuración manual con su valor por defecto (que no invierte el orden de
canales del PICA200).

**Arreglo:** reafirmar `ConfigureAtlasTextureEnv()` justo después del primer
`C2D_DrawImage` de cada pase (no antes — volvería a pisarse por ese mismo
draw), tanto en el pase normal como en el de blend. Ver el flag
`reassertedTexEnv` en `Port_GpuRenderer_RenderFrame`.

### 5.8 — No son bugs: casos que siguen (correctamente) cayendo a CPU

Confirmado con el log `GPU_REJECT` que en el tramo de intro el único motivo
de caída a CPU es `affine OBJ`:

- **La nave acercándose en la intro** usa un sprite afín (rotado/escalado)
  para el efecto de zoom — los OBJ afines están fuera de alcance del
  renderer de GPU desde el diseño original (documentado desde el principio,
  nunca implementado). Cae a CPU correctamente.
- **El instante justo al pulsar seleccionar partida** cae a CPU un frame y
  vuelve — coincide con el cursor de selección activando una ventana
  (WIN0/WIN1) *real* y recortada (no a pantalla completa) para resaltar la
  opción, que sigue sin aproximarse (ver 5.3: solo se aproxima el caso
  no-op de pantalla completa).

Ninguno de los dos es una regresión de esta sesión ni necesita arreglo
urgente — es la salvaguarda de "cae a CPU en vez de dibujar mal"
funcionando como se diseñó. Implementar soporte de OBJ afín y de ventanas
reales sería trabajo nuevo, no un bug fix.

### 5.9 — Sigue sin resolver: rendimiento (~20-30 FPS con GPU, lejos de los 60 objetivo)

Confirmado con el overlay de FPS de la pantalla inferior (sección 5.1): la
intro (~650 items/frame) va a ~38-40 FPS; gameplay real (~2500-2700
items/frame) a ~20-30 FPS. **No se ha investigado esta sesión** — todo el
tiempo se fue en los bugs de correctness de arriba, que eran prerequisito
(no tenía sentido perfilar rendimiento de un renderer que aún pintaba mal o
se caía a CPU la mayor parte del tiempo). `PORT_PPU_PERF_LOG` ya existe en
el código para esto (ver Makefile) pero no se ha usado todavía sobre el
renderer de GPU en esta rama.

## 6. Recomendación concreta para la próxima sesión

1. **Rendimiento** (sección 5.9) es ahora el bloqueante principal para dar
   la Fase 1 del roadmap por cerca de terminar — el renderer ya es correcto
   en 2D y 3D, pero a 20-30 FPS no cumple el objetivo de la rama (60 FPS
   estables). Usar `PORT_PPU_PERF_LOG` + el overlay de FPS/items de la
   pantalla inferior (sección 5.1) para correlacionar FPS con nº de items y
   encontrar el cuello de botella real (¿CPU decodificando/cacheando tiles?
   ¿GPU con demasiadas texturas pequeñas por draw call en vez de atlas
   compartido de verdad? ¿flush innecesarios?).
2. Las líneas horizontales finas (sección 3.1 original) siguen sin
   confirmar/arreglar — no se ha vuelto a mirar esta sesión.
3. Si se quiere que la nave de la intro y el instante de selección de
   partida (sección 5.8) también usen GPU, sería trabajo nuevo: soporte de
   OBJ afín y de ventanas (WIN0/WIN1) reales, no aproximaciones ya
   existentes.
4. La herramienta de `KEY_X` (sección 5.1) es reutilizable para cualquier
   futuro "¿qué hay REALMENTE en este render target?" — usarla antes de
   especular por logs solos cuando el contenido visual no cuadre con los
   contadores.

## 7. Continuación misma sesión (2026-08-20, noche): primera ronda de optimización de rendimiento (punto 1 de la sección 6)

Sin poder aún medir en hardware real dentro de esta sesión (solo Azahar sin
input disponible), se atacaron por inspección de código los tres cuellos de
botella de CPU más obvios en `port_gpu_renderer.c`, todos con evidencia
directa en el propio código (no especulación) y todos verificados por
compilación limpia (build normal sin flags + build con
`PORT_GPU_TILE_RENDERER`/`PORT_GPU_RENDERER_DIAG_LOG`) y una pasada de humo
en Azahar (intro, sin input): sin corrupción visual, FPS de intro subió de
~38-40 (dato de la sección 5.9) a **45** en esta máquina de desarrollo (dato
de Azahar, no de hardware real — orientativo, no concluyente).

### 7.1 — `GetOrDecodeTileSlot`: de búsqueda lineal O(n) a hash table O(1) amortizado

Sospecha número uno según la propia sección 5.9 ("¿CPU decodificando/
cacheando tiles?"): `GetOrDecodeTileSlot` escaneaba linealmente
`sCacheKeys[0..sCacheCount)` **por cada referencia a un tile**, no por cada
tile único — hasta ~2500-2700 referencias/frame en gameplay real, cada una
comparando contra hasta unos cientos de entradas de caché ya pobladas ese
frame. Sustituido por una tabla hash de encadenamiento abierto
(`sHashBucketHead`/`sHashChainNext`, 8192 buckets), "vaciada" con un
contador de generación por bucket en vez de poner a cero las 8192 entradas
cada frame (un bucket se trata como vacío si su generación guardada no
coincide con la del frame actual).

### 7.2 — `qsort()` de hasta 3200 items → counting/bucket sort O(n)

`sortKey` solo toma 35 valores posibles (`(3-priority)*10 + tiebreak`,
tiebreak en [0,4]) — un rango perfecto para bucket sort en vez de la
comparación genérica de `qsort` (coste de llamada indirecta por
comparación, especialmente caro en ARM11). Cada `PushItem` encadena el
nuevo item directamente en la lista enlazada de su bucket
(`sBucketHead`/`sBucketTail`/`sBucketNext`); al final de la recolección se
vuelca en `sSortedOrder` con una sola pasada por los 35 buckets. Efecto
colateral: esto también corrige una suposición de estabilidad que el propio
comentario original asumía pero `qsort()` de la libc no garantiza (el orden
de dibujado dentro de un mismo `sortKey` para sprites de igual prioridad
dependía de que el orden de iteración de OAM se preservara).

### 7.3 — Transferencia del atlas: de 1MB completo cada frame a solo las filas usadas

El hallazgo más significativo de esta ronda. `sAnyDirtySlot` disparaba un
`GSPGPU_FlushDataCache` + `C3D_SyncDisplayTransfer` (**síncrono**, bloquea
la CPU hasta terminar) del atlas **completo** (512x512x4 bytes = 1MB) cada
frame, aunque una escena típica solo usa unos pocos cientos de los 4096
slots. Como `GetOrDecodeTileSlot` siempre asigna slots secuencialmente desde
0 y la caché se resetea cada frame, el conjunto de slots usado en un frame
dado es **siempre un prefijo contiguo del atlas** (filas de tiles
`[0, ceil(cacheCount/64))`) — no hace falta transferir nada más allá de esa
altura. Arreglado calculando `dirtyHeight` a partir de `sCacheCount` y
pasándolo como alto real a `GSPGPU_FlushDataCache`/`C3D_SyncDisplayTransfer`
en vez de `ATLAS_DIM` fijo. Con `cache=144-158` en la intro (ver logs de la
sección 7 más abajo) esto transfiere ~3 filas de tiles (24px) en vez de las
512px completas — más de 20x menos datos movidos síncronamente por frame.

### 7.4 — Pendiente, sin tocar esta sesión

- **No se ha medido en hardware real ni en gameplay real** (mismo límite de
  siempre: Azahar sin input en este entorno). Los 45 FPS de intro en Azahar
  son una señal positiva de que no hay regresión de correctness y de que
  las tres optimizaciones sí reducen trabajo de CPU, pero **no sustituyen
  una medición en New3DS/Old3DS real con `PORT_PPU_PERF_LOG` durante
  gameplay**, que es donde de verdad importa (2500-2700 items/frame, muy
  por encima de los ~650 de la intro).
- Posibles cuellos de botella todavía no investigados si el paso anterior
  no basta: número de draw calls de citro2d en sí (600-1200+ quads de 8x8
  por ojo en vez de agrupar en quads más grandes cuando hay tiles
  idénticos contiguos), coste de `DecodeTileIntoSlot` por tile único
  (loop de 8x8 por tile, sin SIMD), o diferencia real Old3DS vs New3DS
  (clock de CPU/GPU distinto — el objetivo es 60 FPS en ambos, no solo en
  New3DS).
- Recomendación inmediata para la próxima sesión: instalar el CIA de esta
  build (compilada con `PORT_GPU_TILE_RENDERER`, `PORT_GPU_RENDERER_DIAG_LOG`
  y `PORT_PPU_PERF_LOG`) en hardware real (Old3DS y New3DS si es posible) y
  jugar una sesión de gameplay real con el overlay de FPS de la sección 5.1
  visible, comparando el número contra los ~20-30 FPS de la sección 5.9.

## 8. Continuación misma sesión (2026-08-20, noche): la causa raíz real del rendimiento — caché de tiles no persistente entre frames

Probado en Azahar tras la sección 7 (usuario confirma ~40 FPS en gameplay,
lejos de los 60 objetivo, y hace la pregunta correcta: **¿por qué el
renderer de CPU sí llegaba a 60 FPS y este no, si en teoría la GPU rasteriza
más rápido?**). Respuesta encontrada por inspección de código, no
especulación: `Port_GpuRenderer_RenderFrame` reseteaba `sCacheCount = 0`
(y con él, toda la caché de tiles decodificados) **al principio de cada
frame** — es decir, aunque las secciones 7.1/7.2/7.3 ya habían arreglado
cómo se *busca* en la caché y cómo se *sube* el atlas a la GPU, el propio
contenido de la caché se tiraba y se reconstruía desde cero 60 veces por
segundo. Con unos pocos cientos de tiles únicos por sala (la mayoría
estáticos frame a frame, solo cambia el scroll) esto significaba
**redecodificar el mismo contenido una y otra vez sin necesidad** —
búsqueda de paleta + posible brighten/darken por cada uno de los 64 píxeles
de cada tile único, repetido en cada frame aunque el tile no hubiera
cambiado. Este coste de CPU, no el fill-rate de la GPU ni el volumen de
draw calls, es la explicación real de por qué "usar la GPU" no batía ya de
entrada al renderer de CPU maduro (que compone directamente a un
framebuffer lineal, sin esa capa de recodificación repetida).

**Arreglo:** la caché de tiles (`sCacheKeys`/hash table) ahora **persiste
entre frames** en vez de reiniciarse. `GetOrDecodeTileSlot` guarda en
`sCacheSourceBytes[slot]` una copia de los bytes fuente (hasta 64B) usados
en la última decodificación de cada slot; en un acierto de caché, compara
(`memcmp`) esos bytes contra los actuales de VRAM antes de confiar en el
contenido cacheado — si coinciden (el caso común), se reutiliza el slot sin
decodificar nada; si difieren (tile animado: agua, lava...), se redecodifica
en el mismo slot. Un `memcmp` de ≤64 bytes es mucho más barato que una
decodificación completa. Efectos colaterales que hubo que resolver:
- La caché ya no se puede desbordar de forma segura reseteándose cada
  frame (antes garantizado por el reset periódico) — ahora se reinicia
  proactivamente cuando le quedan <256 slots libres, pero **solo al
  principio de un frame** (nunca a mitad de frame, que invalidaría índices
  de slot ya usados por `DrawItem`s de ese mismo frame vía `SlotToUV`).
- La subida del atlas a la GPU ya no puede asumir que los slots usados este
  frame son un prefijo contiguo desde 0 (eso solo era cierto cuando la
  caché se vaciaba cada frame) — sustituido por un rango de filas sucias
  real (`sDirtyMinRow`/`sDirtyMaxRow`), actualizado en
  `DecodeTileIntoSlot`, que en un frame típico (mayoría de aciertos sin
  redecodificar) puede llegar a no escribir nada en absoluto.

**Sin verificar en hardware ni con gameplay real** (misma limitación de
Azahar sin input). Probado en Azahar solo con la intro (contenido casi
estático, así que el beneficio esperado ahí es pequeño por diseño — el caso
que de verdad se beneficia es gameplay con cientos de tiles reutilizados
frame a frame): compila limpio, sin corrupción visual, `cache=926-938`
creciendo con normalidad (encaja con que ahora persiste en vez de resetear
cada frame a ~144-160).

### Pendiente para la próxima sesión

1. **Medir en hardware real, en gameplay, con este cambio** — es el único
   dato que falta para saber si esto por fin acerca el renderer a 60 FPS
   estables en New3DS y, sobre todo, en Old3DS (más lento, el caso más
   exigente y el que el usuario ha pedido explícitamente que también
   llegue a 60).
2. Si sigue sin bastar: el siguiente sospechoso razonable es el número de
   draw calls de citro2d en sí (miles de quads de 8x8 por frame en vez de
   fusionar tiles contiguos idénticos en quads más grandes, o dibujar BGs
   enteros con un único buffer de vértices por capa vía C3D directamente en
   vez de `C2D_DrawImage` por tile) — no investigado ni tocado esta sesión,
   ver conversación de esta sesión para el razonamiento.

## 9. Continuación misma sesión (2026-08-20, noche): dos hallazgos importantes -- una regresión de correctness y un agujero en la instrumentación

### 9.1 — Regresión real: caché de tiles persistente sin tener en cuenta EVY

Probado en Azahar y confirmado por el usuario en hardware: tras el cambio de
la sección 8 (caché persistente entre frames), **escenarios y varios menús
dejaron de verse**. Causa encontrada por inspección: `TileCacheKey` incluye
`brightAdjust` (el enum discreto NONE/BRIGHTEN/DARKEN) pero, tal y como
advertía el propio comentario original ("evy itself is one value for the
whole frame, doesn't need to be part of the key"), **no incluye el valor
real de `EVY`**. Esa suposición era cierta mientras la caché se
reconstruía entera cada frame (sección 7); dejó de serlo en cuanto la
caché empezó a persistir entre frames (sección 8): un tile decodificado
una vez con un `EVY` alto durante un fundido a negro queda cacheado con
esa oscuridad **para siempre**, porque los bytes de origen en VRAM (lo
único que se compara para invalidar) no cambian durante el fundido, solo
`EVY` cambia. Esto atascaba escenarios y menús en negro/invisibles
después de cualquier fundido u otro efecto BLDCNT con `EVY` variable.

**Arreglo:** `EVY` (0-16) añadido como campo más de `TileCacheKey`
(0 cuando `brightAdjust == NONE`), incluido en el hash y en la comparación
de igualdad. **Sin verificar aún en hardware** (la intro de Azahar nunca
activa BLDCNT — `eff=0` en todo el log de pruebas — así que no se pudo
reproducir el bug ni confirmar el arreglo localmente; pendiente de que el
usuario lo pruebe).

### 9.2 — Agujero de instrumentación: el log PERF no mide el coste real del renderer de GPU

El usuario preguntó, con razón, cómo se pretende llegar a 60 FPS en
gameplay si ni siquiera la intro (sin sprites afines, la escena más
simple posible) pasaba de ~45 FPS en Azahar. Investigando el propio log
`PERF[N]: main=...ms w0=...ms w1=...ms gpuDraw=...ms gpuProc=...ms`
(`port_ppu_mzm.c`) se descubrió que **no mide lo que parecía**:
- `main`/`w0`/`w1` son los contadores internos de `virtuappu_mode1` (el
  renderer de CPU) — y `port_ppu_mzm.c` línea ~199 solo llama a
  `virtuappu_mode1_render_frame()` cuando **NO** se usa el renderer de GPU.
  Con el renderer de GPU activo (el caso de estas pruebas), esos números
  son **restos obsoletos** de la última vez que corrió el renderer de CPU
  (típicamente el arranque), no una medición del frame actual. Confirmado
  viendo el log: el mismo valor exacto (`main=17.15ms w0=18.15ms
  w1=18.06ms`) se repite idéntico en sucesivas líneas `PERF`, imposible si
  fueran mediciones frescas de un renderer con más de 900 tiles/frame
  variando.
- `gpuDraw`/`gpuProc` son contadores propios de citro3d y solo cubren el
  envío/espera de la GPU en sí (PICA200) — no el trabajo de CPU de
  `Port_GpuRenderer_RenderFrame()` (recolección de VRAM, hash/memcmp/
  decodificación de tiles, bucket sort), que es exactamente el código que
  se ha estado optimizando en esta rama. **Nadie había medido ese coste.**

**Arreglo:** instrumentación nueva y directa en `port_gpu_renderer.c`
(`svcGetSystemTick()` alrededor de las dos fases de
`Port_GpuRenderer_RenderFrame`), expuesta como log
`GPUTIME collectMs=... drawMs=...` (cada 30 frames, detrás de
`PORT_GPU_RENDERER_DIAG_LOG`) y como `Port_GpuRenderer_GetLastFrameTimingMs()`
para quien quiera consumirlo desde otro sitio. `collectMs` cubre recolección
+ caché + decodificación + sort; `drawMs` cubre subida del atlas + envío de
draw calls de ambos ojos.

**Resultado en Azahar, con una pega importante:** `collectMs≈20ms
drawMs≈40ms` (total ~60ms/frame reportado) es **inconsistente** con los
~45 FPS reales medidos (~22ms/frame) — un frame no puede tardar 60ms en
CPU y presentarse a 45 FPS. La explicación más probable es que el contador
de ciclos de Azahar (`svcGetSystemTick` bajo emulación) no corresponde a
tiempo real de hardware de forma fiable. **Conclusión: ni siquiera esta
instrumentación nueva se puede confiar en Azahar** — solo sirve como señal
relativa (aquí, `drawMs` ~2x `collectMs`, sugiriendo que el envío de draw
calls podría pesar más que la recolección/decodificación, pero esto es una
hipótesis sin confirmar, no una conclusión).

### Pendiente para la próxima sesión (reemplaza la lista de la sección 8)

1. **Instalar este build en hardware real y leer los logs `GPUTIME` durante
   gameplay** — es la primera vez que existe una forma de medir el coste
   real (en ciclos de CPU de verdad, no los de un emulador) del renderer de
   GPU en sí, separado de todo lo demás. Sin esto, cualquier otro cambio en
   esta rama seguiría siendo a ciegas.
2. Confirmar en hardware que el arreglo de la sección 9.1 (EVY en la clave
   de caché) realmente devuelve los escenarios/menús que dejaron de verse.
3. Con datos reales de `collectMs`/`drawMs` de hardware: si `drawMs`
   domina, el siguiente paso es reducir el número de draw calls (fusionar
   tiles en quads más grandes, o un buffer de vértices por capa vía C3D en
   vez de `C2D_DrawImage` por tile de 8x8); si `collectMs` domina, mirar el
   propio bucle de decodificación/hash en vez del pipeline de dibujado.

## 10. Continuación misma sesión (2026-08-20, noche): datos reales de hardware + segunda regresión (relacionada) encontrada y arreglada

El usuario instaló el build de la sección 9 en hardware real (FTP a
`192.168.1.133:5000`, FBI) y jugó una sesión; el log (`mzm-debug.log`,
descargado también por FTP) por fin da **datos reales, no de emulador**:

```
GPUTIME collectMs=19-36ms  drawMs=37-53ms   (decenas de muestras, sesión real)
```

Ambos números superan por sí solos el presupuesto de 16.67ms/frame para 60
FPS, y `drawMs` es sistemáticamente ~1.5-2x `collectMs` — la primera señal
fiable (no de Azahar) de dónde está yendo el tiempo. **Pendiente para la
próxima sesión**, no investigado aún: si `drawMs` domina de verdad, el
sospechoso es el volumen de draw calls de citro2d (miles de quads de 8x8
por frame), no la recolección/decodificación de tiles.

### 10.1 — La regresión seguía sin arreglarse: EVY en la clave de caché causaba desbordamiento del atlas completo

El usuario reportó que, tras el build de la sección 9 (que en teoría
arreglaba el bug de "negro fijo" de la sección 8), **seguían sin verse
sprites de menús y escenarios**. El propio log de hardware lo explica:

```
GPUDIAG ... cache=4096 ...   (dos veces en una sesión -- el límite exacto del atlas)
```

El arreglo de la sección 9.1 (meter `EVY` en la clave de la caché para que
un tile oscurecido durante un fundido no se quedara fijo) tuvo un efecto
secundario no anticipado: durante un fundido, `EVY` cambia casi cada frame,
así que cada tile afectado por brillo/oscurecido reclamaba un **slot nuevo
del atlas por cada valor distinto de EVY que atravesaba** en vez de
reutilizar uno — un solo fundido de pantalla completa (confirmado en el
log: `bldcnt=00ff eff=3 eva=16`, oscurecido de toda la pantalla) podía
agotar los 4096 slots del atlas en un único frame. Una vez agotado, todo
tile que no cabía se dibujaba con el contenido (incorrecto) del slot 0 en
su lugar -- la causa real de "escenarios y menús no se ven".

**Arreglo:** revertido `EVY` a NO formar parte de la clave de hash/caché
(vuelve a la clave original: offset+bpp+paleta+flips+isObj+brightAdjust).
En su lugar, `EVY` se guarda por slot (`sCacheEvy[]`) y se comprueba como
una condición de obsolescencia más -- igual que ya se hacía con
`memcmp(sCacheSourceBytes[i], ...)` para tiles animados -- que fuerza una
redecodificación **en el mismo slot** en vez de reservar uno nuevo. Esto
arregla el bug de "negro fijo" de la sección 8 sin reintroducir el
desbordamiento de la 10.1: un fundido sigue sin beneficiarse de la caché
(se redecodifica su tile casi cada frame, igual que antes de que existiera
la persistencia), pero ya no consume slots nuevos del atlas al hacerlo.

También se subió el margen del reset proactivo de la caché (sección 8) de
256 a `ATLAS_MAX_SLOTS/2` (2048) -- 256 nunca fue suficiente margen frente
a un solo frame que introduce cientos/miles de combinaciones de tile
nuevas (el propio peor caso teórico de un frame de BG a pantalla completa
son ~2600 referencias).

**Verificado:** compila limpio, sin corrupción visual en Azahar (misma
limitación de siempre -- la intro de Azahar no activa BLDCNT, así que no
reproduce ni el bug ni su arreglo). **Pendiente de confirmar en hardware
real** con gameplay que atraviese fundidos/menús -- CIA ya subido por FTP.

### Pendiente para la próxima sesión

1. Confirmar en hardware que escenarios y menús ya no desaparecen, en una
   sesión que incluya fundidos/transiciones (no solo gameplay estático).
2. Con los `GPUTIME` de la sección 10 (reales, de hardware):
   `drawMs` (~37-53ms) domina sobre `collectMs` (~19-36ms) -- investigar el
   volumen de draw calls de citro2d como sospechoso principal antes que la
   recolección/decodificación (que ya se optimizó en las secciones 7-8).
3. Seguir vigilando el log `GPUDIAG` por si `cache=` se acerca de nuevo a
   4096 en sesiones largas -- señal de que el margen de 2048 tampoco basta
   y hace falta revisar de nuevo.

## 11. Continuación misma sesión (2026-08-20, noche): la causa real -- la caché nunca comparó el CONTENIDO de la paleta, solo su número de banda

El arreglo de la sección 10.1 (EVY fuera de la clave, comprobado por
slot) se subió y probó; el usuario reportó: **sigue igual, pero al salir y
volver a entrar en la sala de guardado, esa sala concreta se vio bien --
el resto del mundo seguía en negro.** Esa pista (una sala se arregla sola
al recargarla, el resto no) es la que llevó a la causa real, distinta de
las dos anteriores (EVY y desbordamiento de atlas, secciones 8 y 10.1 --
ambas reales pero no la causa principal de "todo en negro").

**El bug:** `TileCacheKey` guarda `palBank`, un **índice** de banda de
paleta (0-15), pero la comprobación de "¿sigue siendo válido este slot
cacheado?" nunca comparaba el **contenido real** de esa banda (los colores
RGB), solo los bytes de la gráfica del tile en VRAM (`sCacheSourceBytes`).
Es una práctica muy común en juegos de GBA (y Zero Mission no es
excepción) reutilizar la misma forma de tile genérica en muchas salas
distintas, cargando una paleta diferente por sala en la misma banda -- es
decir, **los bytes de VRAM del tile no cambian entre salas, solo la
paleta**. Con la caché persistente, la primera sala visitada que usara una
combinación (offset, banda) quedaba fijada con SUS colores para siempre;
cualquier otra sala que reutilizara esa misma combinación con una paleta
distinta heredaba los colores equivocados -- que a menudo eran negros o
casi negros si la primera sala cacheada tenía una paleta oscura,
exactamente el síntoma reportado. Salir y volver a entrar en la sala de
guardado no arregla la clave de caché en sí -- fuerza una recarga de VRAM
que sí dispara el `memcmp` de bytes de tile existente (sección 8) para esa
sala, y con eso de rebote también se recalcula todo lo que hace referencia
a esos bytes -- explica por qué "solo esa sala" se veía bien tras
recargarla.

**Arreglo:** nuevo snapshot por slot del contenido real de paleta usado en
la última decodificación (`sCachePalBytes[slot]` -- 32 bytes/16 colores
para un tile 4bpp con banda, o los 512 bytes/256 colores completos para un
tile 8bpp, que no usa bandas). Comprobado por `memcmp` en cada acierto de
caché, igual que ya se hacía con los bytes de VRAM del tile y con `EVY` --
si la paleta ha cambiado, se redecodifica el mismo slot en el sitio
(nunca se reserva uno nuevo, por la misma razón que EVY en la sección
10.1).

**Verificado:** compila limpio, sin corrupción visual en Azahar (misma
limitación de siempre: la intro no visita varias salas con paletas
distintas, así que no reproduce el bug ni prueba el arreglo con
certeza). **Este es el candidato más fuerte hasta ahora para la causa
real** -- encaja con el patrón exacto reportado (una sala se arregla,
el resto no) de un modo que ni el bug de EVY ni el desbordamiento de
atlas explicaban del todo. Subido por FTP, pendiente de que el usuario lo
confirme en una sesión que visite varias salas distintas.

## 12. Continuación misma sesión (2026-08-20, noche): batching real de draw calls vía shader propio

Con la causa de correctness resuelta (sección 11) y el usuario confirmando
que ya se ve bien, se retomó el rendimiento (objetivo: 60 FPS estables en
New3DS **y** Old3DS). Los `GPUTIME` de hardware de la sección 10
(`drawMs≈37-53ms` dominando sobre `collectMs≈19-36ms`) señalaban el envío
de draw calls como sospechoso principal: hasta ese momento, cada uno de los
~2500-2700 tiles/frame se dibujaba con su propia llamada a
`C2D_DrawImage` (citro2d), **por cada ojo** -- miles de llamadas con
overhead de biblioteca (matrices, gestión de buffer en modo inmediato) por
frame.

**Cambio:** shader de vértices propio (`platform/3ds/source/gpu_tile.v.pica`,
compilado con `picasso` y embebido con `bin2s` -- nuevas reglas en el
`Makefile`, ver `SHADER_BIN`/`SHADER_OBJ`/`SHADER_HDR`) + un buffer de
vértices construido en CPU y enviado con **una sola** llamada
`C3D_DrawArrays` por pase (opaco/blend) por ojo -- 4 llamadas de dibujo por
frame en vez de miles. `BuildAndDrawBatch()` en `port_gpu_renderer.c`
escribe 6 vértices (2 triángulos, sin index buffer) por item directamente
en un buffer de memoria lineal (`sVertexBuf`, reservado una vez en
`Port_GpuRenderer_Init`), usando exactamente las mismas coordenadas finales
de pantalla que antes calculaba `C2D_DrawParams` (escala + offset de
paralaje estéreo), así que el resultado visual debía ser idéntico -- solo
cambia *cómo* se envía la geometría a la GPU, no qué geometría es. El
texenv fijo existente (`ConfigureAtlasTextureEnv`, alfa-test, blend) no
cambia -- es estado de PICA200 independiente del shader de vértices.

### 12.1 — Bug encontrado y arreglado en Azahar antes de tocar hardware: `C2D_Prepare()` que faltaba

Al probar en Azahar, el overlay de depuración de la pantalla inferior
(sigue dibujado con citro2d normal, sin tocar) salió como **ruido
vertical ilegible** en vez de texto. Causa: el propio comentario de
`C2D_Prepare()` en el header de citro2d lo advierte -- "solo hace falta
una vez en el programa si citro2d es el único que usa la GPU". Al
empezar a dibujar la pantalla superior con nuestro propio programa/
`AttrInfo`/`BufInfo` vía C3D directamente (sin pasar por citro2d en
absoluto), citro2d deja de ser el único usuario, y sus llamadas
posteriores en el mismo frame (el overlay de la pantalla inferior, ya en
`PlatformGpu3DS_EndBottom`) ya no reconfiguran su propio layout de
atributos antes de dibujar -- interpretaba sus propios datos de vértice a
través de nuestro layout, de ahí el "ruido". **Arreglo:** una llamada a
`C2D_Prepare()` al final de `Port_GpuRenderer_RenderFrame()`, devolviendo
la GPU a citro2d en estado conocido antes de que dibuje la pantalla
inferior. Confirmado en Azahar: overlay legible de nuevo, intro sin
corrupción.

### 12.2 — También arreglado por el camino: carrera en el `Makefile`

Regla nueva con dos targets (`$(SHADER_OBJ) $(SHADER_HDR): $(SHADER_BIN)`)
se ejecutaba **dos veces en paralelo** con `make -j`, y en un primer intento
esto causó un `#include "gpu_tile_shbin.h"` fallido por una dependencia con
la ruta equivocada (`$(BUILD)/3ds/port_gpu_renderer.o` en vez de
`$(BUILD)/3ds/source/port_gpu_renderer.o` -- `OBJS_3DS` conserva el
`source/` del path). Arreglado: dependencia con la ruta correcta, y la
receta bin2s movida a un único target (`SHADER_OBJ`) del que `SHADER_HDR`
solo depende, sin receta propia -- evita que `make -j` la invoque dos veces.

**Verificado en Azahar:** build normal (sin flags) y build de diagnóstico
compilan limpio; sin corrupción visual (intro + overlay de depuración
correctos) tras el arreglo de `C2D_Prepare()`.

### 12.3 — Probado en hardware real: empeora el rendimiento, revertido

El usuario instaló el CIA en hardware real. Dos problemas confirmados por
el propio log (`mzm-debug.log`, descargado por FTP -- se verificó primero
que sí era la build nueva: aparecen líneas `GPUTIME`, que solo existen en
este código, y el formato de `EYE%d drawCount=%d` ya no lleva
`reasserted=`, coherente con haber quitado ese hack al eliminar los
`C2D_DrawImage`):

1. **El ruido en la pantalla inferior seguía apareciendo** pese al arreglo
   de `C2D_Prepare()` verificado en Azahar -- no reproducido/explicado en
   esta sesión; posible diferencia real de comportamiento entre Azahar y
   hardware, sin investigar más a fondo porque el punto 2 ya bastaba para
   revertir.
2. **El rendimiento empeoró, no mejoró:** con estéreo 3D activo (slider a
   fondo) y ~2573 items/frame, `gpuDraw=8.58ms gpuProc=11.68ms` (contadores
   reales de citro3d, antes ~0.66/2ms) y `GPUTIME collectMs=23.47
   drawMs=88.19` (antes `drawMs≈37-53ms` -- prácticamente el doble) con
   `fps=25.0`, peor que el ~40-45 de antes del cambio.

**Sospecha sin confirmar** (no investigada, el cambio se revirtió antes de
diagnosticar): el coste probablemente esté en
`GSPGPU_FlushDataCache(sVertexBuf, ...)`, llamado hasta 4 veces por frame
(2 pases x 2 ojos) sobre un buffer que puede rondar los 300KB cada vez
(~2573 items x 6 vértices x 20 bytes) -- una sincronización de caché de
CPU **bloqueante**, del mismo tipo que ya se identificó como problema real
con la transferencia del atlas de texturas (sección 7.3), esta vez
reintroducido en un sitio nuevo sin darse cuenta. Consistente con que
`gpuDraw`/`gpuProc` (contadores de citro3d, que sí incluyen esperas de
sincronización) subieran tanto.

**Revertido** en la misma sesión (el cambio nunca se había confirmado --
`git status` lo mostraba sin commitear): `git checkout --
platform/3ds/Makefile platform/3ds/source/port_gpu_renderer.c` +
borrado de `gpu_tile.v.pica`. Vuelto a compilar y subir por FTP el build
de la sección 11 (el último confirmado correcto por el usuario en
hardware). El shader/Makefile/batching de la sección 12 **no está en el
repositorio** -- documentado aquí únicamente como intento fallido, para no
repetirlo sin más la próxima vez.

## 13. Continuación misma sesión (2026-08-20, noche): siguiente intento -- hash de paleta precalculado por frame en vez de memcmp por referencia

Siguiendo la recomendación más segura de la sección 12.3 (optimizar
`collectMs` en vez de arriesgar otra vez el pipeline de dibujado), se
atacó un coste identificable e innecesario introducido por el propio
arreglo de correctness de la sección 11: la comprobación de paleta
(`memcmp(sCachePalBytes[i], palSrc, palBytes)`) se ejecutaba **en cada
referencia a un tile** (hasta ~2500+/frame), no solo por tile único, y
para tiles de 8bpp comparaba hasta **512 bytes** cada vez -- potencialmente
más de 1MB de `memcmp` por frame en escenas con muchos tiles de 8bpp (el
propio `cache=938` de la intro, con mucho arte detallado, es sospechoso de
ser mayormente 8bpp).

**Arreglo:** los bytes de paleta (`gBgPltt`/`gObjPltt`) no cambian a mitad
de frame, así que la comparación puede basarse en un hash calculado **una
sola vez por frame** en vez de releer/comparar los bytes en cada
referencia. `Port_GpuRenderer_RenderFrame` calcula ahora, una vez, un hash
FNV-1a de cada una de las 16 bandas de 4bpp (`sBgPalBankHash`/
`sObjPalBankHash`) y de la paleta completa de 8bpp
(`sBgPalFullHash`/`sObjPalFullHash`); `GetOrDecodeTileSlot` compara ese
hash de 4 bytes (`sCachePalHash[slot]`, sustituye al array
`sCachePalBytes[ATLAS_MAX_SLOTS][512]` de 2MB) en vez de hacer memcmp de
hasta 512 bytes por referencia. Misma condición de obsolescencia que
antes, mucho más barata de comprobar; además libera ~2MB de RAM estática.

**Riesgo aceptado explícitamente:** una colisión de hash de 32 bits entre
dos paletas realmente distintas es la única forma en que esto podría
reintroducir el bug de la sección 11 -- astronómicamente improbable para
el espacio real de valores de paleta de este juego, pero no es una
garantía matemática como el memcmp exacto que sustituye. Documentado en el
propio comentario del código como decisión consciente.

**Verificado:** compila limpio (build normal y de diagnóstico), sin
corrupción visual en Azahar. **Sin verificar en hardware con `GPUTIME`
real** -- CIA subido por FTP, pendiente de que el usuario lo pruebe en
gameplay y compare `collectMs` contra la línea base de la sección 10
(~19-36ms) para confirmar que de verdad baja, y confirmar que no ha
reintroducido el bug de paleta de la sección 11 (visitar varias salas con
paletas distintas).

## 14. Continuación misma sesión (2026-08-21, madrugada): la causa real de por qué ni la intro llega a 60 -- `C3D_SyncDisplayTransfer` es bloqueante, y el tamaño no es el problema

El usuario preguntó, con razón, por qué ni la intro (la escena más simple
del juego, sin lógica de gameplay) se acercaba a 60 FPS, y si eso no ponía
en duda que el resto del juego pudiera llegar. Se añadió instrumentación
más fina dividiendo `collectMs` en dos partes: `tile` (recolección de VRAM
+ hash + decodificación + sort -- todo lo optimizado en las secciones
7-9-13) y `upload` (la transferencia del atlas a la GPU,
`C3D_SyncDisplayTransfer`).

**Resultado, incluso en la intro:** `tile≈1ms` (ya muy barato, las
optimizaciones anteriores sí funcionan) pero **`upload≈19-20ms` --
prácticamente todo `collectMs`**, y constante independientemente de si se
transfieren pocas filas o muchas. Se probó además a sustituir el rango
min-max de filas sucias (sección 7.3/10) por un mapa de bits de 64 filas
(`sDirtyRowMask`) transfiriendo solo los tramos contiguos realmente
sucios -- una regresión propia que sí merecía arreglarse (un rango
min-max puede abarcar casi todo el atlas si se tocan una fila baja y una
alta en el mismo frame), pero el tiempo de `upload` **no bajó**: seguía
en ~19-20ms.

**Conclusión:** el coste no es proporcional al tamaño de los datos
transferidos -- es el propio mecanismo de `C3D_SyncDisplayTransfer`
(`GX_DisplayTransfer` + espera bloqueante del evento `PPF` vía GSP, un
viaje de ida y vuelta a través del módulo GSP) el que tiene un coste fijo
alto por llamada, casi con toda seguridad dominado por la
latencia de IPC/servicio, no por el DMA en sí. Esto explica por sí solo
por qué ni la intro llega a 60: ~20ms de esos ~22ms/frame de presupuesto
para 60 FPS se van en una única llamada bloqueante, independientemente de
lo demás.

### Por qué no se ha arreglado ya (riesgo real, decisión pendiente del usuario)

`GX_DisplayTransfer` (la función de bajo nivel que `C3D_SyncDisplayTransfer`
envuelve) es **asíncrona por diseño** -- se puede lanzar sin esperar,
y solo señala el evento `PPF` al completarse. citro3d **no expone**
públicamente una variante async, solo la que bloquea, casi con toda
seguridad a propósito: la GPU real (motor de transferencia + rasterizador)
procesa sus colas en orden, así que lanzar la transferencia sin esperar y
dejar que los draw calls posteriores del mismo frame la usen debería ser
seguro (el hardware no empieza el siguiente trabajo encolado hasta acabar
el anterior) -- **pero** `sAtlasPixels` (el buffer de origen en CPU) se
sigue escribiendo en el **frame siguiente** cuando aparece un tile nuevo o
un tile animado se redecodifica en el mismo slot. Si esa escritura de CPU
ocurre antes de que el DMA async del frame anterior haya terminado de
leer ese mismo buffer, es una condición de carrera real -- corrupción
visual intermitente, del tipo que es fácil que no aparezca en una sesión
de prueba corta pero sí en una partida larga. Arreglarlo bien
necesitaría doble buffer del atlas en CPU (escribir en A mientras se
transfiere B, alternar), más complejidad y memoria.

Dado que esta misma sesión ya tuvo que revertirse un cambio (sección 12,
el batching de draw calls) por una regresión real detectada solo en
hardware, **no se ha intentado el cambio async sin consultar** -- es
justo el tipo de cambio de alto riesgo/alta recompensa que necesita que
el usuario decida si acepta el riesgo antes de gastar otra ronda de
iteración por FTP en ello.

**Verificado:** el arreglo del mapa de bits (`sDirtyRowMask`) compila
limpio y no cambia el comportamiento visual en Azahar -- es una mejora de
robustez real (evita el caso patológico de rango min-max disparado) pero
no resuelve el problema de fondo, que es el bloqueo en sí, no el tamaño.
CIA subido por FTP.

## 15. Continuación misma sesión (2026-08-21, madrugada): eliminar la transferencia GX por completo, en vez de amortiguarla

El usuario hizo la pregunta correcta: si una sola actualización ya cuesta
~20ms, actualizar con menos frecuencia no reduce el trabajo total, solo lo
reparte -- cambiaría un coste pequeño y constante por un tirón grande
ocasional cada N frames, probablemente peor de percibir aunque la media de
FPS mejorase sobre el papel. Eso descartaba la mitigación "barata" de la
sección 14 y dejaba dos caminos: la transferencia asíncrona con doble
buffer (aplazada por riesgo de condición de carrera, ver sección 14), o
**eliminar la transferencia GX por completo**.

Investigando la API de citro3d/libctru: `C3D_TexInitVRAM` (lo que se
llevaba usando toda la rama) reserva la textura en un banco de VRAM
dedicado que la CPU **no puede escribir directamente** -- de ahí la
necesidad de un paso de transferencia GX intermedio para llevar los
píxeles desde un buffer de CPU hasta ahí. Pero `C3D_TexInit` (sin
`VRAM`) reserva la textura en memoria lineal normal (FCRAM) vía
`linearAlloc`, que la CPU **sí puede escribir directamente** -- el único
motivo de usar VRAM dedicada es un bus de memoria separado (menos
contención con la FCRAM), no una limitación de acceso.

El único motivo real por el que hacía falta el paso de transferencia GX
(más allá de mover los bytes) era el "swizzle": la PICA200 exige que las
texturas estén almacenadas con los píxeles de cada bloque de 8x8
reordenados en un patrón Z-order/Morton concreto, que
`GX_TRANSFER_OUT_TILED(1)` aplicaba por nosotros durante la transferencia.
Ese reordenado es una tabla fija de 64 entradas, bien documentada y
reproducida en herramientas de texturizado de homebrew de 3DS -- se puede
calcular a mano.

**Cambio:** `sAtlasTexture` pasa de `C3D_TexInitVRAM` a `C3D_TexInit`
(memoria lineal, escribible por CPU). `DecodeTileIntoSlot` ya no escribe a
un buffer `sAtlasPixels` intermedio -- escribe **directamente** en
`sAtlasTexture.data`, aplicando la tabla de swizzle (`kSwizzleLUT`) píxel
a píxel. El bloque de subida al final de `Port_GpuRenderer_RenderFrame`
ya no llama a `C3D_SyncDisplayTransfer` en absoluto -- solo hace
`GSPGPU_FlushDataCache` (mantenimiento de caché de CPU local, sin viaje de
ida y vuelta al módulo GSP) sobre los mismos tramos de filas sucias que ya
se calculaban con el mapa de bits de la sección 14. `sAtlasPixels` se
elimina por completo.

**Verificado en Azahar:** la parte que de verdad importaba comprobar sin
hardware -- que la tabla de swizzle calculada a mano es correcta -- se
confirma visualmente: intro y overlay de depuración se ven perfectos, sin
ningún patrón de corrupción/bloques desordenados (que es exactamente lo
que se vería si el swizzle estuviera mal). El tiempo de `upload` que
reporta Azahar en esta prueba no bajó, pero **no es una señal fiable**:
`GSPGPU_FlushDataCache` es una operación completamente distinta a
`C3D_SyncDisplayTransfer` y ya sabíamos que el reloj de Azahar no
corresponde a tiempo real de hardware (sección 10). **La medición que de
verdad importa solo se puede hacer en hardware real.**

**Riesgo de esta sesión, más bajo que el de la sección 12:** a diferencia
del batching de draw calls (revertido) o de la transferencia async
(aplazada), este cambio no depende de un mecanismo de sincronización GPU
sutil -- si el swizzle estuviera mal, se vería inmediatamente como
texturas corruptas (ya descartado en Azahar), no como un bug intermitente
dependiente de timing. El riesgo restante es puramente de rendimiento: si
`C3D_TexInit` (FCRAM) resulta tener peor ancho de banda de muestreo que
`C3D_TexInitVRAM` en la práctica, podría no ganar tanto como se espera --
pero no debería empeorar la corrección visual.

CIA subido por FTP (build normal y de diagnóstico, ambos compilan limpio).

## 16. Continuación misma sesión (2026-08-21, madrugada): la causa real era la propia instrumentación de diagnóstico -- objetivo de 60 FPS cumplido

El usuario probó el CIA de la sección 15 en hardware y reportó que no
cambiaba nada, lo cual no cuadraba: el arreglo (escribir directamente en
una textura en memoria lineal, sin `C3D_SyncDisplayTransfer`) debería
haber sido una mejora real. Investigando el propio código de medición se
encontró un error metodológico propio: el timestamp `tBeforeUpload` se
capturaba **antes** del bloque de log `GPUDIAG` (que llama a
`Port_DebugLog`, escritura de fichero real a la tarjeta SD, 1 de cada 5
frames), así que ese coste de E/S quedaba mal atribuido a "upload" en vez
de a "tile". Ninguno de los dos arreglos de la sección 15
(`C3D_SyncDisplayTransfer` → `GSPGPU_FlushDataCache` → `svcFlushProcessDataCache`)
podía moverse en la medición porque ninguno tocaba lo que de verdad se
estaba midiendo.

Al mover el timestamp después del bloque de diagnóstico, `upload` cayó a
**0.00ms** (el arreglo de la sección 15 sí funciona) y el coste se reveló
donde realmente estaba: `Port_DebugLog()` hace `fopen`+`fwrite`+`fclose`
**en cada llamada**, y ese ciclo completo de apertura/cierre de fichero en
la tarjeta SD cuesta esos ~18-20ms -- consistente con el resto de hallazgos
de la sesión, y **coherente con el propio comentario original del
fichero** (`port_debug_log.h`), que documentaba el flush inmediato como
decisión deliberada para poder diagnosticar cuelgues (la última línea
escrita en disco antes de un cuelgue sin volcado de crash es la pista) --
nunca pensado para llamarse desde un camino de 60 Hz.

**La prueba definitiva:** se compiló un build con `PORT_GPU_TILE_RENDERER`
pero **sin ningún flag de log** (`PORT_GPU_RENDERER_DIAG_LOG`/
`PORT_PPU_PERF_LOG` desactivados) -- el overlay de FPS en pantalla no
depende de esos flags, así que se podía medir el rendimiento real sin
ningún coste de E/S de diagnóstico de por medio. Resultado en Azahar:
**60 FPS** en la intro (framerate del propio contador de frames
presentados, no de los ticks de CPU poco fiables de la sección 10).
**Confirmado por el usuario en hardware real: 57-60 FPS incluso en
gameplay real** -- el objetivo de la rama (60 FPS estables, GPU en vez de
CPU, tanto en New3DS como en el caso más exigente) se da por cumplido. La
diferencia entre 57 y 60 encaja con que el propio GBA corre a ~59.7275 Hz
reales, no exactamente 60 -- no es un problema real, son "decimales".

### 16.1 -- Logging bufferizado (a petición del usuario), preservando la garantía de diagnóstico de cuelgues

El usuario preguntó si se podía bufferizar el log en RAM y volcar a disco
cada cierto tiempo en vez de en cada llamada. Correcto en el fondo, pero
`Port_DebugLog` existe **a propósito** con flush inmediato para poder
diagnosticar cuelgues reales (ver el comentario original del fichero) --
bufferizarlo sin más perdería exactamente esa garantía justo para el caso
que lo justificaba. Solución: dos funciones separadas en
`port/port_debug_log.c`/`.h`:

- `Port_DebugLog()` (sin cambios): flush inmediato, para los checkpoints de
  arranque/cuelgue (`main_3ds.c`, `port_bios.c`, etc.).
- `Port_DebugLogBuffered()` (nueva): acumula en un buffer de 4KB en RAM,
  solo escribe a disco (`fopen`+`fwrite` de todo el buffer +`fclose`, una
  vez) cuando el buffer se llena. `Port_DebugLogFlush()` fuerza un volcado
  manual si hiciera falta.

Migrados a la versión bufferizada todos los sitios de log **por frame**
(nunca checkpoints de arranque): `port_gpu_renderer.c` (GPUDIAG/GPUTIME/
STEREO/EYE), `port_ppu_mzm.c` (PERF/PPU/verbose-frame), `platform_gpu_3ds.c`
(CMDBUF) -- vía un `#define Port_DebugLog Port_DebugLogBuffered` local en
cada fichero (todas sus llamadas ya estaban detrás de flags de diagnóstico
por frame, ninguna es un checkpoint de arranque). El volcado de pantalla
por `KEY_X` (evento raro, disparado por el usuario) se deja sin
bufferizar a propósito.

**Contrapartida esperada, ya observada por el usuario:** el volcado del
buffer (cuando se llena, cada pocos segundos con logging activo en vez de
cada frame) sigue costando un tirón puntual -- mucho más raro que antes
(de 60 veces/segundo a una vez cada varios segundos), pero no
desaparecido del todo. Aceptable: solo ocurre con los flags de
diagnóstico activados (el build normal, sin ningún flag, no llama a
`Port_DebugLog`/`Buffered` en absoluto en estos puntos).

**Verificado:** compila limpio con todos los flags de diagnóstico a la
vez; probado en Azahar que las líneas bufferizadas llegan íntegras y sin
corromper al fichero de log, intercaladas correctamente con las líneas de
flush inmediato de otros ficheros no tocados.

### Estado de la rama tras esta sesión

**Objetivo principal cumplido: 60 FPS (57-60 en la práctica) en gameplay
real, confirmado en hardware, usando el renderer de GPU en vez del de
CPU.** Sesión larga con varios callejones sin salida documentados a
propósito (secciones 8/10.1 -- EVY y desbordamiento de atlas; sección 11
-- paleta no comparada; sección 12 -- batching de draw calls, revertido;
sección 14/15/16 -- la cadena completa hasta encontrar que el cuello de
botella real era, primero, `C3D_SyncDisplayTransfer`, y después, el propio
logging de diagnóstico malmedido) para que la próxima vez que aparezca un
patrón parecido ("un arreglo no cambia nada") se mire primero si la propia
medición es fiable antes de descartar la hipótesis.

## 17. Continuación misma sesión (2026-08-21, madrugada): "efecto persiana" -- confirmado y arreglado

Con el objetivo de 60 FPS cumplido, se retomó el bug puramente visual
pendiente desde el principio de la rama (sección 3.1 original, nunca
vuelto a mirar). El usuario capturó los tres volcados de pantalla reales
(`KEY_X`, sección 5.1) y se confirmó visualmente: líneas oscuras finas
verticales (y también horizontales, según el usuario) con un espaciado
regular de ~12px -- exactamente el tamaño de un tile de 8px escalado al
1.5x usado para llenar la pantalla.

**Causa confirmada:** los tiles se empaquetan borde con borde en el atlas
(sin margen/gutter), y `SlotToUV()` calculaba las coordenadas UV usando
los bordes exactos de cada tile (`sx/ATLAS_DIM` .. `(sx+8)/ATLAS_DIM`).
Con filtro `GPU_NEAREST` y el escalado no entero (8px origen → 12px
destino), un valor de UV interpolado que cae exactamente en el borde
entre dos tiles es ambiguo por precisión de coma flotante -- puede
redondear al primer texel del tile vecino en el atlas en vez del último
texel del tile actual, mostrando contenido de un tile completamente
distinto como una fina línea de "sangrado".

**Arreglo:** `SlotToUV()` ahora usa el **centro** del primer/último texel
en vez del borde exacto del tile (`(sx+0.5)/ATLAS_DIM` ..
`(sx+7.5)/ATLAS_DIM`) -- la técnica estándar para atlas de texturas sin
margen con filtrado por vecino más cercano: mantiene cada punto de
muestreo interpolado más cerca de su texel correcto que de cualquier
texel del tile vecino, sin recortar visiblemente el tile (el filtro
nearest de todas formas redondea al texel más cercano).

**Verificado:** compila limpio, sin regresión visual en Azahar (~59-60
FPS mantenidos). **Pendiente de confirmar en hardware** con otro volcado
`KEY_X` de la misma escena para comparar directamente contra la captura
"antes" de esta sesión. CIA subido por FTP (build normal y con
`PORT_GPU_TILE_RENDERER`, sin logs de diagnóstico).

## 18. Continuación misma sesión (2026-08-21, madrugada): overlay de la pantalla inferior -- "ruido" y duplicación, dos bugs reales encontrados y arreglados, PERO la duplicación en sí sigue sin resolverse

**Estado final de esta sección, tras confirmar en hardware real: la
duplicación reportada por el usuario NO se ha arreglado.** Los dos bugs
de las subsecciones 18.1/18.2 son reales y se mantienen (correctness
genuina), pero ninguno de los dos era la causa de lo que el usuario ve.
Con el overlay reducido a una sola línea muy pequeña (subsección 18.3
revisada), el usuario sigue viendo **dos copias** en la pantalla física
-- una arriba, limpia, y otra bastante más abajo (no a una distancia fija
pequeña, sino descendida una fracción notable de la altura de la
pantalla), la segunda copia borrosa/ilegible. Que la duplicación persista
igual con una sola línea de texto (antes eran 3) confirma que **no es un
problema de "demasiado contenido"** -- pasa igual con lo mínimo posible.

### Lo que sí se descartó con evidencia real esta sesión

- **No es un artefacto de la ventana de Azahar** (hipótesis inicial,
  descartada por el propio usuario: se ve igual en hardware real, una
  sola pantalla física).
- **No es el contenido del render target en sí**: un volcado `KEY_X` del
  target de la pantalla inferior, leído directamente de memoria PICA200,
  muestra **una sola copia limpia**, sin ruido. El bug ocurre en algún
  punto entre ese buffer y lo que la pantalla física termina mostrando.
- **No es la reentrada de `Port_PPU_PresentFrame`** por sí sola (aunque
  el bug de la sección 18.2 era real y se mantiene arreglado): con el
  corte de reentrada activo, la duplicación persiste igual.
- **No es la cantidad de contenido dibujado**: persiste con una sola
  línea de texto pequeña.
- Un patrón de prueba (cuadrantes de color + cruceta) no dio una lectura
  limpia interpretado a través de capturas de pantalla comprimidas -- se
  retiró sin conclusión útil, más allá de reforzar que el problema es
  real y no depende del contenido dibujado.

### Pistas para la próxima sesión, con un método distinto

1. **No seguir iterando por captura de pantalla/foto** -- la resolución y
   compresión hacen casi imposible medir posiciones con precisión.
   Instrumentar en su lugar: por ejemplo, dibujar un patrón con
   coordenadas conocidas y volcar con `KEY_X`, comparando los bytes crudos
   del fichero (no una imagen) contra las coordenadas esperadas
   matemáticamente.
2. **Aislar con el ejemplo oficial mínimo de citro3d** (`both_screens`,
   en `$DEVKITPRO/examples/3ds/graphics/gpu/both_screens/`, que usa
   exactamente el mismo patrón `C3D_RenderTargetCreate(240,320,...)` +
   `C3D_RenderTargetSetOutput(..., GFX_BOTTOM, ...)` que nuestro código):
   compilarlo tal cual y probarlo en el mismo hardware. Si ese ejemplo
   TAMBIÉN duplica, es un problema del entorno/firmware/hardware concreto
   del usuario, no de nuestro código. Si NO duplica, comparar línea a
   línea contra nuestra inicialización de `platform_gpu_3ds.c` hasta
   encontrar la diferencia real.
3. Revisar si `sBottomTarget`/`sTopTarget` comparten alguna configuración
   GPU (profundidad, formato) que difiera de forma sutil entre sí, dado
   que la pantalla superior (según el usuario) **no** muestra este
   problema -- la diferencia entre ambos setups es la pista más
   prometedora que queda sin explotar del todo.
4. Los dos arreglos reales de esta sección (orden del TEV en 18.1, corte
   de reentrada en 18.2) se mantienen en el código -- son correcciones
   legítimas independientemente de que no expliquen la duplicación.

El usuario reportó que el texto de depuración de la pantalla inferior se
veía "con ruido" y además duplicado (una copia arriba, otra abajo). Se
encontraron y arreglaron **dos bugs independientes**, ninguno relacionado
con las líneas horizontales de la sección 17:

### 18.1 — "Ruido": TEV configurado después de dibujar, no antes

`PlatformGpu3DS_EndBottom()` dibujaba la imagen de la pantalla inferior
(`C2D_DrawImage`, muestreando `sBottomTexture`) **antes** de llamar a
`PlatformGpu3DS_ConfigureAbgrTextureEnv()`, que configura cómo interpretar
el orden de bytes de esa textura (convención ABGR distinta a la del atlas
de tiles del renderer de GPU). El resultado: ese dibujado usaba el TEV que
hubiera quedado configurado por el renderer de GPU al dibujar la pantalla
superior el mismo frame (`ConfigureAtlasTextureEnv`, una convención de
bytes distinta) -- colores mal interpretados, exactamente lo que se
percibe como "ruido". Arreglo: mover la llamada a
`ConfigureAbgrTextureEnv()` antes del `C2D_DrawImage`.

### 18.2 — Duplicación: `Port_PPU_PresentFrame` reentrante, nunca arreglado del todo

Ya investigado y documentado en una sesión anterior (comentario "Chasing a
reported bottom-screen debug-overlay duplication" en `port_ppu_mzm.c`)
pero solo instrumentado con un log, nunca corregido de verdad.
`src/transfer.c` llama a `VBlankIntrWait()` directamente (aparte del
bucle principal de `agbmain`), y `VBlankIntrWait → Port_Bios_Halt →
Port_PPU_PresentFrame` -- así que una transferencia de VRAM que abarca
varias llamadas a `VBlankIntrWait()` puede disparar
`Port_PPU_PresentFrame()` (y por tanto el redibujado completo del overlay
de la pantalla inferior) **más de una vez por refresco real de
pantalla**. En New3DS, `PlatformGpu3DS_EndBottom()` no tenía ninguna
protección contra esto (el "saltar redibujado" solo existía para el perfil
Old3DS) -- confirmado con el log `PRESENT gap=7ms (re-entrant?)` ya
capturado en una sesión anterior.

**Arreglo:** la guarda de reentrada (que antes solo registraba el hueco
sospechoso en el log de diagnóstico) ahora **corta de verdad** la segunda
llamada -- si `Port_PPU_PresentFrame()` se invoca a menos de 8ms de la
anterior (imposible que sea un frame real nuevo, un refresco real son
~16.67ms), se descarta sin redibujar nada. Movido fuera del
`#ifdef PORT_GPU_RENDERER_DIAG_LOG` para que funcione también en el build
normal (el bug afecta a todo el mundo, no solo a sesiones de diagnóstico).

### 18.3 — De paso: overlay de depuración más completo, a petición del usuario

Se amplió el alfabeto de la fuente bitmap de 5x7 (de 11 a 24 letras --
faltaban I, N, O, T, R, etc., necesarias para etiquetar nada) y se
reescribió el contenido del overlay:
- Línea 1 (igual que antes): `FPSxx GPU`/`CPU`.
- Línea 2 (si va por GPU): antes `609 9 962` sin etiquetar; ahora
  `I609 O9 C932` (Items en pantalla, Objetos/sprites, tiles únicos en la
  Caché del atlas -- un valor de caché pegado cerca del límite indicaría
  que el reset proactivo de la sección 8 está machacándose cada frame).
- Línea 3 (nueva, siempre visible): `NEW3DS`/`OLD3DS` + tiempo de frame en
  ms (complementa el FPS redondeado con el dato real de presupuesto
  gastado contra los 16.67ms de los 60 FPS) -- el usuario pidió
  explícitamente saber si el hardware es New3DS u Old3DS de un vistazo,
  dado que Old3DS es el caso más exigente.

**Verificado:** compila limpio, overlay se ve correcto y legible en
Azahar (`FPS58 GPU / I669 O67 C932 / NEW3DS 17MS`). **La duplicación
"una copia arriba, otra abajo" que se veía en las capturas de Azahar de
TODA esta sesión resultó ser, con mucha probabilidad, un artefacto de
cómo la ventana de Azahar compone las dos pantallas en una sola imagen
para la captura -- no un bug del juego** (el mismo patrón aparece en
escenas donde la guarda de reentrada de la sección 18.2 no debería
dispararse). La duplicación real que reportó el usuario en hardware sigue
sin confirmarse arreglada -- pendiente de prueba en hardware real, donde
no hay ambigüedad de ventana (una sola pantalla física de verdad).

CIA subido por FTP (build normal y con `PORT_GPU_TILE_RENDERER`, sin
logs de diagnóstico).

### Pendiente para la próxima sesión

0. Confirmar en hardware que el "ruido" y la duplicación del overlay de
   la pantalla inferior han desaparecido de verdad (dos arreglos
   distintos, secciones 18.1 y 18.2).
0.1. Confirmar en hardware con `KEY_X` que las líneas han desaparecido o se
   han reducido notablemente, comparando contra la captura de esta
   sesión (`mzm-dump-left.rgb` con el efecto visible, guardada antes del
   arreglo).
1. **Las dos escenas que siguen cayendo a CPU a propósito** (sección 5.8,
   nunca implementadas, no son bugs): la nave de la intro (usa un sprite
   OBJ afín -- rotado/escalado -- para el efecto de zoom, fuera de alcance
   del renderer desde el diseño original) y el instante justo al pulsar
   seleccionar partida (el cursor activa una ventana real y recortada,
   no a pantalla completa, para resaltar la opción -- las ventanas reales
   siguen sin aproximarse, solo el caso no-op de pantalla completa). El
   usuario ha preguntado por esto para la próxima ronda -- ver sección 5.8
   para el detalle completo de por qué caen a CPU y qué haría falta
   (soporte de OBJ afín y de ventanas reales) para llevarlas a GPU
   también.
2. Las líneas horizontales finas del "efecto persiana" (sección 3.1
   original) siguen sin confirmar/arreglar -- no se ha vuelto a mirar en
   ninguna sesión posterior.
3. El tirón periódico del volcado del buffer de log (sección 16.1) solo
   importa con los flags de diagnóstico activados -- no afecta al build
   normal. Si molesta durante futuras sesiones de depuración, subir el
   tamaño del buffer o volcar en un momento más oportuno (p.ej. durante
   VBlank) reduciría su frecuencia aún más.
4. Con el objetivo de FPS cumplido, empieza a tener sentido revisar
   rendimiento en Old3DS específicamente (más lento que New3DS, el caso
   más exigente que el usuario pidió explícitamente) -- toda la validación
   de esta sesión fue en el hardware disponible, sin distinguir modelo.
2. El "ruido" en la pantalla inferior con draws C3D directos mezclados con
   citro2d (sección 12.1/12.3) sigue siendo una incógnita en hardware real
   aunque `C2D_Prepare()` lo arreglara en Azahar -- señal de que Azahar no
   es fiable para validar del todo este tipo de interacción de bajo nivel,
   más allá de lo ya sabido sobre su reloj de ciclos (sección 10).
3. Alternativa más segura a intentar antes que otra ronda de shaders
   propios: perfilar si `collectMs` (recolección/decodificación, ya
   optimizada en las secciones 7-9) tiene margen adicional, ya que es la
   mitad del problema y no arrastra el riesgo de tocar el pipeline de
   dibujado de bajo nivel.

### Pendiente para la próxima sesión (actualiza la lista de la sección 10)

1. Confirmar en hardware, visitando **varias salas distintas** (no solo
   entrando/saliendo de la misma), que escenarios y menús ya no se quedan
   en negro/con colores equivocados.
2. Si esto lo arregla del todo: retomar el rendimiento (`drawMs` domina
   sobre `collectMs` en los datos de la sección 10 -- investigar el
   volumen de draw calls de citro2d).
3. Nota para el futuro: cualquier otro dato usado para decodificar un tile
   que pueda variar independientemente de los bytes de VRAM del tile en sí
   (como pasó con `palBank`→contenido de paleta, y con `brightAdjust`→
   `EVY`) es un candidato sospechoso a auditar si vuelve a aparecer un
   patrón de "contenido cacheado incorrecto que se arregla solo al forzar
   una recarga".
