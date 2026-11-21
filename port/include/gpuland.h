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
extern volatile int port_gl_ok;         /* the card has drawn the ground (its pictures are shown) */

/* while a terrain polygon is drawn into the sharp canvas: each pixel the lit mappers write is
 * noted at port_gl_sh[pixel - port_gl_sh_base] as 0x100 | its colour (src/2d/perloop.c,
 * fl8lin.c); NULL otherwise.  A pixel still holding that colour at the end shows the ground */
extern uint16_t *port_gl_sh;
extern uint16_t port_gl_sh_val;         /* ... noted as this | the colour: 0x100 the ground, 0x200 an object */
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

/* ... and its weather (kind -1 none, 0 rain, 1 snow; the game's drop colour, how many drops
 * (1 the game's usual), the drift across the view a step of the fall, the time in seconds, the
 * viewer on the map), which the card draws in place of the game's drops */
void port_gl_weather(int kind, int colour, float density, float wind, float time, float cam_x, float cam_y);
extern int port_gl_wx_on;              /* the card draws the weather (sharp3d.c leaves the drops out) */

/* the game's thread (weather.c): a terrain polygon of n points, textured (tex: 64 x 64 colour
 * numbers) or flat (tex NULL, colour), through the light table ltab (24 x 256; NULL: unlit) */
void port_gl_poly(const PortGlVert *v, int n, const uint8_t *tex, int colour, const uint8_t *ltab);

/* the game's thread (src/3d/tmap.c): a textured polygon of an object (a unit, a building...) as
 * the 3D library draws it into the sharp canvas: its points on the canvas (sx, sy in pixels) and
 * their depth (z), texels (u, v) and light; its texture w x h colour numbers (key: the same
 * texture's), see-through where 0 (trans), through the light table ltab (NULL: unlit).  The card
 * draws the objects in the order they come, as the game does (far to near) */
typedef struct {
    float sx, sy, z, u, v, shade;
} PortGlObjVert;
void port_gl_obj(const PortGlObjVert *v, int n, const uint8_t *bits, int w, int h, int trans,
                 const uint8_t *ltab, const void *key);
/* ... drawing into the sharp canvas (not a picture of its own: the water's reflections) */
int port_gl_obj_canvas(const void *bits);

/* the game's thread (port/mods/effects.c): an effects pack's picture (w x h RGBA, key: the same
 * picture's) where the game places the sprite on the sharp canvas (x0, y0 .. x1, y1, mirrored
 * left for right), at op (0..1) of its alpha, lit by its light row's fit (colour * a + b: fit[0..2]
 * a, fit[3..5] b; NULL unlit).  1: the card draws it (the caller notes the pixels it would cover
 * in port_gl_fxsh, as 0x100 | the canvas's colour there, and draws nothing); 0: the game's */
int port_gl_fx(const uint8_t *rgba, int w, int h, const void *key, float x0, float y0, float x1, float y1,
               int mirror, float op, const float *fit);
extern uint16_t *port_gl_fxsh;

/* the display's thread (platform.c): the frame's capture drawn on the renderer's graphics card;
 * pal the game's colours (0xAARRGGBB).  NULL when there is nothing to show (no capture, not the
 * GPU renderer, no DXIL shaders) */
struct SDL_Renderer;
struct SDL_Texture *port_gl_render(struct SDL_Renderer *r, void *capture, const uint32_t *pal);
void port_gl_shutdown(void);

/* the display's thread, after port_gl_render drew the capture: what the picture's 3D showed (W x
 * H: 1 the ground, 2 an object the card drew, 0 the game's own; 4 an effect over it) << 8 | the
 * canvas's colour then, from row `row` of the canvas on; NULL when the card has no picture of it.
 * A pixel whose colour has changed since (the target's label, a HUD picture drawn over the view
 * after the 3D) is the game's */
const uint16_t *port_gl_mask(void *capture, int row);
#define PORT_GL_K(m, c) ((unsigned char)(m) == (c) ? (m) >> 8 : 0)     /* the mask's kind, if still c */

/* the alpha the display gives the pixels where the ground goes (their colour kept; an object's
 * one less: PORT_GL_MARK_A - 1), and the
 * render state that puts the card's ground there as a texture of the picture is drawn: its texel
 * (u, v) is the card picture's (u * sx, v * sy + oy); the pixels' alpha then a (the 3D's).  NULL:
 * none */
#define PORT_GL_MARK_A 253
struct SDL_GPURenderState *port_gl_state(struct SDL_Renderer *r, float sx, float sy, float oy, float a);

#endif
