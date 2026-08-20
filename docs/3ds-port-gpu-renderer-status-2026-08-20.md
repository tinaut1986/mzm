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
