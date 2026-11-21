/* gpuland.h -- the GPU ground (port/video/gpuland.c): the main sharp view's terrain polygons
 * caught while the game draws them, drawn again on the graphics card. */
#ifndef GPULAND_H
#define GPULAND_H

#include <stdint.h>

/* a polygon's point as the terrain's polygon routines get it: view space (x right, y down, z
 * ahead, the game's units / 65536), the texture's texel (0..64) and the light (the light table's
 * row, 0..23) */
typedef struct {
    float x, y, z, u, v, shade;
} PortGlVert;

extern int port_gl;                     /* FREEFALL_GPU_LAND: 1 in the view, 2 alone (testing) */
extern int port_gl_capturing;           /* the main view's sharp drawing is being caught */

/* while a terrain polygon is drawn into the sharp canvas: each pixel the lit mappers write is
 * noted at port_gl_sh[pixel - port_gl_sh_base] as 0x100 | its colour (src/2d/perloop.c,
 * fl8lin.c); NULL otherwise.  A pixel still holding that colour at the end shows the ground */
extern uint16_t *port_gl_sh;
extern uint16_t *port_gl_shc;          /* ... the far land's (colras.c, haze.c): the whole drawing */
extern const uint8_t *port_gl_sh_base;
void port_gl_shadow(int on);            /* weather.c: around a terrain polygon routine */

/* the game's thread (sharp3d.c): around the main view's sharp drawing; *slot is the frame's own
 * capture (made on the first use), canvas its pixels, W x H, half_w / half_h its camera's */
void port_gl_begin(void **slot, const uint8_t *canvas, int W, int H, long half_w, long half_h);
void port_gl_end(void);

/* the game's thread (sharp3d.c), before port_gl_end: the far land beyond the polygons, from the
 * map (port/video/gpufar.c) */
void port_gl_far(void);

/* ... with the view: the game's view matrix (cell offsets ey, height, ex to view space, rows)
 * and offset, and its fog (start, end in squares, its colour; fog 0: none) for each pixel */
void port_gl_view(const float m[9], const float mz[3], int fog, float start, float end, int colour);

/* the game's thread (weather.c): a terrain polygon of n points, textured (tex: 64 x 64 colour
 * numbers) or flat (tex NULL, colour), through the light table ltab (24 x 256; NULL: unlit) */
void port_gl_poly(const PortGlVert *v, int n, const uint8_t *tex, int colour, const uint8_t *ltab);

/* the display's thread (platform.c): the frame's capture drawn on the renderer's graphics card;
 * pal the game's colours (0xAARRGGBB).  NULL when there is nothing to show (no capture, not the
 * GPU renderer, no DXIL shaders) */
struct SDL_Renderer;
struct SDL_Texture *port_gl_render(struct SDL_Renderer *r, void *capture, const uint32_t *pal);
void port_gl_shutdown(void);

/* the display's thread, after port_gl_render drew the capture: where the picture shows the ground
 * (W x H, 1 = ground), from row `row` of the canvas on; NULL when the card has no picture of it */
const uint8_t *port_gl_mask(void *capture, int row);

/* the alpha the display gives the pixels where the ground goes (their colour kept), and the
 * render state that puts the card's ground there as a texture of the picture is drawn: its texel
 * (u, v) is the card picture's (u * sx, v * sy + oy); the pixels' alpha then a (the 3D's).  NULL:
 * none */
#define PORT_GL_MARK_A 253
struct SDL_GPURenderState *port_gl_state(struct SDL_Renderer *r, float sx, float sy, float oy, float a);

#endif
