# Guía de Arquitectura y Hoja de Ruta para Futuras Sesiones / IAs

**Fecha de creación:** 2026-08-20  
**Proyecto:** *Metroid Zero Mission* - Port Nativo para Nintendo 3DS  
**Rama activa de modernización:** `feat/native-gpu-renderer` (rama base consolidada: `main`)

---

## 1. Estado Actual del Proyecto (Lo que ya funciona al 100%)

Antes de abordar cualquier tarea nueva, es fundamental conocer la base técnica sólida que ya está implementada y verificada en hardware real:

1. **Lógica del Juego y Físicas (100% funcional):**
   * Movimiento de Samus, físicas, colisiones de bloques/clipdata, proyectiles y salto espacial.
   * IA de todos los jefes y enemigos (incluido el fix de **Deorem** en `src/sprite_debris.c`, sección 6m de `docs/3ds-port-status-2026-08-17.md`).
   * Guardado y carga persistente en SRAM (`mzm.sav` en tarjeta SD).
2. **Motor de Audio (100% melódico, ~98% libre de petardeos):**
   * Frecuencias correctas en `port/port_bios.c` (`MidiKey2Freq` usa `waveData[1]`).
   * Cero desfase de audio (eliminado el ringbuffer inflado de 3s).
   * Sintetizador PSG software completo (`port/port_psg_synth.{c,h}`).
   * Avance regular de DMA `gMusicInfo.unk_10 += gMusicInfo.unk_C` cada frame en `src/agbmain.c`.
3. **Cero Material con Copyright en el Repositorio:**
   * La ROM original (`mzm_eu.gba` o `mzm_us.gba`) se carga dinámicamente desde `sdmc:/3ds/Metroid Zero Mission 3DS/` en tiempo de ejecución.

---

## 2. Hoja de Ruta Modular (Plan de Modernización a Nativo)

La modernización del port se realiza de forma **estrictamente modular** para permitir pruebas en hardware en cada paso sin romper nunca el juego.

```
[ Fase 1: Renderer GPU (PICA200) + 3D ] ──► [ Fase 2: Pantalla Táctil / HUD ]
                    │
                    ▼
[ Fase 3: Audio Nativo 16-bit / 32kHz ] ──► [ Fase 4: RetroAchievements (rcheevos) ]
```

---

### Fase 1: Renderer GPU Nativo con `citro3d` / `citro2d` y 3D Estereoscópico (EN PROGRESO)

**Estado real a 2026-08-20 (noche): ver `docs/3ds-port-gpu-renderer-status-2026-08-20.md`
para el detalle completo, léelo antes de tocar `port_gpu_renderer.c`.** Resumen:
sigue detrás del flag `PORT_GPU_TILE_RENDERER` (apagado por defecto, no afecta
al build normal). Se encontraron y arreglaron varios bugs reales serios esta
sesión (canales de color invertidos, transparencia rota, forced blank no
gestionado, mapeo 2D de sprites 8bpp, y el más importante: el buffer de
citro2d se agotaba a las 128 primeras quads, causando que cualquier escena
grande se viera cortada en una franja superior) y se implementó soporte de
blending (BLDCNT) que antes se rechazaba por completo. **Pero el objetivo
principal —que el gameplay real pase por GPU y llegue a 60 FPS— sigue sin
cumplirse**: el gameplay, una vez cargada la partida, se sigue renderizando
por CPU. Quedan además sin diagnosticar: líneas horizontales finas en toda
imagen (sospecha de escalado 1.5x no entero + filtro `GPU_NEAREST`), y el
menú de selección de partida que solo muestra el fondo hasta pulsar un botón
(momento en el que además cae a CPU). Ver el documento de estado para el plan
concreto de la próxima sesión.

* **Objetivo:** Sustituir la PPU por software en CPU (`port_ppu_mzm.c` / `mode1.c`) por renderizado en la GPU PICA200 de la 3DS.
* **Beneficios:**
  * Libera el ~90% del uso de CPU (60 FPS garantizados en Old 3DS y New 3DS).
  * **Efecto 3D estereoscópico real**: profundidad física por capas controlada con el slider 3D.
* **Módulos Técnicos:**
  1. **Atlas de Tiles en VRAM de GPU (`sBgTileTexture`, `sObjTileTexture`)**:
     * Decodificar los bloques de caracteres de GBA (4bpp y 8bpp) a texturas nativas `GPU_RGBA8`.
  2. **Tilemaps como Quads por Capa (BG0..BG3)**:
     * Interpretar `BG0CNT`..`BG3CNT` y registros de scroll (`BG0HOFS`/`BG0VOFS`).
     * Aplicar desplazamiento horizontal estéreo: $dX = Z_{\text{capa}} \times \text{slider3D} \times \text{escala}$.
  3. **Sprites OAM por Hardware**:
     * Recorrer los 128 registros de `gOamMem`.
     * Dibujar los sprites en el plano de acción ($Z = -0.5$).
  4. **Modos de Fusión y Ventanas GBA**:
     * Implementar `BLDCNT`, `BLDALPHA` y `BLDY` usando combinadores de textura (`C3D_TexEnv`) o shaders PICA200.

#### Mapeo de Profundidades Estereoscópicas Sugerido:
| Capa | Elemento en Pantalla | Profundidad Z | Offset Paralaje ($dX$) |
| :--- | :--- | :--- | :--- |
| **HUD / UI** | Contador de vida, minimapa, misiles | $0.0$ | $0.0$ px (plano de pantalla) |
| **BG0** | Niebla, humo, columnas en primer plano | $+1.5$ | $+2.5$ px (hacia fuera) |
| **Samus / Sprites** | Samus, proyectiles, enemigos | $-0.5$ | $-0.8$ px (plano de juego) |
| **BG1** | Plataformas, escenario interactivo | $-0.5$ | $-0.8$ px (plano de juego) |
| **BG2** | Fondos intermedios, cuevas | $-2.0$ | $-3.5$ px (profundidad media) |
| **BG3** | Cielo, estrellas, fondo lejano | $-4.0$ | $-7.0$ px (profundidad infinita) |

---

### Fase 2: Segunda Pantalla (Pantalla Inferior Táctil)

* **Objetivo:** Aprovechar la pantalla táctil de 320x240 para funciones avanzadas sin pausar la acción.
* **Funciones:**
  * Minimapa interactivo en tiempo real con zoom.
  * Selector rápido de armas (Tocar para equipar Misil / Supermisil / Bomba de energía).
  * Menú de estado y configuración táctil.

---

### Fase 3: Motor de Audio Nativo Moderno (C puro)

* **Objetivo:** Desacoplar el audio de los registros emulados de DMA de GBA.
* **Funciones:**
  * Mezclador nativo en C a 16-bit estéreo / 32.768 Hz directo a buffers NDSP.
  * Mayor fidelidad acústica sin conversiones intermedias de 8 bits.

---

### Fase 4: RetroAchievements (`rcheevos`)

* **Objetivo:** Soporte oficial de logros de RetroAchievements (Game ID: 1394).
* **Implementación:**
  * Integrar la librería ligera en C [`rcheevos`](https://github.com/RetroAchievements/rcheevos).
  * Exponer el callback `rc_peek(uint32_t address)` que lee directamente de `gIwram` y `gEwram`.
  * Mostrar notificaciones emergentes de logros desbloqueados en la pantalla inferior.

---

## 3. Reglas y Convenciones para IAs y Desarrolladores

1. **NO introducir código con Copyright**:
   * No incluir gráficos, música ni datos binarios extraídos de la ROM comercial en el repositorio. Todo debe leerse en tiempo de ejecución desde `gRomData`.
2. **Mantener siempre el flujo de compilación limpio**:
   * Compilar siempre con:
     ```bash
     cd platform/3ds
     export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
     export PATH=$DEVKITARM/bin:$HOME/tools/bin:$PATH
     make clean && make -j$(nproc)
     ```
3. **Flujo de pruebas en hardware real por FTP**:
   * Subida de CIA:
     ```bash
     curl --ftp-method nocwd -T platform/3ds/mzm-3ds.cia ftp://192.168.1.138:5000/cias/mzm-zm.cia
     ```
4. **Documentar cada avance**:
   * Registrar cualquier hallazgo, causa raíz o nuevo subsistema en `docs/3ds-port-status-2026-08-17.md`.
