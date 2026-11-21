/* screen.h -- screen filters (screen.c) */
#ifndef TN_SCREEN_H
#define TN_SCREEN_H
#include <stdio.h>
#include <stdint.h>
#include <SDL3/SDL.h>

void port_screen_load(void (*get)(const char *key, char *out, int size));
void port_screen_save(FILE *f);         /* screen_<name>= lines, only the changed ones */
void port_screen_filters_off(void);     /* the Filters page's settings as the original */
/* the display (platform.c, main thread) */
int port_screen_gpu_wanted(void);       /* a filter needs SDL's "gpu" renderer */
void port_screen_gpu_unavailable(void); /* ... and it could not start */
void port_screen_renderer_gone(void);   /* before the renderer is destroyed */
unsigned port_screen_serial(void);      /* changes when the colour settings change */
void port_screen_grade(const uint32_t *pal, uint32_t *out);
void port_screen_grade_rgb(const uint8_t *rgb, uint32_t *out, int n);   /* the same for RGB pixels */
void port_screen_grade_lut(uint8_t lut[256], int *sat);  /* ... per channel, then the colour % */
void port_screen_rect(int w, int h, int ww, int wh, SDL_FRect *dst);
/* "Movie size: Fill screen": the rect to draw the game's w x h screen at so that its movie
 * (mx, my, mw, mh) fills the ww x wh window; 0 with Original or a movie that fills it already */
int port_screen_movie_rect(int w, int h, int ww, int wh, int mx, int my, int mw, int mh, SDL_FRect *dst);
void port_screen_draw(SDL_Renderer *r, SDL_Texture *t, int w, int h, const SDL_FRect *dst);
/* "Widescreen": the main menu at 16:9 (menuwide.c), PORT_MENU_WIDE_E columns each side of its 640 */
#define PORT_MENU_WIDE_E 107
int port_menu_wide_up(const uint8_t *scr, int pitch);  /* the 640 x 480 screen shows the main menu */
void port_menu_wide_fill(void *out, int opitch, const uint8_t *scr, int pitch, const uint32_t *pal);
int port_screen_fxaa_wanted(void);     /* "Anti-aliasing: FXAA" on (and the GPU renderer works) */
SDL_Texture *port_screen_fxaa(SDL_Renderer *r, SDL_Texture *src, int w, int h);   /* its pass */
/* the settings screen.c keeps, as the options screen's lines (options.c puts them on its pages) */
enum { S_SHARP3D, S_WIDE, S_AA, S_FOG, S_TRUECOL, S_MAP, S_EFFECTS, S_OBJPACK, S_CKPACK, S_HMOTION, S_ARMS, S_INFRARED,
       S_SCALE,
       S_VIDEOS, S_MOVIES,
       S_TERRAIN, S_SMOOTH, S_WATER, S_RIPPLES, S_WATERFX,
       S_OBJECTS, S_ENEMIES, S_TREESIZE, S_BURN, S_GPULAND,
       S_BRIGHT, S_GAMMA, S_CONTRAST, S_COLOUR, S_SCAN, S_EFFECT, S_RESET, S_COUNT };
int port_screen_usable(int i);          /* 0: the line needs another setting first (drawn grey) */
void port_screen_preset(int p);         /* a preset's values: 0 Classic, 1 Vanilla+, 2 Freefall */
int port_screen_is_preset(int p);       /* the settings are preset p's */
int port_screen_is_value(int i);        /* a setting (not the Reset line) */
const char *port_screen_label(int i);
const char *port_screen_help(int i);
const char *port_screen_help_v(int i, int v);     /* for value v (-1: the one it has) */
void port_screen_value(int i, char *buf, size_t n);
void port_screen_change(int i, int dir);
int port_screen_get(int i);             /* a choice's button index, or a filter's number */
void port_screen_set(int i, int v);
int port_screen_range(int i, int *lo, int *hi, int *step);  /* a filter's; 0 for others */
#endif
