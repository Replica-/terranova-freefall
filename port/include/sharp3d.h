/* sharp3d.h -- "3D resolution" (port/video/sharp3d.c): the main 3D view drawn a second time at 2x to
 * 6x the original pixels, for the display only, and laid under the game's own screen. */
#ifndef TN_SHARP3D_H
#define TN_SHARP3D_H

#include <stdint.h>

/* the setting: 0 = Original, 2, 4 or 6 = that many times the pixels */
extern int port_sharp3d;
extern volatile int port_s3d_out_h;     /* height of the game's picture in the window (display) */
/* the whole screen's size the 3D is drawn at, with setting s on a scr_h (200 or 400) screen */
void port_s3d_size(int s, int scr_h, int *w, int *h);

/* "Widescreen": 0 = Off, 1 = Fit window: in a mission the whole game screen is stretched evenly
 * to the window's shape (up to 16:9; the cockpit's or the goggles' art, the HUD around the view),
 * with the main 3D view drawn behind it as wide as its stretched window at the right proportions,
 * port_s3d_wide_e more of the game's columns on each side of the screen (worked out by the display
 * from the window), the game's own picture in its middle (so what the HUD draws over the 3D, the
 * reticle, stays on its units).  The game sets port_s3d_stretch while the mission's screen is up
 * and names the main view's widget (its stencil: the window's shape) before drawing it. */
extern int port_widescreen;
/* "Terrain distance" (0 Low = the game's, 1 Medium, 2 High): the smooth ground farther out in the
 * picture drawn for the display; the game's own view stays as it is.  "Show FPS" (options.c,
 * drawn by platform.c).  "Water reflection" 0 Low (the original), 1 Medium, 2 High (rndsky.c, the sharp view only). */
extern int port_terrain, port_show_fps, port_water_full, port_water_ripples;
extern int port_water_fx;               /* "Water effects": 0 Off, 1 On (rndsky.c) */
/* "Terrain smoothing" (off / on): the far land's columns textured like the near ground, their
 * light and haze blended from point to point (haze.c, the sharp view only) */
extern int port_far_smooth;
extern int port_obj_dist, port_enemy_dist;
extern int port_map_smooth;             /* "Map smoothing": 0 Off, 1 On (mapdisp.c) */
extern int port_map_cone;               /* "Map view cone" (mapcone.c) */
extern int port_map_radar;              /* "Radar without MFD" (mapcone.c) */
/* "Effects pack" (fxpack.c loads it, effects.c draws it): a mod's Effects folder, by the mod's
 * name ("" = off), whose pictures the sharp view draws for the game's effect animations;
 * port_fx_update loads a changed one (the start of a sharp drawing), port_fx_packs lists the
 * mods that have one */
extern char port_fx_pack[64];
extern char port_mods_dir[1024];        /* the Mods folder next to tnfree.exe (main.c) */
void port_fx_update(void);
int port_fx_packs(char out[][64], int max);
/* a pack's folder: the mod's Mods/<name>/<kind> when it is a folder, else Mods/<name>.zip/<kind> */
const char *port_pack_dir(const char *kind, const char *name, char *out, size_t n);
int port_mod_sets(char out[][64], int max);     /* the mods: zips (and folders) in Mods/ holding packs */
void port_mod_set_info(const char *set, char *out, size_t n);  /* its mod.txt as one line, or "" */
/* modzip.c: mod paths through a .zip ("Mods/Freefall.zip/Effects/1171/01.png") read from the zip */
void *port_mod_load(const char *path, size_t *len);    /* a file's bytes (SDL_free), disk or zip */
int port_mod_type(const char *path);                   /* 0 nothing, 1 a file, 2 a folder */
int port_mod_list_zip(const char *path, int what, char out[][64], int max); /* -1: not in a zip */
int port_zip_files(const char *path, char out[][256], int max);  /* a zip's files; -1: no zip */
/* "Objects pack": a mod's Objects folder ("" = off) whose pictures the sharp view draws for
 * the trees, bushes and rocks (port_fx_update loads it too), port_obj_packs lists the mods */
extern char port_obj_pack[64];
int port_obj_packs(char out[][64], int max);
/* "Tree size" (0 Original, 1 Realistic): the sharp view draws each tree, bush and rock 0.75 to
 * 1.5 times its size, by its place on the map (port_tree_scale, drawobj_29ACA8) */
extern int port_tree_size;
/* "Tree explosion" (0 Original, 1 Realistic): a tree or bush that dies bursts on its own picture in
 * the sharp view (burn.c) */
extern int port_burn;
/* widescreen: zoom level zoom of a game map display shows no more than the land's width
 * (src/game/hud/mapdisp.c) */
int port_map_zoom_ok(const void *map, long zoom);
/* "Fog" (0 Off, 1 On; fog.c): port_fog_begin (with the game's camera: its sky; and its canvas's
 * colour table in the game's infrared, else NULL) / port_fog_end around each sharp drawing */
extern int port_fog;
extern int port_ir_style;               /* "Infrared": 0 Original, 1 Sci-fi (infrared.c) */
int port_ir_on(void);                   /* ... on, and the game's infrared showing */
int port_ir_showing(void);              /* the game's infrared showing (either look) */
int port_ir_heat(int r, int g, int b, float t0, float t1);   /* a colour in its ramp by brightness */
void port_ir_palette(uint32_t *pal);    /* the display's colours with the thermal ramp */
void port_ir_sensor(uint8_t *p, int W, int H, int kx, int ky);  /* the sharp view's picture */
void port_fog_begin(const void *cam, const void *clut);
void port_fog_end(void);
/* "True colour" (0 Off, 1 On, 2 On + sun glow; truecol.c): port_tc_begin before the main view's
 * sharp drawing into picture idx (of its three) high, W x H; port_tc_end after it, giving the
 * picture's full colours (port_s3d_frame tc) or NULL, and the rows that have them */
extern int port_truecol;
int port_tc_mode(void);                 /* truecol.c: the True colour in use (GPU rendering: On) */
void port_tc_begin(int idx, uint8_t *high, int W, int H);
unsigned long *port_tc_end(const uint8_t **rows);
extern unsigned long port_tc_pal[256];  /* the game's colours (RGB) of the drawing (port_tc_begin) */
extern volatile unsigned port_s3d_us;   /* the main view's last drawing time (microseconds) */
int port_s3d_wanted(void);              /* any of the settings that draw the 3D again is on */
extern volatile int port_s3d_wide_e;
extern volatile int port_s3d_stretch;
int port_s3d_stretch_up(void);          /* ... and no menu over it (display) */
void port_s3d_window(const void *sten, int x0, int y0, int x1, int y1);
/* the art drawn over the main view each frame (helmet.c: the cockpit's frame, the goggles'):
 * w x h at (x, y) of the screen, non-zero where it covers the 3D; NULL: none.  Kept, not copied */
void port_s3d_overlay(const uint8_t *mask, int w, int h, int x, int y);
/* the targets' brackets and labels the HUD draws over the main view (hudmain.c), drawn on a
 * layer as wide as the wide view instead: begin pushes its canvas and gives the game's columns
 * left of the game's view on it (-1: no wide view this frame, nothing pushed), end pops it */
int port_s3d_hud_begin(void);
void port_s3d_hud_end(void);
/* a piece of the current target's label (hudmain.c), as GfxDrawText(font, s, ...) on the current
 * canvas with the point (x, y) of its box at ax, ay (in halves: 0 left / top, 1 the middle,
 * 2 right / bottom), each line centred in it when centre.  With a sharp picture of 2x or more it
 * is drawn into that at 3/4 of the game's size (the game's own text on its screen shown as the
 * 3D there; not drawn on the wide view's layer).  port_target_label: "Target label", 0 the
 * game's block, 1 around the box */
void port_label_piece(int font, char *s, int x, int y, int ax, int ay, int centre);
extern int port_target_label;
extern int port_hud_noclamp;            /* drawobj.c: a unit's point beyond the canvas too */
/* the display draws the game's pointer itself (the stretched screen); the game draws none */
extern volatile int port_wide_cursor;
int port_cursor_shown(void);            /* the game shows its pointer (not hidden) */
int port_hud_goggles(void);             /* the full-screen goggles are up (not the cockpit) */
void port_player_place(long x, long y, unsigned heading);  /* the benchmark's moves (bench.c) */
int port_game_paused(void);             /* the game is paused (P) */
void port_cursor_pos(int *x, int *y);   /* where the game has it */

/* Widescreen, the mouse beyond the game's picture over the wide view: the game
 * gets the pointer at the screen's edge, the HUD takes the real position from here (aiming,
 * firing, clicking a unit to target it: hudmain.c, helmet.c) and the display draws the pointer
 * (platform.c).  Set by the display (main thread), read by the game. */
extern volatile int port_wide_ptr;      /* 1 while the pointer is beside the screen */
extern volatile int port_wide_ptr_x, port_wide_ptr_y;   /* where, in the game's pixels */
/* ... the cockpit's side screens' maps (port_s3d_map): wider than the game's at the right
 * proportions; the pointer over one beyond the game's map: its slot (1 + the view; 0 none), the
 * game has the pointer at the map's edge, the real place is port_wide_map_x / _y */
extern volatile int port_wide_map, port_wide_map_x, port_wide_map_y;

#define PORT_S3D_EY 5                   /* the layered helmet: the view's rows more above and below
                                         * (game rows of a 200-row screen) */

/* the units the wide view drew (every one, also those in the middle): their boxes in the
 * game's pixels of the main view, for clicks and brackets beside the screen */
typedef struct {
    short x0, y0, x1, y1;
    short ent;
    short px, py;                       /* its point the HUD's brackets and label go around */
    uint8_t col;                        /* bracket colour (0: none), as the HUD's */
    uint8_t cur;                        /* the current target */
} port_wide_targ;
#define PORT_WIDE_TARGS 64
int port_wide_targets(port_wide_targ *out);     /* the newest list: count */
/* game thread, while the wide view draws (viewdist.c via hudmain.c): a unit's box and point on
 * the wide canvas */
extern int port_s3d_targ_rec;
void port_wide_target(const short *rect, const short *pt, int ent, int col, int cur);
/* display: the game's pointer bitmap (sup_c.h tnm_cursor); 0 if none */
int port_cursor_get(const uint8_t **bits, int *w, int *h, int *row, int *type, int *hx, int *hy,
                    int *color);

/* A finished picture of the main 3D view, ready to be shown with the screen it was drawn for. */
#define PORT_S3D_LABS 8
typedef struct {
    int x, y, w, h;                     /* a piece of the target's label drawn into high */
    size_t m;                           /* ... its text's pixels: w x h at lab_m + m, 1 = text */
} port_s3d_lab;
typedef struct {
    int valid;
    int scr_w, scr_h;                   /* the game's screen mode */
    int x, y, w, h;                     /* the view on the game's screen */
    int kx, ky;                         /* picture = (kx * w + 2 * ex) x (ky * h) */
    int ex;                             /* widescreen: the picture's columns on each side */
    int ey;                             /* the layered helmet: the picture's game rows more above and
                                         * below the view (high's rows -ey * ky up to (h + ey) * ky) */
    int wide_e;                         /* the screen's extra columns on each side it was made for */
    uint8_t *win;                       /* the stretched screen (main view): w x h, 1 where the
                                         * view's window is, 2 at its edge where the art's own
                                         * shape decides (NULL: not stretched) */
    uint8_t *scr;                       /* w x h: the view on the screen as the game copied it with
                                         * this picture (the HUD over the 3D); NULL: not copied */
    short tip_x0, tip_y0, tip_x1, tip_y1;   /* a popup help box shown with it (x1 <= x0: none) */
    int cov_n;                          /* the goggles' screens drawn over the view with it (the */
    short cov[3][4];                    /* HUD's there, whatever the 3D's colours): x0 y0 x1 y1 */
    uint8_t *hudm;                      /* w x h: 1 where a HUD picture drawn over the view has
                                         * its pixels (the suit's figure in the goggles); NULL: none */
    short cur_x0, cur_y0, cur_x1, cur_y1;   /* the game's pointer in scr (x1 <= x0: none) */
    int goggles;                        /* made for the goggles' screen (else the cockpit's) */
    int ckpack;                         /* ... for a cockpit pack's picture (port_ck_on) */
    int map;                            /* a map (port_s3d_map), not 3D: no anti-aliasing */
    uint8_t *hud;                       /* ... the HUD's layer: (w + 2 ex / kx) x h, 0 = nothing */
    uint8_t *low;                       /* w x h: the 3D picture the game drew, before the HUD */
    uint8_t *high;                      /* (kx * w + 2 * ex) x (ky * h) */
    uint32_t *tc;                       /* "True colour": as high, 0xFE000000 | RGB where the pixel
                                         * has a full colour (before the colour settings), else 0;
                                         * NULL: none */
    const uint8_t *tc_rows;             /* ... per row: 0 none in the row (tc not set there) */
    uint32_t tc_pal[256];               /* ... the game's colours (RGB) they were worked out with:
                                         * the display follows a later change of the palette with
                                         * them (the fade to black at a mission's end, P101) */
    int lab_n;                          /* the target label's pieces in high ("Anti-aliasing" */
    port_s3d_lab lab[PORT_S3D_LABS];    /* leaves their text sharp) */
    uint8_t *lab_m;
    size_t lab_cap, lab_used;
    unsigned serial;
    void *gl;                           /* the GPU ground's capture of its terrain (gpuland.c) */
} port_s3d_frame;

/* game thread, the HUD's marks drawn sharp (mapcone.c): the main view's picture being made and
 * where point (x, y) of the current canvas is in it (the picture's pixels); 0 if there is none */
typedef struct {
    uint8_t *high;
    uint32_t *tc;                       /* as port_s3d_frame */
    const uint8_t *tc_rows;
    int w, h, kx, ky;
    double X, Y;
    port_s3d_frame *f;
} port_s3d_paint;
int port_s3d_paint_at(double x, double y, port_s3d_paint *p);
/* ... a box of it whose marked pixels "Anti-aliasing" leaves sharp: its mask (x1 - x0 across,
 * zeroed, 1 = sharp); NULL if there is no room */
uint8_t *port_s3d_paint_sharp(const port_s3d_paint *p, int x0, int y0, int x1, int y1);

/* the views drawn sharp: 0 the main view, 1..3 the side screens' views (hview3d.c) */
#define PORT_S3D_SLOTS 4

/* display (main thread): the newest finished picture of a view, NULL if there is none to show */
const port_s3d_frame *port_s3d_take(int slot);

/* game thread, called from the decompiled game (hudmain.c, tngfx.c) */
void port_s3d_view_drawn(int slot);     /* the view was drawn this frame (any kind) */
unsigned port_s3d_views(int slot);      /* ... how many times so far */
void port_s3d_view_plain(int slot);     /* its widget drew with no picture made for it */
/* after the game drew a 3D view (FfView, FfCamera); mirror: the game mirrors its rows after */
void port_s3d_render(int slot, void *view, void *cam, int mirror);
/* after the game drew a map (MapDisp, mapdisp_276EF8(map, b)) on the current canvas: the squad
 * map, the other side screens' maps (1 + view) and the full-screen map (0) drawn sharp too */
void port_s3d_map(int slot, void *map, long b);
/* the goggles' radar (mapdisp.c port_map_hidden): map (a MapDisp) drawn whole, land and marks, at
 * kx x ky its pixels: *land its land, *pic with its marks, *pw x *ph; 0 if not drawn */
int port_s3d_radar_map(void *map, long b, int kx, int ky, const uint8_t **land, const uint8_t **pic,
                       int *pw, int *ph);
void port_s3d_blit(const void *rect);  /* GfxBlit copied an LGRect of the back buffer to the screen */
void port_s3d_tip(const void *rect);   /* helmet.c: a popup help box (LGRect) drawn this frame */
void port_s3d_cover(int x0, int y0, int x1, int y1);  /* helmet.c: a goggles' screen drawn this frame */
void port_s3d_hudkeep(const void *bm, long x, long y);   /* hudcb.c: a HUD picture kept until drawn again (NULL: none) */
void port_s3d_hudbits(const void *bm, long x, long y);   /* helmet.c: a HUD picture's pixels, drawn at x, y */
void port_s3d_block(long c, long x, long y, int only0);   /* a pixel as a kx x ky block */
void port_s3d_text(int font, char *s, long x, long y);    /* the game's text, its pixels as blocks */
void port_s3d_drop(long c, const void *bm, long x16, long y16);   /* weather.c, while drawing sharp */

#endif
