# Banco de Capas

Herramienta de escritorio (un solo HTML, sin dependencias ni servidor) para
mirar el juego capa por capa y decidir a mano qué bloques quieres mover de
plano en el 3D estereoscópico del port.

Dos formas de mirar lo mismo:

- **Mapas** — las 315 salas del juego, decodificadas de los datos del repo.
  Sin ROM y sin emular nada. Es el modo de trabajo normal.
- **Grabación** — una escena concreta capturada en la consola, con sus sprites
  y sus registros. Sirve para ver un caso que sólo se da jugando.

## Uso

```bash
python3 tools/layer-workbench/serve.py
```

En Windows el ejecutable se llama `python` a secas; en Debian y Ubuntu, sólo
`python3`.

Genera `maps.json` si hace falta, lo sirve y abre el navegador.

También puedes abrir `index.html` a pelo, pero entonces el navegador no deja
leer `maps.json` desde disco (mismo origen) y hay que soltarlo a mano con el
botón `MAPAS` cada vez. Se acepta tanto `maps.json` como `maps.json.gz`, que
pesa una quinta parte.

Para una grabación, suelta un `mzm-rec.bin` (DEBUG → HERRAMIENTAS → GRAB.
ESCENA en la consola; queda en `sdmc:/3ds/`).

- **El mapa de área**: el mismo de la pantalla de pausa, dibujado de los datos
  del juego (`sMinimapDataPointers`, 32×32 celdas). Pulsa una celda y abre esa
  sala encuadrando esa celda; la sala abierta sale recuadrada. Tres de cada
  cuatro celdas son de una sola sala: cuando una cae en varias, entra en la
  primera y deja las otras a un clic en «también aquí». `−`/`+` cambian su
  tamaño.

  La huella de cada sala lleva dos recortes, los dos comprobados contra el
  mapa que dibuja el juego. Descuenta el borde de la sala, cuyos bloques caen
  antes de `SCREEN_PADDING` y arrastran la huella una celda hacia fuera —por
  eso una sala se marcaba hasta pasada su puerta y las celdas de dos salas
  vecinas salían marcadas en las dos—, y descuenta las celdas sin aire: los
  datos de una sala llegan más lejos que su parte jugable, y esa roca de
  relleno el juego no la dibuja. En Kraid 7 las dos celdas que el mapa deja en
  blanco son justo las que tienen 0% de aire. Entre los dos recortes, las
  celdas marcadas que el mapa deja en blanco pasan del 11% al 1% y los solapes
  de 3713 a 256.
- **Izquierda**: las salas del área elegida, o las capturas de la grabación.
- **Centro**: el resultado final con tus correcciones ya aplicadas y, debajo,
  cada capa por separado — arriba como está, abajo como quedaría.
- **Pulsa un bloque** en cualquier capa (o en el resultado) para seleccionarlo.
  No se mueve nada: **lo marcan todas las vistas**: relleno y con borde
  grueso en la capa a la que pertenece, a trazos y atenuado en las demás. Vale
  también pulsar donde esa capa no tiene nada: se marca la posición igual, que
  es media faena para decidir a qué plano mandar algo. `↑↓←→` mueve la
  selección, y la vista sólo se desplaza si se ha salido, lo justo para volver
  a verla.
- **Arrastra sobre el lienzo** para coger el rectángulo que dibujes; mientras
  arrastras se ve lo que va a coger. `Ctrl` suma lo nuevo a lo que ya hubiera,
  tanto en un clic suelto como en un rectángulo. El destino que elijas se
  aplica a todo el conjunto de una vez.
- **`COPIAR INFO`** vuelca lo seleccionado como texto: la sala, sus prioridades
  BGCNT, su mezcla alfa, y qué tiene **cada** capa en esa posición, incluidas
  las vacías. Es lo que hace falta para razonar sobre un caso sin tener la
  herramienta delante.
- **`May`+arrastra para recorrer la sala**, ya que arrastrar a secas
  selecciona. Las vistas se mueven juntas, lo mismo que la que arrastras: no comparten una posición absoluta, porque
  tienen tamaños distintos y por tanto recorridos distintos. Una vista que ya
  esté en su tope no arrastra a las demás, y arrastrar no cambia la selección.
- `Ctrl`+rueda o los botones `−`/`+` cambian el zoom, sin perder el punto que
  estuvieras mirando.
- **Pulsa una corrección de la lista** y te lleva a su sala y su bloque, aunque
  esté en otra área.
- **Todo son ventanas flotantes** —el mapa, el resultado y cada capa—: se
  arrastran por la cabecera a cualquier punto de la pantalla y se redimensionan
  por cualquiera de sus cuatro bordes o cuatro esquinas. Al agrandarlas, lo que
  crece es el lienzo. La que arrastras pasa al frente mientras dura el gesto.
  Cada ventana de capa rota entre `AMBAS`, `ANTES` y `DESPUÉS` con el botón de
  su cabecera; con una sola vista el lienzo ocupa el doble y lo que decía la
  franja de en medio pasa a la cabecera. Cada una se
  minimiza con `—` a la barra de abajo y se restaura pulsándola allí; el resultado además se maximiza con `▣`, y entonces se va al fondo para
  poder seguir consultando las capas por encima. Las pestañas de los separadores pliegan las
  barras laterales, y `RECOLOCAR` reparte respetando las que estén abiertas.
  Las barras, sus separadores y la barra de minimizadas quedan siempre por
  encima de las ventanas: éstas se renumeran dentro de un rango acotado, en vez
  de subir de una en una hasta pasarse. Siempre
  queda un trozo de ventana dentro de la pantalla, así que no hay forma de
  perder una. La colocación aguanta entre salas y entre sesiones.

Las vistas se sincronizan por el centro, no por la esquina: el escenario y las
tarjetas de capa tienen alturas distintas, y compartiendo la esquina superior
izquierda cada una acabaría enseñando un trozo distinto de la sala.
- **Elige destino** en el panel derecho. El bloque se dibuja al nivel de esa
  capa, ENCIMA de ella; para dejarlo detrás de una capa, mándalo a la anterior.

## Qué capas salen

En modo mapa salen **BG0, BG1 y BG2**: son las que la sala define como mapas de
bloques RLE, dibujados con el tileset. BG0 lo tienen 153 salas.

No salen:

- **BG3** (302 salas) y los 8 BG0 que van comprimidos en LZ77. Ésos no son
  mapas de bloques sino tilemaps crudos, y se dibujan con los gráficos de fondo
  del tileset (`pBackgroundGraphics`), no con sus bloques: hacen falta otro
  decodificador y otro juego de gráficos. BG3 es además siempre la capa del
  fondo, con prioridad 3 fija.
- **Los sprites**, que no están en los datos de mapa en absoluto: se generan al
  jugar desde los spritesets de la sala. Sólo se ven en una grabación.

Las dos siguen valiendo como destino de una corrección aunque no se dibujen.

## Mezcla alfa

172 salas mezclan una capa con la escena en vez de dibujarla encima
(`TransparencySetRoomEffectsTransparency`, `src/transparency.c:170` y `:305`).
El visor lo aplica, y hace falta: el BG0 de Crateria 1 son 4096 bloques
iguales, todos negro sólido, y sin la mezcla taparía la sala entera. Con sus
coeficientes (EVA 13, EVB 16) el resultado es la escena tal cual — es una capa
de efecto, no de dibujo. En el otro extremo hay salas con EVB 0, donde el BG0
sí tapa del todo.

Los coeficientes de cada sala salen en la barra de arriba y en la cabecera de
la capa que mezcla. Un bloque que muevas se dibuja sin mezclar: la corrección
cambia su plano de profundidad, no cómo se mezcla.

El orden de las capas **no es fijo**: lo decide el campo `transparency` de la
sala (`TransparencySetRoomEffectsTransparency`, `src/transparency.c:89`), y de
ahí salen las prioridades BGCNT que se ven en cada recuadro. En 271 salas BG0
va delante de todo, en 40 va delante BG1, y en 2 de Chozodia BG0 acaba por
detrás de BG2. Importa porque es exactamente de esa prioridad de donde el port
deriva la profundidad estéreo (`PortStereoDepth_BgTier`).

## El archivo de correcciones

`GUARDAR .INC` escribe `port_layer_fixes.inc` directamente en disco (en
navegadores con File System Access; en el resto cae a una descarga normal).
`CARGAR .INC` lo lee de vuelta, así que la lista se edita entre sesiones y no
hay forma de duplicar una entrada: la clave es `(área, sala, capa, blockX,
blockY)` y una segunda asignación al mismo bloque sustituye a la anterior.

```
PORT_LAYER_FIX(area, room, capaOrigen, blockX, blockY, block, capaDestino)
```

`blockX`/`blockY` son la posición del bloque **en la sala**, la misma que usa
`pDecomp` en `RoomUpdate*Tilemap` (`src/room.c:783`). Deliberadamente no son
coordenadas del screenmap: ése da la vuelta cada 32×16 bloques, y 173 de las
315 salas son más anchas de 32 bloques y 177 más altas de 16, así que una
clave de screenmap señalaría a varios bloques a la vez en más de la mitad del
juego.

Cada línea lleva el valor del bloque como checksum. Si los datos de la sala
cambiaran, la corrección falla en voz alta en vez de mover otro bloque. Es
además lo que el port tiene en la mano justo donde tendría que aplicarla, al
transferir bloques de sala.

Una grabación enseña coordenadas de screenmap, así que para escribir la clave
hay que deshacer la vuelta. Eso lo hace el visor con el origen de pantalla que
la propia grabación guarda, y **lo comprueba** contra los datos de la sala: el
bloque de esa posición tiene que producir justo la entrada de tilemap que se
ve. Por eso en modo grabación hay que tener `maps.json` cargado para poder
corregir; sin comprobar, un BG con parallax podría caer en la vuelta
equivocada sin que se notara.

## Regiones

Las correcciones valen para US, EU y JP: los tilemaps de sala
(`data/rooms/<área>/<área>_<n>_bg1.gfx`) no tienen condicionales de región, y
en `src/data/rooms_data.c` sólo hay cinco, sobre efecto de ácido y pista de
música. La numeración de salas y las coordenadas de bloque son las mismas.

## Los datos

`build_maps.py` recorre `src/data/rooms_data.c` y los archivos sueltos y emite
`maps.json`: 315 salas en siete áreas, los 69 tilesets que usan, los tiles
comunes y los mapas de área.

Los gráficos del minimapa son 384 tiles, no los 160 que declara
`sMinimapTilesGfx`: el DMA de la pantalla de pausa copia 0x3000 bytes y se sale
al array siguiente en ROM (`sPauseScreen_40f4c4`), que es justo lo que falta
para completarlos. Sin esa segunda mitad, las celdas especiales —estaciones de
guardado y de mapa, objetos, los nombres de área— salen negras. Todo transcrito de los cargadores del propio juego, con la referencia
al lado (ver la cabecera de `mzmdata.py` y los comentarios de `build_maps.py`).

El número de área que va al `.inc` sale de `maps.json`, que lo toma del orden
de la enum `Area` (`include/constants/connection.h`), para que no haya una
segunda lista que se pueda desincronizar.

`make_test_rec.py` fabrica un `mzm-rec.bin` sintético de una sala de 64×64
bloques. Es lo único que comprueba que el desenvuelto de coordenadas acierta,
porque una sala pequeña no daría la vuelta y no probaría nada.

## Dónde va el .inc

En `platform/3ds/source/port_layer_fixes.inc`, y **no hay que moverlo a mano**.
Arrancando con `serve.py`, el visor lo lee de ahí al abrir, y `CARGAR` y
`GUARDAR` trabajan contra esa misma ruta: `CARGAR` lo relee —útil si lo has
tocado por fuera— y `GUARDAR` lo escribe. No se guarda solo; la insignia de la
barra avisa cuando hay cambios sin guardar. La escritura es atómica, para que
un guardado a medias no deje al port compilando una lista truncada.

Abriendo `index.html` a pelo nada de esto es posible —el navegador no escribe
en disco sin diálogo— y los dos botones caen al diálogo de archivos de siempre.

Ahí lo recoge
`port_layer_fixes.c`, que lo compila como tabla y valida cada entrada contra el
tilemap de la sala al entrar en ella: una corrección cuyo bloque ya no coincide
se descarta en vez de mover otro bloque.

El include es **opcional** (`__has_include`). Sin el archivo, la tabla queda
vacía, el render se salta la búsqueda y el port se comporta exactamente como
antes — así que una build con correcciones y otra sin ellas se diferencian sólo
por ese archivo.

## Pendiente

No están cosidas las salas en un mapa de área completo. `mapX`/`mapY` de
`RoomEntry` no sirven: colocarlas en una rejilla fija deja entre 29 y 58 pares
solapados por área a cualquier tamaño de celda. La fuente correcta son las
tablas de puertas (`sBrinstarDoors`, `rooms_data.c:667`), donde cada puerta
lleva su rectángulo de bloques en la sala de origen y la puerta de destino, y
eso fija dos salas una respecto a otra exactamente.

## Nota

El área y la sala sólo están en grabaciones con magic `MZM4`. Una anterior
carga igual, pero deja esa mitad de la clave sin rellenar y la herramienta lo
avisa en rojo.
