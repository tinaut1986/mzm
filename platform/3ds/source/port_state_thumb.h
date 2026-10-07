#ifndef PORT_STATE_THUMB_H
#define PORT_STATE_THUMB_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Screenshot stored with each save state, shown in the state's detail modal.
 * Landscape RGB565, PORT_THUMB_W x PORT_THUMB_H: the top screen's last
 * rendered frame at half size. No <3ds.h> here so the GBA-side save state
 * code can include this header. */
#define PORT_THUMB_W 200
#define PORT_THUMB_H 120
#define PORT_THUMB_PIXELS (PORT_THUMB_W * PORT_THUMB_H)

/* Reads the top screen's left-eye target back from the GPU and fills `out`
 * (PORT_THUMB_PIXELS entries). False if the renderer is not up. */
bool PortStateThumb_Capture(uint16_t* out);

/* Uploads `pixels` (PORT_THUMB_PIXELS RGB565 entries) as the picture the modal
 * shows; NULL clears it. Cheap enough to call when a modal opens. */
void PortStateThumb_Set(const uint16_t* pixels);
bool PortStateThumb_HasImage(void);

/* Draws the current picture into the bottom screen's citro2d scene. */
void PortStateThumb_Draw(float x, float y, float w, float h, float depth);

#ifdef __cplusplus
}
#endif

#endif /* PORT_STATE_THUMB_H */
