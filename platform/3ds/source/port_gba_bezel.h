#ifndef PORT_GBA_BEZEL_H
#define PORT_GBA_BEZEL_H

#include <stdbool.h>
#include <citro3d.h>

typedef enum {
    GBA_BEZEL_MODE_NONE = 0,
    GBA_BEZEL_MODE_FULL,  /* Pixel Perfect: full 400x240 frame with 240x160 cutout */
    GBA_BEZEL_MODE_SIDES, /* Scaled Original (360x240): left/right 20px strips */
} GbaBezelMode;

/* Initialize the GBA bezel texture. Decompresses the embedded asset once into GPU memory. */
void PortGbaBezel_Init(void);

/* Free the bezel texture resources. */
void PortGbaBezel_Shutdown(void);

/* Returns true if the bezel texture was successfully initialized. */
bool PortGbaBezel_Ready(void);

/* Returns true if the GBA bezel should be rendered. */
bool PortGbaBezel_Active(void);

/* Returns the current bezel display mode. */
GbaBezelMode PortGbaBezel_GetMode(void);

/* Render the GBA bezel quad over the target. Transparent inner window discards fragments. */
void PortGbaBezel_Draw(C3D_RenderTarget* target);

#endif /* PORT_GBA_BEZEL_H */
