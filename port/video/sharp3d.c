/* sharp3d.c -- "3D resolution": the main 3D view drawn a second time at 2x to 6x the original
 * pixels, for the display only.
 *
 * The game draws its frame exactly as before (the original 3D view, then the HUD on top of it),
 * so everything the game takes from its own picture stays the same: the object under the mouse
 * (the probe pixel), the AI's "seen" marks, the pitch limit.  Right after the game's 3D view,
 * port_s3d_render draws the same view again into a bigger canvas of its own:
 *   - the game's whole data object (and the camera list on the heap) is saved before and put
 *     back after (and the 3D point pool), so the second drawing changes nothing the game can see;
 *   - the tables that hold one entry per canvas column, the save canvas and the render stack are
 *     swapped for bigger ones while it draws (port_s3d_on, see g5_b.h / g5_c.h);
 *   - the camera is a copy of the game's with the bigger canvas (same zoom: the same view).
 * The 3D picture the game drew is kept too (low).  When the game copies the view to the screen
 * (GfxBlit, port_s3d_blit) the picture is handed to the display, which shows the sharp picture
 * where the screen still holds the game's 3D pixels and the screen itself wherever the HUD or the
 * mouse pointer drew over them (platform.c).
 */
#include <math.h>
#include <string.h>
#include "g7_a.h"
#include "g5_c.h"
#include "sup_c.h"
#include "g8_c.h"
#include "port.h"
#include "platform.h"
#include "sharp3d.h"
#include "gpuland.h"
#include "cockpitpack.h"
#include "arms.h"
#include "helmetfx.h"
#include "edms.h"

#undef ff_col_ybuf
#undef sky_cols_save
#undef vd_spans
#undef sky_start
#undef sky_shade
#undef sky_dv
#undef sky_du
#undef sky_u
#undef sky_v
#undef sky_refl_row

int port_sharp3d;
volatile int port_s3d_out_h;
int port_widescreen;
int port_terrain, port_far_smooth, port_show_fps, port_water_full, port_water_ripples;
int port_water_fx;                      /* "Water effects" (screen.c, rndsky.c) */
int port_obj_dist;                      /* "Object distance" (screen.c): 0 the game's */
int port_enemy_dist;                    /* "Enemy distance" (screen.c): 0 the game's, 1 twice */
int port_tree_size;                     /* "Tree size" (screen.c): 0 the game's, 1 Realistic */
volatile unsigned port_s3d_us;
int port_s3d_mul = 1, port_s3d_tr128;
uchar *port_s3d_haze_ltab;
static uchar haze_ltab[16 * 0x1800];    /* planet_light through each haze level's shade row */

int port_s3d_wanted(void)
{
    return port_sharp3d || port_widescreen || port_terrain || port_far_smooth || port_water_full || port_fog ||
           port_obj_dist || port_enemy_dist || port_fx_pack[0] || port_obj_pack[0] || port_tree_size || port_ir_style ||
           port_truecol || port_burn || port_arms_pack[0];
}

/* the shell's back picture, which it draws and then copies to the screen (menuwide.c: the main
 * menu's pixels without the game's pointer, which is drawn on the screen); NULL unless 640 x 480 */
const uchar *port_back_picture(int *row)
{
    const grs_bitmap *b = &tnui_back_canvas.bm;
    if (!b->bits || b->w != 640 || b->h != 480 || b->row < 640)
        return NULL;
    *row = b->row;
    return b->bits;
}

/* "Tree size: Realistic", drawobj_29ACA8 while the sharp view draws: the width in the world
 * (scale) of a tree, bush or rock (a frame of 1160..1163) at (x, y) times 0.6 to 2, by its
 * place on the map, so a tree has the same size every time.  The picture keeps its proportions
 * and stands on the same point, and the screen box the view hides, clips and reflects it by
 * (drawobj_29CCE0) grows with it.  The game itself (shots, collisions, the line of sight) and its
 * own drawing keep the original size. */
fix port_tree_scale(long ref, fix x, fix y, fix scale)
{
    ulong id = (ulong)ref >> 16, h;

    if (!port_s3d_on || !port_tree_size || id < 1160 || id > 1163)
        return scale;
    h = (ulong)x * 0x9E3779B1u ^ (ulong)y * 0x85EBCA77u;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return (fix)(((long long)scale * (0x999A + (long)(h % 0x16667))) >> 16);
}

/* "Terrain distance" for the sharp view's camera (a copy of the game's): the bands the ground is
 * drawn in (smooth polygons first, then columns every 1, 2, 4, 8 squares; the game's own are
 * 2 / 12 | 30 / 64 / 100 / 240 at its middle detail) */
static const ushort terrain_bands[3][6][3] = {     /* start, end, step */
    { { 0 } },
    { { 0, 2, 1 }, { 2, 32, 1 }, { 32, 48, 1 }, { 48, 96, 2 }, { 96, 140, 4 }, { 140, 240, 8 } },
    { { 0, 2, 1 }, { 2, 48, 1 }, { 48, 64, 1 }, { 64, 128, 2 }, { 128, 180, 4 }, { 180, 280, 8 } },
};
#define SMOOTH_MAX 48                   /* the ring patterns (tcache_tab) end at 49 */

/* a band's drawing mode: the byte before its record (ff_cam +0x5C + 12k; weather.c) */
#define BAND_MODE(c, k) (((uchar *)(c))[0x5C + 12 * (k)])

static void detail_camera(FfCamera *c)
{
    int k;
    if (port_terrain > 0 && port_terrain < 3) {
        /* the drawing modes go with the new bands: the side screens' cameras have one smooth
         * band (band0 1) where the main view's has two, and a column band's mode left on a smooth
         * band has no polygon routine (holes and stray polygons, P23).  The smooth bands keep the
         * camera's last smooth mode (and its first one for the nearest band when it has two), the
         * column bands its first column mode. */
        int b0 = c->band0 > 0 ? c->band0 : 1;
        uchar near = BAND_MODE(c, 0), smooth = BAND_MODE(c, b0 - 1), cols = BAND_MODE(c, b0);
        BAND_MODE(c, 0) = b0 > 1 ? near : smooth;
        for (k = 1; k < 6; k++)
            BAND_MODE(c, k) = k < 2 ? smooth : cols;
        for (k = 0; k < 6; k++) {
            c->band[k].d = terrain_bands[port_terrain][k][0];
            c->band[k].e = terrain_bands[port_terrain][k][1];
            c->band[k].step = terrain_bands[port_terrain][k][2];
        }
        c->band0 = 2;
        c->band_end = 6;
    }
}

/* after FfCameraSetup scaled the bands by the zoom: the smooth ring within the patterns */
static void detail_clamp(FfCamera *c)
{
    if (port_terrain && c->band[c->band0 - 1].b > SMOOTH_MAX) {
        c->band[c->band0 - 1].b = SMOOTH_MAX;
        c->band[c->band0].a = SMOOTH_MAX;
    }
}

static void haze_tables(void)
{
    int l, j;
    for (l = 0; l < 16; l++) {
        const uchar *row = planet_shade + (l << 8);
        uchar *d = haze_ltab + l * 0x1800;
        for (j = 0; j < 0x1800; j++)
            d[j] = row[planet_light[j]];
    }
}
volatile int port_s3d_wide_e;
volatile int port_s3d_stretch, port_wide_cursor;
static const grs_stencil *win_sten;     /* the main view's widget (port_s3d_window) */
static int win_x0, win_y0, win_x1, win_y1;

void port_s3d_window(const void *sten, int x0, int y0, int x1, int y1)
{
    win_sten = sten;
    win_x0 = x0;
    win_y0 = y0;
    win_x1 = x1;
    win_y1 = y1;
}

static const uint8_t *ovl_mask;
static int ovl_w, ovl_h, ovl_x, ovl_y;

void port_s3d_overlay(const uint8_t *mask, int w, int h, int x, int y)
{
    ovl_mask = w > 0 && h > 0 ? mask : NULL;
    ovl_w = w;
    ovl_h = h;
    ovl_x = x;
    ovl_y = y;
}

int port_cursor_shown(void)
{
    return tnmouse_hide_count > 0;
}

int port_game_paused(void)
{
    return game_paused != 0;
}

int port_hud_goggles(void)
{
    return hud_helmet != 0;
}

/* the benchmark (bench.c): the player put at x, y, facing heading.  The game's move sets the
 * legs where the entity was (it gets the new place from the physics at the next frame), so it is
 * called again a frame later: the legs a whole map apart from the body stepped to places off the
 * map and crashed (biped.c) */
void port_player_place(long x, long y, unsigned heading)
{
    game_entities[game_player_entity].heading = (ushort)heading;
    physics_2D47D8(game_player_entity, x, y);
}

/* "Arms pack" (arms.c): how far the player's suit moved on the ground since the last call, in
 * metres (a map unit 0x10000 is 6), forward along its heading and to its right, and the game's
 * time that took (s); 0 when it is not on the ground (the jump jets: more than 0x8000 above it,
 * standing and walking it is 0x2400 .. 0x4E00), was placed (more than 5 m) or the last call was
 * long ago (the cockpit's view between, a saved game loaded) */
int port_player_motion(double *fwd, double *side, double *secs)
{
    static long lx, ly, le;
    static ulong lt;
    GameEntity *e = &game_entities[game_player_entity];
    double dx, dy, a;
    int on = 1;
    *fwd = *side = *secs = 0;
    if (!game_player_entity)
        return 0;
    dx = (double)(e->x - lx) * 6 / 65536;
    dy = (double)(e->y - ly) * 6 / 65536;
    *secs = (double)(long)(game_time - lt) / 1000;
    if (le != game_player_entity || dx * dx + dy * dy > 25 || *secs < 0 || *secs > 0.25 ||
        e->z - map_2949C4(e->x, e->y) > 0x8000)
        on = 0;
    lx = e->x;
    ly = e->y;
    lt = game_time;
    le = game_player_entity;
    if (!on) {
        *secs = 0;
        return 0;
    }
    a = e->heading * (6.283185307179586 / 65536);   /* forward (cos, sin), right (-sin, cos) */
    *fwd = dx * cos(a) + dy * sin(a);
    *side = -dx * sin(a) + dy * cos(a);
    return 1;
}

void port_cursor_pos(int *x, int *y)
{
    *x = tnm_pos.x;
    *y = tnm_pos.y;
}
volatile int port_wide_ptr, port_wide_ptr_x, port_wide_ptr_y;
volatile int port_wide_map, port_wide_map_x, port_wide_map_y;
int port_s3d_targ_rec;

/* the units the wide view drew: written into one list while the other is read */
static port_wide_targ wt_list[2][PORT_WIDE_TARGS];
static volatile int wt_count[2], wt_pub;
static int wt_n, wt_e, wt_kx, wt_ky, wt_ey;   /* (wt_ey: the rows drawn above the view) */

void port_wide_target(const short *r, const short *pt, int ent, int col, int cur)
{
    port_wide_targ *t;
    if (!port_s3d_targ_rec || wt_n >= PORT_WIDE_TARGS || r[2] < r[0] || r[3] < r[1])
        return;
    t = &wt_list[wt_pub ^ 1][wt_n++];
    t->x0 = (short)(r[0] / wt_kx - wt_e);
    t->x1 = (short)(r[2] / wt_kx - wt_e);
    t->y0 = (short)(r[1] / wt_ky - wt_ey);
    t->y1 = (short)(r[3] / wt_ky - wt_ey);
    /* (a unit drawn as a picture has no point of its own: pt is still its box, and the game
     * takes the box's middle, hud_bracket) */
    t->px = (short)((pt[0] + pt[2]) / 2 / wt_kx - wt_e);
    t->py = (short)((pt[1] + pt[3]) / 2 / wt_ky - wt_ey);
    t->ent = (short)ent;
    t->col = (uint8_t)col;
    t->cur = (uint8_t)cur;
}

int port_wide_targets(port_wide_targ *out)
{
    int p = wt_pub, n = wt_count[p];
    memcpy(out, wt_list[p], n * sizeof *out);
    return n;
}

int port_cursor_get(const uint8_t **bits, int *w, int *h, int *row, int *type, int *hx, int *hy,
                    int *color)
{
    TnCursor c = tnm_cursor;
    if (!c.bm.bits || c.bm.w <= 0 || c.bm.h <= 0)
        return 0;
    *bits = c.bm.bits;
    *w = c.bm.w;
    *h = c.bm.h;
    *row = c.bm.row;
    *type = c.bm.type;
    *hx = c.hot.x;
    *hy = c.hot.y;
    *color = tnm_cursor_color;
    return 1;
}

/* the tables the decompiled renderer uses (g5_b.h / g5_c.h map the names to these) */
int port_s3d_on, port_s3d_kx = 1, port_s3d_ky = 1, port_s3d_low_h = 200, port_s3d_low_w = 320,
    port_s3d_cols = 0x140, port_s3d_ex, port_s3d_fov_x, port_s3d_ts_turn;
long *port_ff_col_ybuf = ff_col_ybuf;
long *port_sky_cols_save = sky_cols_save;
ushort *port_vd_spans = vd_spans;
long port_vd_spans_max = 0x800;
long *port_sky_start = sky_start, *port_sky_shade = sky_shade, *port_sky_dv = sky_dv,
     *port_sky_du = sky_du;
ulong *port_sky_u = sky_u, *port_sky_v = sky_v;
uchar **port_sky_refl_row = sky_refl_row;

#define MAX_K 6
#define MAX_COLS 4096                   /* widescreen up to 682 of the game's columns at 6x */
#define STACK_SIZE (24 << 20)
#define SPANS_BIG 0x7F00                /* span indices are kept as negative shorts */
#define MAX_ROWS 2048

static long big_col_ybuf[MAX_COLS], big_sky_cols[MAX_COLS];
static ushort big_spans[SPANS_BIG];
static long big_sky_start[MAX_ROWS], big_sky_shade[MAX_ROWS], big_sky_dv[MAX_ROWS],
            big_sky_du[MAX_ROWS];
static ulong big_sky_u[MAX_ROWS], big_sky_v[MAX_ROWS];
static uchar *big_sky_refl_row[MAX_ROWS];
/* static, not from the heap: the game's own heap blocks keep their places (frame comparisons) */
static uchar big_stack[STACK_SIZE], big_vd[MAX_COLS * MAX_ROWS];
/* the water reflection reads rows above the view (the HUD's, in the game's back buffer): room
 * around each sharp picture for that */
#define MARGIN 0x10000
static uchar frame_low[PORT_S3D_SLOTS][3][320 * 480];
static uchar frame_scr[PORT_S3D_SLOTS][3][320 * 480];  /* the view on the screen, at the copy */
static uchar frame_win[3][320 * 480];   /* the stretched screen: where the view's window is */
static uchar frame_hudm[3][320 * 480];  /* the main view's HUD pictures' pixels, at the copy */
#define EDGE 3
#define HUD_COLS 512
static uchar frame_hud[3][HUD_COLS * 480];      /* ... the HUD's layer (port_s3d_hud_begin) */
static uchar frame_high[3][MARGIN + MAX_COLS * MAX_ROWS + MARGIN];
/* the side screens' views are small: up to SIDE_MAX of the game's pixels */
#define SIDE_MAX (160 * 120)
static uchar side_high[PORT_S3D_SLOTS - 1][3][MARGIN + SIDE_MAX * MAX_K * MAX_K + MARGIN];
static uchar data_save[TN_DATA_SIZE];
#define TEMP_SIZE (4 << 20)
static uchar big_temp_buf[TEMP_SIZE];   /* temp_malloc's buffer (48 KB in the game) */
static MemStack big_temp;
static uchar hvlist_save[0x30];
static uchar points_save[1000 * 32];    /* g3_init(1000): the 3D point pool on the heap */
static uchar blast_save[0x4000];        /* the 4 textures with blast marks (blasttm.c), on the heap */
extern int port_blast_live;

/* per view (slot 0 the main view, 1..3 the side screens): three pictures, one being made (game
 * thread), one finished, one on the screen (display) */
typedef struct {
    port_s3d_frame frames[3];
    port_s3d_frame *making, *done, *shown;
    unsigned views, views_at_blit;
    int idle_blits;
    int plain;                          /* the view drew with no picture made (port_s3d_view_plain) */
    int x, y, w, h;                     /* where the view was last (0 x 0: not yet) */
} Slot;
static Slot slots[PORT_S3D_SLOTS];
static volatile int lock;
static unsigned serial;

static Slot *slot_of(int i)
{
    Slot *sl = &slots[i];
    if (!sl->making) {
        sl->making = &sl->frames[0];
        sl->done = &sl->frames[1];
        sl->shown = &sl->frames[2];
    }
    return sl;
}

static void spin_lock(void)
{
    while (__sync_lock_test_and_set(&lock, 1))
        ;
}

static void spin_unlock(void)
{
    __sync_lock_release(&lock);
}

/* how many times the pixels, across (kx) and down (ky), at setting k for a scr_h screen */
static void choose_k_at(int k, int scr_h, int *kx, int *ky)
{
    if (k > MAX_K)
        k = MAX_K;
    if (k < 1)                          /* Original (widescreen only): the game's pixels */
        k = 1;
    *kx = k;
    *ky = scr_h > 200 ? (k + 1) / 2 : k;
}

static void choose_k(int scr_h, int *kx, int *ky)
{
    choose_k_at(port_sharp3d, scr_h, kx, ky);
}

void port_s3d_size(int setting, int scr_h, int *w, int *h)
{
    int kx, ky;
    choose_k_at(setting, scr_h, &kx, &ky);
    *w = 320 * kx;
    *h = scr_h * ky;
}

extern int port_game_options_up;        /* port/ui/options.c: the game's options screen (O) is up */
int port_help_screen_up(void);          /* help.c: the game's help screen (H) is up */

/* the stretched screen: the mission's screen is up (not the game's options or help screen over
 * it: P87) */
int port_s3d_stretch_up(void)
{
    return port_s3d_stretch && !port_game_options_up && !port_help_screen_up();
}

void port_s3d_view_drawn(int slot)
{
    Slot *sl = slot_of(slot);
    sl->views++;
    sl->making->valid = 0;
}

unsigned port_s3d_views(int slot)
{
    return slot_of(slot)->views;
}

/* a side screen's widget drew something with no picture made for it (the order menu, a list,
 * static: helmet.c helm_widget_draw): the next copy of it goes to the display as the game drew
 * it, not after three copies (with the order menu drawn once, the map stayed over it, P94) */
void port_s3d_view_plain(int slot)
{
    Slot *sl = slot_of(slot);
    sl->plain = 1;
    sl->making->valid = 0;
}

/* the resources: the sharp view draws things the game's view does not (more terrain detail,
 * widescreen), so it can load a resource the game has not loaded (the game's heap would change)
 * and each RefGet moves its resource to the LRU chain's tail (the order the game later drops
 * resources in would change).  So while it draws, memory comes from its own arena: a resource it
 * loads stays there, out of the game's table (and is put back in for the next sharp view while
 * the game has not loaded it itself); the LRU chain is put back afterwards. */
#define ARENA_SIZE (32 << 20)
static uchar arena[ARENA_SIZE];
static size_t arena_used;
static void *res_cache[0x10000];        /* by id: a resource loaded into the arena */
static struct { void *ptr; Id next, prev; uchar lock; } lru_save[0x10000];
static long lru_n;
static void *(WATCOMCALL *game_malloc)(size_t);
static void *(WATCOMCALL *game_realloc)(void *, size_t);
static void (WATCOMCALL *game_free)(void *);

static int in_arena(const void *p)
{
    return (const uchar *)p >= arena && (const uchar *)p < arena + ARENA_SIZE;
}

static void *WATCOMCALL arena_malloc(size_t n)
{
    size_t need = (n + 15) & ~(size_t)7;
    uchar *p;
    if (arena_used + need > ARENA_SIZE)
        return game_malloc(n);          /* freed again in lru_back */
    p = arena + arena_used;
    arena_used += need;
    *(size_t *)p = n;
    return p + 8;
}

static void WATCOMCALL arena_free(void *p)
{
    if (p && !in_arena(p))
        game_free(p);
}

static void *WATCOMCALL arena_realloc(void *p, size_t n)
{
    void *q;
    size_t old;
    if (p && !in_arena(p))
        return game_realloc(p, n);
    q = arena_malloc(n);
    if (q && p) {
        old = *(size_t *)((uchar *)p - 8);
        memcpy(q, p, old < n ? old : n);
    }
    return q;
}

/* before the sharp view: the chain kept, the arena's resources put back in */
static void lru_keep(void)
{
    long i;
    game_malloc = f_malloc;
    game_realloc = f_realloc;
    game_free = f_free;
    lru_n = gResDesc ? resDescMax + 1 : 0;
    if (arena_used > ARENA_SIZE / 2) {  /* full: start again */
        memset(res_cache, 0, sizeof res_cache);
        arena_used = 0;
    }
    for (i = 0; i < lru_n; i++) {
        lru_save[i].ptr = gResDesc[i].ptr;
        lru_save[i].next = gResDesc[i].next;
        lru_save[i].prev = gResDesc[i].prev;
        lru_save[i].lock = gResDesc[i].lock;
        if (res_cache[i]) {
            if (!gResDesc[i].ptr)
                gResDesc[i].ptr = res_cache[i];
            else
                res_cache[i] = NULL;    /* the game has its own now */
        }
    }
}

/* in each pass's scratch (enter_scratch): the arena */
static void arena_on(void)
{
    f_malloc = arena_malloc;
    f_realloc = arena_realloc;
    f_free = arena_free;
}

/* after it (the allocators come back with the data object) */
static void lru_back(void)
{
    static int told;
    long i = 0;
    if (!gResDesc || resDescMax + 1 != lru_n)
        goto changed;
    for (i = lru_n - 1; i >= 0; i--) {
        void *p = gResDesc[i].ptr;
        if (p == lru_save[i].ptr || lru_save[i].ptr)
            continue;
        if (in_arena(p))
            res_cache[i] = p;
        else
            game_free(p);               /* the arena was full */
        gResDesc[i].ptr = NULL;
        gResDesc[i].lock = lru_save[i].lock;
    }
    for (i = 0; i < lru_n; i++)
        if (gResDesc[i].ptr != lru_save[i].ptr)
            goto changed;
    for (i = 0; i < lru_n; i++) {
        gResDesc[i].next = lru_save[i].next;
        gResDesc[i].prev = lru_save[i].prev;
    }
    return;
changed:
    if (!told++)
        port_log("sharp 3D: resource %ld was loaded or dropped while drawing\n", i);
}

/* the render stack, temp_malloc's buffer and the save canvas swapped for the sharp view's own
 * (the game's come back with the data object) */
static void enter_scratch(void)
{
    ffmem_stack.baseptr = big_stack;
    ffmem_stack.sz = STACK_SIZE;
    MemStackInit(&ffmem_stack);
    ffmem_stack_on = 1;                 /* its scratch blocks from there, not the game's heap */
    big_temp.baseptr = big_temp_buf;
    big_temp.sz = TEMP_SIZE;
    MemStackInit(&big_temp);
    temp_mem_stack = &big_temp;
    vd_canvas.bm.bits = big_vd;
    arena_on();                         /* its resources from its own memory (lru_keep) */
}

/* everything the game can see back as it was before the sharp view (np: the point pool's bytes) */
static void leave_scratch(long np)
{
    memcpy(tn_data_obj, data_save, TN_DATA_SIZE);
    memcpy(game_438C20, hvlist_save, sizeof hvlist_save);
    if (np)
        memcpy(point_list, points_save, np);
    if (port_blast_live)                /* P71: else blast_map named copies the pass had refilled */
        memcpy(blast_cache[1].buf, blast_save, sizeof blast_save);
}

/* FREEFALL_S3D_TIME=1: the main view's drawing time in the log, every 100 frames */
static void time_log(uint64_t us)
{
    static int on = -1, n;
    static uint64_t sum, max;
    if (on < 0)
        on = getenv("FREEFALL_S3D_TIME") != NULL;
    if (!on)
        return;
    sum += us;
    max = us > max ? us : max;
    if (++n == 100) {
        port_log("sharp 3D: %u us a frame on average, %u at most\n", (unsigned)(sum / n), (unsigned)max);
        n = 0;
        sum = max = 0;
    }
}

/* the window's shape for the stretched screen (frame_win): the main view widget's stencil minus
 * the art over it, then the EDGE pixels beside it.  It changes only with the screen, so it is
 * made again only when its inputs change (it took about a millisecond a frame in the goggles) */
static uint8_t win_made[320 * 480];
static struct {
    int x, y, w, h, x0, y0, x1, y1, ow, oh, ox, oy;
    const void *sten, *ovl;
    uint32_t sum;
} win_key;

static uint32_t win_sum(int h)
{
    uint32_t sum = 0;
    int i, ry;
    if (win_sten)
        for (ry = 0; ry < h; ry++) {
            const grs_sten_elem *el = (const grs_sten_elem *)((ulong)win_sten->elem + ry * 8);
            for (; el; el = el->n)
                sum = sum * 31 + (uint32_t)(el->l * 65536 + el->r);
        }
    if (ovl_mask)
        for (i = 0; i < ovl_w * ovl_h; i++)
            sum = sum * 3 + ovl_mask[i];
    return sum;
}

/* widescreen: the main view's more columns on each side (x its column on the screen, w its
 * width): its share of the stretched screen's extra ones; with a cockpit pack's picture
 * (port_ck_on: the screen 1:1 in the middle) as far as the picture's sides */
static int view_extra(int x, int w, int wide_e, int ckp)
{
    int sw = tnui_back_canvas.bm.w, l = x, r = sw - x - w;
    if (ckp)
        return wide_e + (l > r ? l : r);
    return (w * wide_e + sw / 2) / sw;
}

static void win_shape(port_s3d_frame *f, int w, int h)
{
    int x, y;
    if (win_key.x == f->x && win_key.y == f->y && win_key.w == w && win_key.h == h &&
        win_key.x0 == win_x0 && win_key.y0 == win_y0 && win_key.x1 == win_x1 && win_key.y1 == win_y1 &&
        win_key.ow == ovl_w && win_key.oh == ovl_h && win_key.ox == ovl_x && win_key.oy == ovl_y &&
        win_key.sten == win_sten && win_key.ovl == ovl_mask && win_key.sum == win_sum(win_y1 - win_y0)) {
        memcpy(f->win, win_made, (size_t)w * h);
        return;
    }
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            int rx = f->x + x - win_x0, ry = f->y + y - win_y0, in = 0;
            if (rx >= 0 && ry >= 0 && rx < win_x1 - win_x0 && ry < win_y1 - win_y0) {
                in = 1;
                if (win_sten) {
                    const grs_sten_elem *el = (const grs_sten_elem *)((ulong)win_sten->elem + ry * 8);
                    in = 0;
                    for (; el && !in; el = el->n)
                        in = rx >= el->l && rx <= el->r;
                }
                if (in && ovl_mask) {   /* the art over the view */
                    int ox = f->x + x - ovl_x, oy = f->y + y - ovl_y;
                    if (ox >= 0 && oy >= 0 && ox < ovl_w && oy < ovl_h && ovl_mask[oy * ovl_w + ox])
                        in = 0;
                }
            }
            f->win[(size_t)y * w + x] = (uint8_t)in;
        }
    /* the stencil's rows are coarser than the art in 320x400: up to EDGE pixels beside the
     * window that no art covers are the window's too, where the screen still shows the 3D
     * there (2: the display looks) */
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            int ox = f->x + x - ovl_x, oy = f->y + y - ovl_y, i, j, near = 0;
            if (f->win[(size_t)y * w + x] ||
                (ovl_mask && ox >= 0 && oy >= 0 && ox < ovl_w && oy < ovl_h && ovl_mask[oy * ovl_w + ox]))
                continue;
            for (j = y - EDGE; j <= y + EDGE && !near; j++)
                for (i = x - EDGE; i <= x + EDGE && !near; i++)
                    near = i >= 0 && j >= 0 && i < w && j < h && f->win[(size_t)j * w + i] == 1;
            if (near)
                f->win[(size_t)y * w + x] = 2;
        }
    memcpy(win_made, f->win, (size_t)w * h);
    win_key.x = f->x; win_key.y = f->y; win_key.w = w; win_key.h = h;
    win_key.x0 = win_x0; win_key.y0 = win_y0; win_key.x1 = win_x1; win_key.y1 = win_y1;
    win_key.ow = ovl_w; win_key.oh = ovl_h; win_key.ox = ovl_x; win_key.oy = ovl_y;
    win_key.sten = win_sten; win_key.ovl = ovl_mask;
    win_key.sum = win_sum(win_y1 - win_y0);
}

/* the first frames after a switch of cockpit and goggles: the screen around the view and the art
 * over it (port_s3d_overlay) are still the other one's, so the picture laid in showed the old
 * window's edges; the game's own picture meanwhile */
static int settling(void)
{
    static int was_goggles = -1, settle;
    if (port_hud_goggles() != was_goggles) {
        was_goggles = port_hud_goggles();
        settle = 2;
    }
    if (settle) {
        settle--;
        return 1;
    }
    return 0;
}

/* everything the game can see is put back after a sharp pass; interrupts wait meanwhile (their
 * changes to the data object would be undone too) */
static int pass_begin(long *np)
{
    int was = port_irq_save();
    memcpy(data_save, tn_data_obj, TN_DATA_SIZE);
    memcpy(hvlist_save, game_438C20, sizeof hvlist_save);
    *np = n_points * (long)sizeof(g3s_point);
    if (point_list && *np > 0 && *np <= (long)sizeof points_save)
        memcpy(points_save, point_list, *np);
    else
        *np = 0;
    if (port_blast_live)
        memcpy(blast_save, blast_cache[1].buf, sizeof blast_save);
    lru_keep();
    enter_scratch();
    return was;
}

static void pass_end(int was, long np)
{
    leave_scratch(np);
    lru_back();
    if (!port_lockstep)                 /* the restore put the tick back (timer.c) */
        port_tick_sync();
    port_irq_restore(was);
}

/* "Arms pack": the player's shots leave from the muzzle of the arm drawn (owner), not
 * from the game's points beside the body (pbasim_28A6F0, off the screen at both sides).  The
 * muzzle's place in the sharp view's pixels with the view and the sharp camera it was drawn with
 * (the game's camera would be off sideways in widescreen: P77); port_arms_shot puts the shot's
 * start on the line from the eye through it, ARMS_SHOT_DIST along (the game's points: 0x1000
 * ahead, 0x2000 aside; nearer, a beam's band starts as thick as the arm).  It goes by the arm on
 * the screen, so it is the display's: lockstep play would have to send it along */
#define ARMS_SHOT_DIST 0x6000
static FfView shot_view;
static FfCamera shot_cam;
static float shot_x, shot_y;
static int shot_mount;
static ulong shot_time;

static void arms_shot_from(const FfView *v, const FfCamera *cam, int W, int H)
{
    float fx, fy;
    int m = port_arms_muzzle(&fx, &fy);
    if (!m || W <= 0 || H <= 0)
        return;
    shot_view = *v;
    shot_cam = *cam;
    shot_x = fx / W * shot_cam.half_w * 2;
    shot_y = fy / H * shot_cam.half_h * 2;
    shot_mount = m;
    shot_time = game_time;
}

/* the shot of the weapon in this mount leaves from the arm drawn */
static int shot_live(long kind)
{
    return shot_mount && kind == shot_mount && (ulong)(game_time - shot_time) <= 250 && port_arms_pack[0] &&
           port_hud_goggles() && shot_cam.half_w > 0 && shot_cam.half_h > 0;
}

int port_arms_shot(long kind, fix *x, fix *y, fix *z)
{
    fix px, py, pz, k;
    long sx, sy;
    if (!shot_live(kind))
        return 0;
    sx = lroundf(shot_x);
    sy = lroundf(shot_y);
    viewdist_2A28D0((ulong)(ushort)(short)sx | (ulong)(ushort)(short)sy << 16, &px, &py, &pz, (uchar *)&shot_cam,
                    &shot_view);
    k = fix_div(ARMS_SHOT_DIST, 0xA0000);       /* (a point 10 along the line) */
    *x = shot_view.x + fix_mul(px - shot_view.x, k);
    *y = shot_view.y + fix_mul(py - shot_view.y, k);
    *z = shot_view.z + fix_mul(pz - shot_view.z, k);
    return 1;
}

/* P127: from the arm the shot starts 2.25 m along the line through the arm's muzzle, about 1.8 m
 * beside the pointer's line (the game's own points: 0.8 m), and the game aims a free shot (no
 * target locked) at the point 10 units (60 m) along the pointer's line, without a lead: the two
 * lines cross only there.  A soldier is hit within 0.94 m of the middle of its ball, so a beam
 * passed beside an enemy standing still under the pointer nearer than about 28 m or farther than
 * about 90 m (with the game's points: farther than 130 m).  A free shot of a straight weapon from
 * the arm aims at what is under the pointer instead, within the weapon's reach: the nearest body
 * the pointer's line passes through (at the point of the line nearest the body's middle), else
 * the land or object it meets (the projectile's own ray cast); nothing there: the game's point.
 * Still no lead, the game's rule for free shots.  The shot from the game's points is left as it
 * was.  pview is the view the pointer's line came from (reticle_target). */
void port_arms_free_aim(long kind, long type, const void *pview, fix *x, fix *y, fix *z)
{
    const FfView *v = pview;
    const OrdType *o;
    double ex, ey, ez, dx, dy, dz, len, best, aim = -1;
    Q px, py, pz, vec[3], range, hit[3];
    terrain_ff tff;
    long me = -1, i;
    if (!shot_live(kind) || type <= 0 || type >= 0x30)
        return;
    o = &ord_table[type];
    if (!o->ray || !o->range)
        return;                         /* lobbed: the game's aim */
    ex = v->x / 65536.0 * 6;
    ey = v->y / 65536.0 * 6;
    ez = v->z / 65536.0 * 6;
    dx = *x / 65536.0 * 6 - ex;
    dy = *y / 65536.0 * 6 - ey;
    dz = *z / 65536.0 * 6 - ez;
    len = sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 0.01)
        return;
    dx /= len;
    dy /= len;
    dz /= len;
    best = o->range * 6.0;
    px = v->x * 6;
    py = v->y * 6;
    pz = v->z * 6;
    vec[0] = (Q)lround(dx * 65536);
    vec[1] = (Q)lround(dy * 65536);
    vec[2] = (Q)lround(dz * 65536);
    range = (Q)lround(best * 65536);
    memset(&tff, 0, sizeof tff);
    tff.my_size = 0x1000;               /* the projectile's (weapphys_2D9540) */
    if (ff_raycast(&px, &py, &pz, vec, &range, hit, &tff)) {
        double hx = hit[0] / 65536.0 - ex, hy = hit[1] / 65536.0 - ey, hz = hit[2] / 65536.0 - ez;
        double t = hx * dx + hy * dy + hz * dz;
        if (t > 0 && t < best)
            best = aim = t;
    }
    if (ENT(game_player_entity)->body)
        me = edms_ph2on[ENT(game_player_entity)->body];
    for (i = 0; i < EDMS_MAX_OBJ && edms_S[i][0][0] > edms_END; i++) {
        double cx, cy, cz, t, p2, r, in;
        if (i == me)
            continue;
        cx = edms_S[i][0][0] / 65536.0 - ex;
        cy = edms_S[i][1][0] / 65536.0 - ey;
        cz = (edms_S[i][2][0] + edms_I[i][EDMS_IDOF_PARAM39]) / 65536.0 - ez;
        t = cx * dx + cy * dy + cz * dz;
        if (t <= 0)
            continue;
        p2 = cx * cx + cy * cy + cz * cz - t * t;
        r = (edms_I[i][EDMS_IDOF_RADIUS] + 0x1000) / 65536.0;
        if (p2 >= r * r)
            continue;
        in = t - sqrt(r * r - p2);      /* where the line goes into the ball */
        if (in < best) {
            best = in;
            aim = t;
        }
    }
    if (aim <= 0)
        return;
    *x = (fix)lround((ex + dx * aim) / 6 * 65536);
    *y = (fix)lround((ey + dy * aim) / 6 * 65536);
    *z = (fix)lround((ez + dz * aim) / 6 * 65536);
}

void port_s3d_render(int slot, void *pview, void *pcam, int mirror)
{
    Slot *sl = slot_of(slot);
    FfCamera *cam = pcam, hc;
    FfView v = *(FfView *)pview;
    grs_canvas *lc = cam->canvas, big;
    port_s3d_frame *f = sl->making;
    long aspect;
    int was, w = lc->bm.w, h = lc->bm.h, kx, ky, y, e = 0, W, wide_e = port_s3d_wide_e, stretch = 0;
    int ckp = port_ck_on;                       /* a cockpit pack's picture (the display's) */
    int ey = 0;
    long off, np;
    uint64_t t0;

    f->valid = 0;
    if (!slot)
        port_map_cone_wide = 0x10000;
    if (!port_s3d_wanted() || (cam->flags & 3) || cam->display || w <= 0 || h <= 0)
        return;
    port_fx_update();                   /* a changed effects or objects pack, before the drawing pass */
    if (!slot && settling())
        return;
    off = lc->bm.bits - tnui_back_canvas.bm.bits;
    if (off < 0 || off >= (long)tnui_back_canvas.bm.row * tnui_back_canvas.bm.h)
        return;
    choose_k(tnui_back_canvas.bm.h, &kx, &ky);
    if (!port_sharp3d)
        kx = ky = 1;
    /* widescreen: only the main view of the mission's screen, as many more columns as its share
     * of the stretched screen (all of the screen's extra ones while it spans it: the goggles) */
    if (port_widescreen && !slot && wide_e > 0 && port_s3d_stretch) {
        e = view_extra((int)(off % tnui_back_canvas.bm.row), w, wide_e, ckp);
        stretch = e > 0;
    }
    if (e > (MAX_COLS / kx - w) / 2)
        e = (MAX_COLS / kx - w) / 2;
    if (e < 0)
        e = 0;
    if (!slot && e)                     /* the map's view cone widens with the view */
        port_map_cone_wide = (fix)(((long long)(w + 2 * e) << 16) / w);
    if (kx < 2 && ky < 2 && !e && (slot || !(port_terrain || port_far_smooth || port_fog || port_obj_dist || port_enemy_dist ||
                                             port_fx_pack[0] || port_obj_pack[0] || port_tree_size || port_ir_on() ||
                                             port_truecol || port_arms_pack[0])))
        return;
    W = (w + 2 * e) * kx;
    if (W > MAX_COLS || h * ky > MAX_ROWS || w * h > (int)sizeof frame_low[0][0])
        return;
    if (slot && w * h > SIDE_MAX)
        return;
    /* the layered helmet (platform.c hlp_draw): its parts move off the view's window by a few
     * rows, so the view is drawn that many rows taller above and below (the same pixels per
     * unit, more of them).  Not above the screen; below it, yes: the goggles' view reaches the
     * screen's last row, and kept inside the screen it got no rows more at all, so the parts
     * moving up left a black line under the goggles' top (owner; the rows below the
     * screen are drawn and not shown) */
    if (!slot && !mirror && ckp && port_helmet_sway) {
        int y0 = (int)(off / tnui_back_canvas.bm.row), sh = tnui_back_canvas.bm.h;
        ey = PORT_S3D_EY * sh / 200;
        /* the goggles: their window reaches above the view's first row (the Heavy's beside the
         * brow panels, row 10 of 400, the view from 16) and the frame moves up to its border's
         * rows: the whole screen above the view, or a black line under the rim (owner) */
        if (port_hud_goggles())
            ey = y0;
        if (ey > y0)
            ey = y0;
        if (ey < 0 || (h + 2 * ey) * ky > MAX_ROWS || (long)W * (h + 2 * ey) * ky > (long)MAX_COLS * MAX_ROWS)
            ey = 0;
    }
    f->low = frame_low[slot][f - sl->frames];
    f->high = (slot ? side_high[slot - 1][f - sl->frames] : frame_high[f - sl->frames]) + MARGIN +
              (size_t)ey * ky * W;
    for (y = 0; y < h; y++)
        memcpy(f->low + (size_t)y * w, lc->bm.bits + (size_t)y * lc->bm.row, w);

    t0 = port_now_us();
    was = pass_begin(&np);
    port_ff_col_ybuf = big_col_ybuf;
    port_sky_cols_save = big_sky_cols;
    port_vd_spans = big_spans;
    port_vd_spans_max = SPANS_BIG;
    port_sky_start = big_sky_start;
    port_sky_shade = big_sky_shade;
    port_sky_dv = big_sky_dv;
    port_sky_du = big_sky_du;
    port_sky_u = big_sky_u;
    port_sky_v = big_sky_v;
    port_sky_refl_row = big_sky_refl_row;
    port_s3d_kx = kx;
    port_s3d_ky = ky;
    port_s3d_low_h = h;
    port_s3d_low_w = w;
    port_s3d_cols = (w + 2 * e > 0x140 ? w + 2 * e : 0x140) * kx;
    port_s3d_ex = e * kx;
    port_s3d_tr128 = port_terrain > 0;
    port_s3d_mul = port_s3d_tr128 ? 16 : 4;
    port_s3d_haze_ltab = NULL;
    if (port_terrain || port_far_smooth || port_fog) {
        haze_tables();
        port_s3d_haze_ltab = haze_ltab;
    }
    port_s3d_on = 1;
    port_fog_begin(cam, lc->gc.fill_type == 1 ? (const void *)lc->gc.fill_parm : NULL);
    if (!slot)                          /* "True colour" (truecol.c) */
        port_tc_begin((int)(f - sl->frames), f->high - (size_t)ey * ky * W, W, (h + 2 * ey) * ky);

    gr_init_canvas(&big, f->high - (size_t)ey * ky * W, BMT_FLAT8, (short)W, (short)((h + 2 * ey) * ky));
    big.gc.fill_type = lc->gc.fill_type;
    big.gc.fill_parm = lc->gc.fill_parm;
    hc = *cam;
    hc.canvas = &big;
    hc.hview_ok = 0;
    hc.rows = 0;
    hc.probe = 0;
    hc.probe_hit = 0;
    aspect = grd_cap->aspect;
    if (kx != ky)                       /* keep zoom_y: aspect * kx / ky */
        grd_cap->aspect = (fix)((long long)aspect * kx / ky);
    detail_camera(&hc);
    FfCameraSetup(&hc, w * kx, (h + 2 * ey) * ky, cam->zoom);    /* (zy as for h: more rows) */
    detail_clamp(&hc);
    if (e) {
        /* widescreen ("Hor+"): the same pixels per unit as the game's view, more of them at the
         * sides; the height of view, the bands and the distances stay the game's.  fov_x too:
         * the objects' view distances scale with it (viewdist.c: the wider, the nearer they
         * vanish); the terrain's cone gets the wider angle on its own (terrstrp.c) */
        fix z = (fix)((long long)cam->zoom * w / (w + 2 * e));
        hc.half_w = W >> 1;
        hc.zoom = z;
        hc.zx = hc.half_w * z;
        port_s3d_fov_x = fix_atan2(0x10000, z);
    }
    {
        if (e && !slot) {               /* the units it draws, for the mouse beside the screen */
            wt_n = 0;
            wt_e = e;
            wt_kx = kx;
            wt_ky = ky;
            wt_ey = ey;
            port_s3d_targ_rec = 1;
        }
        if (!slot && !mirror)           /* the GPU ground: the terrain's polygons caught (gpuland.c) */
            port_gl_begin(&f->gl, W, (h + 2 * ey) * ky, hc.half_w, hc.half_h);
        ffpipe_29EDB4(&v, &hc);
        port_gl_end();
        if (port_s3d_targ_rec) {
            port_s3d_targ_rec = 0;
            wt_count[wt_pub ^ 1] = wt_n;
            wt_pub ^= 1;
        }
        /* the terrain builder walks three 45 degree sectors around the heading: where the wide
         * view reaches past them, terrstrp.c adds the squares of the sectors turned that way */
    }
    grd_cap->aspect = aspect;

    f->tc = slot ? NULL : (uint32_t *)port_tc_end(&f->tc_rows);  /* (with the drawing's sky and fog) */
    if (f->tc && ey) {                  /* (its rows from the view's first, as high's) */
        f->tc += (size_t)ey * ky * W;
        f->tc_rows += (size_t)ey * ky;
    }
    if (f->tc) {
        int c;
        for (c = 0; c < 256; c++)
            f->tc_pal[c] = (uint32_t)port_tc_pal[c];
    }
    port_fog_end();
    port_s3d_on = 0;
    port_s3d_tr128 = 0;
    port_s3d_mul = 1;
    port_s3d_haze_ltab = NULL;
    port_s3d_kx = 1;
    port_s3d_ky = 1;
    port_s3d_cols = 0x140;
    port_s3d_ex = 0;
    port_s3d_fov_x = 0;
    port_ff_col_ybuf = ff_col_ybuf;
    port_sky_cols_save = sky_cols_save;
    port_vd_spans = vd_spans;
    port_vd_spans_max = 0x800;
    port_sky_start = sky_start;
    port_sky_shade = sky_shade;
    port_sky_dv = sky_dv;
    port_sky_du = sky_du;
    port_sky_u = sky_u;
    port_sky_v = sky_v;
    port_sky_refl_row = sky_refl_row;
    pass_end(was, np);
    if (!slot && !mirror && port_arms_pack[0] && port_hud_goggles()) {
        int suit = port_ck_goggles_suit();      /* "Arms pack": the goggles' frame is up (arms.c) */
        if (suit > -2) {
            int cx, cy;
            if (port_wide_ptr) {
                cx = port_wide_ptr_x;
                cy = port_wide_ptr_y;
            } else
                port_cursor_pos(&cx, &cy);
            port_arms_draw(f->high, f->tc, f->tc_rows, W, h * ky, (h + ey) * ky,
                           (cx - ((double)(off % tnui_back_canvas.bm.row) + w / 2.0)) / ((w + 2 * e) / 2.0),
                           ((double)(off / tnui_back_canvas.bm.row) + h / 2.0 - cy) / (h / 2.0),
                           port_player_weapon_mount(), suit);
            arms_shot_from(&v, &hc, W, h * ky);
        }
    } else if (!slot && !mirror && port_arms_pack[0] && port_ck_view_suit() > -2)
        port_arms_preload(port_ck_view_suit());     /* the cockpit: the arm's poses ready (P78) */
    if (port_ir_on())                   /* Infrared: Sci-fi (infrared.c) */
        port_ir_sensor(f->high - (size_t)ey * ky * W, W, (h + 2 * ey) * ky, kx, ky);
    if (!slot) {
        port_s3d_us = (unsigned)(port_now_us() - t0);
        time_log(port_s3d_us);
    }

    f->scr_w = tnui_back_canvas.bm.w;
    f->scr_h = tnui_back_canvas.bm.h;
    f->x = off % tnui_back_canvas.bm.row;
    f->y = off / tnui_back_canvas.bm.row;
    f->w = w;
    f->h = h;
    f->kx = kx;
    f->ky = ky;
    f->ex = e * kx;
    f->ey = ey;
    f->wide_e = wide_e;
    f->win = NULL;
    f->hud = NULL;
    f->scr = NULL;
    f->tip_x1 = f->tip_x0 = 0;
    f->cov_n = 0;
    f->hudm = NULL;
    f->lab_n = 0;
    f->lab_used = 0;
    f->cur_x1 = f->cur_x0 = 0;
    f->goggles = port_hud_goggles();
    f->ckpack = ckp;
    f->map = 0;
    if (stretch && w + 2 * e <= HUD_COLS && h <= 480) {
        f->hud = frame_hud[f - sl->frames];
        memset(f->hud, 0, (size_t)(w + 2 * e) * h);
    }
    if (stretch) {                      /* the window's shape: the widget's stencil (row spans) */
        f->win = frame_win[f - sl->frames];
        win_shape(f, w, h);
    }
    if (mirror) {                       /* the rear view: rows mirrored, as hview3d's mirror_rows */
        int x;
        for (y = 0; y < h * ky; y++) {
            uchar *row = f->high + (size_t)y * W;
            uint32_t *tr = f->tc && f->tc_rows[y] ? f->tc + (size_t)y * W : NULL;
            for (x = 0; x < W / 2; x++) {
                uchar t = row[x];
                row[x] = row[W - 1 - x];
                row[W - 1 - x] = t;
                if (tr) {
                    uint32_t c = tr[x];
                    tr[x] = tr[W - 1 - x];
                    tr[W - 1 - x] = c;
                }
            }
        }
        /* the game's own picture as mirror_rows leaves it: pairs of columns from each end, so
         * with a width that is not a multiple of 4 the middle columns stay (the sharp picture
         * is mirrored whole; else they counted as the HUD's, a low-resolution seam) */
        for (y = 0; y < h; y++) {
            uchar *row = f->low + (size_t)y * w;
            for (x = 0; x < (w >> 2) * 2; x++) {
                uchar t = row[x];
                row[x] = row[w - 1 - x];
                row[w - 1 - x] = t;
            }
        }
    }
    sl->x = f->x;
    sl->y = f->y;
    sl->w = w;
    sl->h = h;
    f->valid = 1;
}

/* The maps (the squad map and the other maps of the side screens, the full-screen map): right
 * after the game drew one (mapdisp_276EF8 on the current canvas), the same map drawn again into
 * the view's picture at kx x ky its size: a copy of the map display with the scale divided by kx
 * (in 320x400 a picture pixel as tall as it is wide: the y shift taken out), its terrain drawn
 * for that scale (kept while the map and the game's own terrain bitmap stay the same), the marks
 * at the picture's scale (port/video/mapsharp.c).  Widescreen, the full-screen map: as many more of
 * its columns on each side as the main view would have (the map shows more of the land), laid
 * at the right proportions in the stretched window like the main view. */
extern int port_map_kx, port_map_ky, port_map_sh, port_map_low_h, port_map_pass;
static uchar map_terr0[MAX_COLS * MAX_ROWS];
static uchar map_terr_side[PORT_S3D_SLOTS - 1][SIDE_MAX * MAX_K * MAX_K];
static uchar map_seen[PORT_S3D_SLOTS][320 * 480];      /* the game's terrain bitmap it was for */
static struct {
    fix x, y, scale;
    int w, h, kx, ky, e, lit, mission, smooth, ok;
} map_key[PORT_S3D_SLOTS];

/* widescreen: how many more of a map's columns show on each side of the game's (w its width) */
static int map_extra(int slot, int w, int kx, int ckp)
{
    int e = 0, wide_e = port_s3d_wide_e;
    /* (with a cockpit pack the side screens are not stretched; the full map is centred) */
    if (port_widescreen && wide_e > 0 && port_s3d_stretch && (!slot || !port_hud_goggles()) &&
        !(slot && ckp))
        e = view_extra((tnui_back_canvas.bm.w - w) / 2, w, wide_e, ckp);
    if (e > (MAX_COLS / kx - w) / 2)
        e = (MAX_COLS / kx - w) / 2;
    return e < 0 ? 0 : e;
}

/* ... for the game's map display m (full: the full-screen map), so that it keeps those columns
 * on the land too: 0 while the map is not drawn wide (mapdisp.c) */
int port_map_extra(const void *map, int full)
{
    const MapDisp *m = map;
    int kx, ky, e;
    if (!port_s3d_wanted() || !m || m->bm.w <= 0)
        return 0;
    choose_k(tnui_back_canvas.bm.h, &kx, &ky);
    if (!port_sharp3d)
        kx = 1;
    e = map_extra(!full, m->bm.w, kx, port_ck_on);
    if (!full && (m->bm.w + 2 * e) * m->bm.h > SIDE_MAX)
        return 0;
    return e;
}

void port_s3d_map(int slot, void *map, long b)
{
    Slot *sl = slot_of(slot);
    MapDisp *m = map, hm;
    grs_canvas *lc = grd_canvas, big;
    port_s3d_frame *f;
    int w = lc->bm.w, h = lc->bm.h, kx, ky, e = 0, W, H, y, was, stretch = 0;
    int wide_e = port_s3d_wide_e, ckp = port_ck_on;
    long off, np, sh = hud_35A544;
    fix aspect;

    if (slot)                           /* (the main view's was counted before the map) */
        port_s3d_view_drawn(slot);
    f = sl->making;
    f->valid = 0;
    if (!port_s3d_wanted() || !m || w <= 0 || h <= 0 || lc->bm.type != BMT_FLAT8 ||
        m->bm.w != w || m->bm.h != h || m->bm.type != BMT_FLAT8 || m->scale <= 0)
        return;
    if (!slot && settling())
        return;
    off = lc->bm.bits - tnui_back_canvas.bm.bits;
    if (off < 0 || off >= (long)tnui_back_canvas.bm.row * tnui_back_canvas.bm.h)
        return;
    choose_k(tnui_back_canvas.bm.h, &kx, &ky);
    if (!port_sharp3d)
        kx = ky = 1;
    /* widescreen: the full map like the main view; the cockpit's side screens are stretched
     * with the screen, so their maps get their share of its extra columns too (the goggles'
     * side screens are drawn over the main view: not stretched) */
    e = map_extra(slot, w, kx, ckp);
    stretch = !slot && e > 0;
    if (kx < 2 && !e)
        return;
    if (kx > 1 && kx != ky << sh)       /* (the settings are even: always so) */
        return;
    W = (w + 2 * e) * kx;
    H = h * ky;
    if (W > MAX_COLS || H > MAX_ROWS || w * h > (int)sizeof frame_low[0][0])
        return;
    if (slot && (w + 2 * e) * h > SIDE_MAX)
        return;
    f->low = frame_low[slot][f - sl->frames];
    f->high = (slot ? side_high[slot - 1][f - sl->frames] : frame_high[f - sl->frames]) + MARGIN;
    f->tc = NULL;
    for (y = 0; y < h; y++)
        memcpy(f->low + (size_t)y * w, lc->bm.bits + (size_t)y * lc->bm.row, w);

    hm = *m;
    if (kx > 1) {
        hm.scale = m->scale / kx;
        hm.inv = m->inv * kx;
    }
    hm.x = m->x - e * m->scale;
    hm.bm.w = (short)W;
    hm.bm.h = (short)H;
    hm.bm.row = (ushort)W;
    hm.bm.bits = slot ? map_terr_side[slot - 1] : map_terr0;
    hm.canvas = &big;

    was = pass_begin(&np);
    if (kx > 1)
        hud_35A544 = 0;
    port_map_pass = 1;
    if (!map_key[slot].ok || map_key[slot].x != hm.x || map_key[slot].y != hm.y ||
        map_key[slot].scale != hm.scale || map_key[slot].w != W || map_key[slot].h != H ||
        map_key[slot].kx != kx || map_key[slot].ky != ky || map_key[slot].e != e ||
        map_key[slot].lit != (m->flags & 8) || map_key[slot].mission != game_359708 ||
        map_key[slot].smooth != port_map_smooth || memcmp(map_seen[slot], m->bm.bits, (size_t)w * h)) {
        mapdisp_2774C0(&hm, NULL);
        map_key[slot].x = hm.x;
        map_key[slot].y = hm.y;
        map_key[slot].scale = hm.scale;
        map_key[slot].w = W;
        map_key[slot].h = H;
        map_key[slot].kx = kx;
        map_key[slot].ky = ky;
        map_key[slot].e = e;
        map_key[slot].lit = m->flags & 8;
        map_key[slot].mission = game_359708;
        map_key[slot].smooth = port_map_smooth;
        map_key[slot].ok = 1;
        memcpy(map_seen[slot], m->bm.bits, (size_t)w * h);
    }
    gr_init_canvas(&big, f->high, BMT_FLAT8, (short)W, (short)H);
    gr_push_canvas(&big);
    aspect = grd_cap->aspect;
    if (kx != ky)                       /* the rings (gen_fix_ring) as tall as they show */
        grd_cap->aspect = (fix)((long long)aspect * kx / ky);
    port_map_kx = kx;
    port_map_ky = ky;
    port_map_sh = (int)sh;
    port_map_low_h = h;
    mapdisp_276EF8((uchar *)&hm, b);
    port_map_kx = port_map_ky = 1;
    port_map_pass = 0;
    grd_cap->aspect = aspect;
    gr_pop_canvas();
    pass_end(was, np);
    hud_35A544 = sh;

    f->scr_w = tnui_back_canvas.bm.w;
    f->scr_h = tnui_back_canvas.bm.h;
    f->x = off % tnui_back_canvas.bm.row;
    f->y = off / tnui_back_canvas.bm.row;
    f->w = w;
    f->h = h;
    f->kx = kx;
    f->ky = ky;
    f->ex = e * kx;
    f->ey = 0;
    f->wide_e = wide_e;
    f->win = NULL;
    f->hud = NULL;
    f->scr = NULL;
    f->tip_x1 = f->tip_x0 = 0;
    f->cov_n = 0;
    f->hudm = NULL;
    f->lab_n = 0;
    f->lab_used = 0;
    f->cur_x1 = f->cur_x0 = 0;
    f->goggles = port_hud_goggles();
    f->ckpack = ckp;
    f->map = 1;
    if (stretch) {
        f->win = frame_win[f - sl->frames];
        win_shape(f, w, h);
    }
    sl->x = f->x;
    sl->y = f->y;
    sl->w = w;
    sl->h = h;
    f->valid = 1;
}

/* the goggles' radar ("Radar without MFD", mapdisp.c port_map_hidden): the middle screen's map
 * (map: a copy of it, the player at its middle) drawn whole as mapdisp_276EF8 draws it, at kx x ky
 * its pixels (the main view's picture's scale; in 320x400 a pixel as tall as it is wide, the y
 * shift taken out): *land its land alone, *pic with its marks too, *pw x *ph; b the unit whose
 * sensors the map shows.  Its own short view cone is left out (the radar draws one out to its
 * rim).  Its top-left on whole pixels of it: the land is drawn again only when the map moved by
 * a pixel (the player at its middle pixel all the while); the game's state stays as it was
 * (pass_begin / pass_end, as for the sharp maps).  0: not drawn */
static uchar radar_land[SIDE_MAX * MAX_K * MAX_K], radar_pic[SIDE_MAX * MAX_K * MAX_K];
static struct {
    fix x, y, scale;
    int w, h, lit, mission, smooth, ok;
} radar_key;

int port_s3d_radar_map(void *map, long b, int kx, int ky, const uint8_t **land, const uint8_t **pic,
                       int *pw, int *ph)
{
    MapDisp *m = map, hm;
    grs_canvas big;
    long np, sh = hud_35A544;
    int W, H, was, cone = port_map_cone;
    fix aspect;

    if (!m || kx < 2 || ky < 1 || kx != ky << sh || m->bm.w <= 0 || m->bm.h <= 0 || m->scale <= 0)
        return 0;
    W = m->bm.w * kx;
    H = m->bm.h * ky;
    if (m->bm.w * m->bm.h > SIDE_MAX || W > MAX_COLS || H > MAX_ROWS)
        return 0;
    hm = *m;
    hm.scale = m->scale / kx;
    hm.inv = m->inv * kx;
    if (hm.scale <= 0)
        return 0;
    /* on whole pixels of the land: drawn again only once the map moved by one (the player, at
     * the middle pixel, stays on it) */
    hm.x -= hm.x % hm.scale;
    hm.y -= hm.y % hm.scale;
    hm.bm.type = BMT_FLAT8;
    hm.bm.flags = 0;
    hm.bm.align = 0;
    hm.bm.w = (short)W;
    hm.bm.h = (short)H;
    hm.bm.row = (ushort)W;
    hm.bm.bits = radar_land;
    hm.canvas = &big;

    was = pass_begin(&np);
    hud_35A544 = 0;
    port_map_pass = 1;
    if (!radar_key.ok || radar_key.x != hm.x || radar_key.y != hm.y || radar_key.scale != hm.scale ||
        radar_key.w != W || radar_key.h != H || radar_key.lit != (m->flags & 8) ||
        radar_key.mission != game_359708 || radar_key.smooth != port_map_smooth) {
        mapdisp_2774C0(&hm, NULL);
        radar_key.x = hm.x;
        radar_key.y = hm.y;
        radar_key.scale = hm.scale;
        radar_key.w = W;
        radar_key.h = H;
        radar_key.lit = m->flags & 8;
        radar_key.mission = game_359708;
        radar_key.smooth = port_map_smooth;
        radar_key.ok = 1;
    }
    gr_init_canvas(&big, radar_pic, BMT_FLAT8, (short)W, (short)H);
    gr_push_canvas(&big);
    aspect = grd_cap->aspect;
    if (kx != ky)                       /* the rings (gen_fix_ring) as tall as they show */
        grd_cap->aspect = (fix)((long long)aspect * kx / ky);
    port_map_kx = kx;
    port_map_ky = ky;
    port_map_sh = (int)sh;
    port_map_low_h = m->bm.h;
    port_map_cone = 0;
    mapdisp_276EF8((uchar *)&hm, b);
    port_map_cone = cone;
    port_map_kx = port_map_ky = 1;
    port_map_pass = 0;
    grd_cap->aspect = aspect;
    gr_pop_canvas();
    pass_end(was, np);
    hud_35A544 = sh;
    *land = radar_land;
    *pic = radar_pic;
    *pw = W;
    *ph = H;
    return 1;
}

/* a rain / snow drop in the sharp view (weather.c): (x16, y16) the drop's place on the game's
 * view in 1/16 pixels, bm its picture (NULL: one pixel of colour c); each pixel of the original
 * becomes a kx x ky block, placed with the drop's sub-pixel position.  Widescreen: the game's
 * drops are repeated at the sides, one view's width out */
static void drop_at(long c, const void *pbm, long x16, long y16)
{
    const grs_bitmap *bm = pbm;
    grs_canvas *cv = grd_canvas;
    int kx = port_s3d_kx, ky = port_s3d_ky;
    int bw = bm ? bm->w : 1, bh = bm ? bm->h : 1, i, j, x, y;
    int l = cv->gc.clip.i.left, t = cv->gc.clip.i.top, r = cv->gc.clip.i.right,
        b = cv->gc.clip.i.bot;
    long sx = ((x16 * kx) >> 4) + port_s3d_ex, sy = (y16 * ky) >> 4;
    for (j = 0; j < bh; j++)
        for (i = 0; i < bw; i++) {
            int v = bm ? bm->bits[j * bm->row + i] : (int)c;
            if (bm && v == 0)           /* transparent */
                continue;
            for (y = sy + j * ky; y < sy + (j + 1) * ky; y++) {
                if (y < t || y >= b)
                    continue;
                for (x = sx + i * kx; x < sx + (i + 1) * kx; x++)
                    if (x >= l && x < r)
                        cv->bm.bits[(long)y * cv->bm.row + x] = (uchar)v;
            }
        }
}

void port_s3d_drop(long c, const void *pbm, long x16, long y16)
{
    if (port_s3d_ex) {
        long d = (long)port_s3d_low_w << 4;
        drop_at(c, pbm, x16 - d, y16);
        drop_at(c, pbm, x16 + d, y16);
    }
    drop_at(c, pbm, x16, y16);
}

/* a pixel the original draws as one pixel of the view (particles, stars): a kx x ky block
 * around (x, y) of the sharp canvas, only on pixels that are 0 when only0 */
void port_s3d_block(long c, long x, long y, int only0)
{
    grs_canvas *cv = grd_canvas;
    int kx = port_s3d_kx, ky = port_s3d_ky, i, j;
    x -= kx / 2;
    y -= ky / 2;
    for (j = y; j < y + ky; j++) {
        if (j < cv->gc.clip.i.top || j >= cv->gc.clip.i.bot)
            continue;
        for (i = x; i < x + kx; i++) {
            uchar *p = cv->bm.bits + (long)j * cv->bm.row + i;
            if (i >= cv->gc.clip.i.left && i < cv->gc.clip.i.right && (!only0 || !*p))
                *p = (uchar)c;
        }
    }
}

/* the game's text in the sharp view (the nav point's distance, drawobj.c): drawn as the game
 * draws it on a canvas of its own, then each of its pixels a kx x ky block from (x, y) of the
 * sharp canvas, so it keeps the game's size */
#define NTW 160
#define NTH 32
static uchar ntbuf[NTW * NTH];

void port_s3d_text(int font, char *s, long x, long y)
{
    grs_canvas *cv = grd_canvas, tc;
    int kx = port_s3d_kx, ky = port_s3d_ky, i, j, X, Y;
    memset(ntbuf, 0, sizeof ntbuf);
    gr_init_canvas(&tc, ntbuf, BMT_FLAT8, NTW, NTH);
    tc.gc.fcolor = cv->gc.fcolor;
    gr_push_canvas(&tc);
    GfxDrawText(font, s, 0, 0);
    gr_pop_canvas();
    for (j = 0; j < NTH; j++)
        for (i = 0; i < NTW; i++) {
            uchar v = ntbuf[j * NTW + i];
            if (!v)
                continue;
            for (Y = y + j * ky; Y < y + (j + 1) * ky; Y++) {
                if (Y < cv->gc.clip.i.top || Y >= cv->gc.clip.i.bot)
                    continue;
                for (X = x + i * kx; X < x + (i + 1) * kx; X++)
                    if (X >= cv->gc.clip.i.left && X < cv->gc.clip.i.right)
                        cv->bm.bits[(long)Y * cv->bm.row + X] = v;
            }
        }
}

static LGRect tip;                      /* the popup help box drawn since the last copy */

void port_s3d_tip(const void *rect)
{
    tip = *(const LGRect *)rect;
}

/* the goggles' screens (views 0..2) drawn over the main view since the last copy: the display
 * shows the HUD's pixels there.  It told the HUD from the 3D by the colours (the screen's pixel
 * not the 3D's), so a picture's pixel of the 3D's colour by chance showed the sharp 3D through
 * the screen (the squad mate's face, the weapon's picture: owner) */
static short cov[3][4];
static int cov_n;

void port_s3d_cover(int x0, int y0, int x1, int y1)
{
    int i;
    if (x1 <= x0 || y1 <= y0)
        return;
    for (i = 0; i < cov_n; i++)
        if (cov[i][0] == x0 && cov[i][1] == y0 && cov[i][2] == x1 && cov[i][3] == y1)
            return;
    if (cov_n == 3)                     /* (not copied for a while: the latest only) */
        cov_n = 0;
    {
        cov[cov_n][0] = (short)x0;
        cov[cov_n][1] = (short)y0;
        cov[cov_n][2] = (short)x1;
        cov[cov_n][3] = (short)y1;
        cov_n++;
    }
}

/* a HUD picture drawn over the main view (helmet.c: the suit's figure in the goggles, bm drawn
 * at x, y of the current canvas): its pixels are the HUD's in the display whatever their colours.
 * Told by the colours alone, a pixel of the picture of the 3D's colour there by chance showed the
 * sharp 3D, and with the "Arms pack" the arm behind it, flickering as the land moved under it
 * (the dark road and the figure's dark colour: owner) */
static uchar hudm_scr[320 * 480];
static int hudm_y0 = 480, hudm_y1;      /* its rows marked since the last copy */

void port_s3d_hudbits(const void *pbm, long x, long y)
{
    const grs_bitmap *bm = pbm;
    grs_canvas *cv = grd_canvas;
    long off = cv->bm.bits - tnui_back_canvas.bm.bits, row = tnui_back_canvas.bm.row, ox, oy;
    int i, j;
    if (!port_s3d_wanted() || bm->type != BMT_FLAT8 || off < 0 || off >= row * tnui_back_canvas.bm.h)
        return;
    ox = off % row;
    oy = off / row;
    for (j = 0; j < bm->h; j++) {
        long cy = y + j, sy = oy + cy;
        if (cy < cv->gc.clip.i.top || cy >= cv->gc.clip.i.bot || sy < 0 || sy >= 480 ||
            sy >= tnui_back_canvas.bm.h)
            continue;
        for (i = 0; i < bm->w; i++) {
            long cx = x + i, sx = ox + cx;
            if (bm->bits[(long)j * bm->row + i] && cx >= cv->gc.clip.i.left && cx < cv->gc.clip.i.right &&
                sx >= 0 && sx < 320)
                hudm_scr[sy * 320 + sx] = 1;
        }
        if (sy < hudm_y0)
            hudm_y0 = (int)sy;
        if (sy + 1 > hudm_y1)
            hudm_y1 = (int)sy + 1;
    }
}

/* ... the weapon's box (hudcb.c): the widget's whole rectangle, where the HUD draws the weapon's
 * picture over its black, is the HUD's until it is drawn again (hudk_scr, its rectangle hudk_r),
 * added to each copy's.  In the cockpits and goggles whose box is over the view's rectangle, a
 * pixel of the picture, or of the black around it, that happened to match the game's art or the
 * 3D there showed the pack's art behind it: the box's bright green (P124, P125, owner) */
static uchar hudk_scr[320 * 480];
static int hudk_r[4];                   /* x0, y0, x1, y1 on the screen (x1 <= x0: none) */

void port_s3d_hudkeep(const void *pbm, long x, long y)
{
    grs_canvas *cv = grd_canvas;
    long off = cv->bm.bits - tnui_back_canvas.bm.bits, row = tnui_back_canvas.bm.row, ox, oy;
    int j;
    (void)pbm;
    (void)x;
    (void)y;
    for (j = hudk_r[1]; j < hudk_r[3]; j++)
        memset(hudk_scr + (size_t)j * 320 + hudk_r[0], 0, (size_t)(hudk_r[2] - hudk_r[0]));
    hudk_r[2] = hudk_r[0];
    if (off < 0 || off >= row * tnui_back_canvas.bm.h)
        return;
    ox = off % row;
    oy = off / row;
    hudk_r[0] = (int)ox;
    hudk_r[1] = (int)oy;
    hudk_r[2] = (int)(ox + cv->bm.w > 320 ? 320 : ox + cv->bm.w);
    hudk_r[3] = (int)(oy + cv->bm.h > 480 ? 480 : oy + cv->bm.h);
    if (hudk_r[2] <= hudk_r[0] || hudk_r[3] <= hudk_r[1]) {
        hudk_r[2] = hudk_r[0];
        return;
    }
    for (j = hudk_r[1]; j < hudk_r[3]; j++)
        memset(hudk_scr + (size_t)j * 320 + hudk_r[0], 1, (size_t)(hudk_r[2] - hudk_r[0]));
}

/* GfxBlit copied rectangle r of the back buffer to the screen: when it holds the whole view,
 * the picture made for this frame goes to the display (with the screen's view as copied) */
static void slot_blit(Slot *sl, const LGRect *r)
{
    port_s3d_frame *f = sl->making;
    if (f->valid) {
        if (r->ul.x > f->x || r->ul.y > f->y || r->lr.x < f->x + f->w || r->lr.y < f->y + f->h)
            return;
    } else if (sl->w > 0) {             /* nothing made: a copy of where the view was counts */
        if (r->ul.x > sl->x || r->ul.y > sl->y || r->lr.x < sl->x + sl->w || r->lr.y < sl->y + sl->h)
            return;
    } else if (r->ul.x > 0 || r->ul.y > 0 || r->lr.x < tnui_back_canvas.bm.w ||
               r->lr.y < tnui_back_canvas.bm.h)
        return;
    if (f->valid) {                     /* the screen as copied now, with this picture */
        int y;
        f->scr = frame_scr[sl - slots][f - sl->frames];
        for (y = 0; y < f->h; y++)
            memcpy(f->scr + (size_t)y * f->w,
                   tnui_back_canvas.bm.bits + (size_t)(f->y + y) * tnui_back_canvas.bm.row + f->x, f->w);
        if (sl == slots) {
            f->tip_x0 = tip.ul.x;
            f->tip_y0 = tip.ul.y;
            f->tip_x1 = tip.lr.x;
            f->tip_y1 = tip.lr.y;
            tip.lr.x = tip.ul.x;
            memcpy(f->cov, cov, sizeof cov);
            f->cov_n = cov_n;
            cov_n = 0;
            if (hudm_y1 > hudm_y0 ||    /* the HUD pictures' pixels on the view */
                (hudk_r[2] > hudk_r[0] && hudk_r[1] < f->y + f->h && hudk_r[3] > f->y)) {
                f->hudm = frame_hudm[f - sl->frames];
                for (y = 0; y < f->h; y++) {
                    int x;
                    uchar *d = f->hudm + (size_t)y * f->w;
                    memcpy(d, hudm_scr + (size_t)(f->y + y) * 320 + f->x, f->w);
                    if (f->y + y >= hudk_r[1] && f->y + y < hudk_r[3])
                        for (x = hudk_r[0]; x < hudk_r[2]; x++)
                            if (x >= f->x && x < f->x + f->w && hudk_scr[(size_t)(f->y + y) * 320 + x])
                                d[x - f->x] = 1;
                }
                if (hudm_y1 > hudm_y0)  /* (none marked, hudk_r alone: rows 480 to 0, P144) */
                    memset(hudm_scr + (size_t)hudm_y0 * 320, 0, (size_t)(hudm_y1 - hudm_y0) * 320);
                hudm_y0 = 480;
                hudm_y1 = 0;
            }
        }
        /* the pointer the game drew into the screen before copying it (the cockpit: helm_262814):
         * the display goes by the screen itself there, the pointer having moved since */
        f->cur_x1 = f->cur_x0 = 0;
        if (tnm_cursor_drawn && tnm_cursor.bm.w > 0) {
            f->cur_x0 = (short)(tnm_draw_pos.x - tnm_cursor.hot.x - 2);
            f->cur_y0 = (short)(tnm_draw_pos.y - tnm_cursor.hot.y - 2);
            f->cur_x1 = (short)(f->cur_x0 + tnm_cursor.bm.w + 4);
            f->cur_y1 = (short)(f->cur_y0 + tnm_cursor.bm.h + 4);
        }
    }
    if (sl->views == sl->views_at_blit && !f->valid && !sl->plain) {
        if (++sl->idle_blits < 3)       /* no view drawn since the last copy: keep the picture */
            return;
    }
    sl->idle_blits = 0;
    sl->plain = 0;
    sl->views_at_blit = sl->views;
    f->serial = ++serial;
    spin_lock();
    sl->making = sl->done;
    sl->done = f;
    spin_unlock();
    sl->making->valid = 0;
}

/* GfxBlit copied rectangle r of the back buffer to the screen: each view it holds whole goes to
 * the display with the picture made for it */
void port_s3d_blit(const void *rect)
{
    int i;
    if (!port_s3d_wanted())
        return;
    for (i = 0; i < PORT_S3D_SLOTS; i++)
        slot_blit(slot_of(i), rect);
}

static grs_canvas hud_cv;
int port_hud_noclamp;

int port_s3d_hud_begin(void)
{
    port_s3d_frame *f = slot_of(0)->making;
    long ft = grd_canvas->gc.fill_type, fp = grd_canvas->gc.fill_parm;
    if (!f->valid || !f->hud)
        return -1;
    gr_init_canvas(&hud_cv, f->hud, BMT_FLAT8, (short)(f->w + 2 * f->ex / f->kx), (short)f->h);
    hud_cv.gc.fill_type = ft;
    hud_cv.gc.fill_parm = fp;
    gr_push_canvas(&hud_cv);
    return f->ex / f->kx;
}

void port_s3d_hud_end(void)
{
    gr_pop_canvas();
}

/* "Target label" (options.c): 0 the game's block right of the target, 1 around its box */
int port_target_label;

/* a piece of the target's label as the game's text: s at (x, y) on the current canvas, each line
 * centred in the piece's width when centre (lines of the font's height, as the game's text) */
static void label_text(int font, char *s, int x, int y, int centre)
{
    grs_font *ft;
    char ln[0x80];
    short w, h, lw, lh;
    int n;
    if (!centre) {
        GfxDrawText(font, s, x, y);
        return;
    }
    ft = ResGet((Id)font);
    gr_font_string_size(ft, s, &w, &h);
    while (*s) {
        for (n = 0; s[n] && s[n] != '\n' && n < (int)sizeof ln - 1; n++)
            ln[n] = s[n];
        ln[n] = 0;
        if (n) {
            gr_font_string_size(ft, ln, &lw, &lh);
            GfxDrawText(font, ln, x + (w - lw) / 2, y);
        }
        y += ft->h;
        s += n;
        if (*s)
            s++;
    }
}

/* the target's label: drawn as the game draws it on a canvas of its own (lbuf, the text; lmask,
 * which pixels it set), then laid into the main view's sharp picture at 3/4 of the game's size,
 * each of the picture's pixels taking the text's pixel under it */
#define LW 320
#define LH 160
#define LABEL_Q 3                       /* the sharp text's size: 3/4 of the game's */
static uchar lbuf[LW * LH], lmask[LW * LH];
static uint8_t lab_pool[3][1 << 19];    /* the main view's pictures' label pixels (lab_m) */

/* a box [x0, x1) x [y0, y1) of the main view's picture being made whose marked pixels are the
 * HUD's, not the 3D's ("Anti-aliasing" leaves them sharp, platform.c): its mask, zeroed, w x h; NULL
 * if there is no room */
static uint8_t *lab_box(port_s3d_frame *f, int x0, int y0, int x1, int y1)
{
    size_t n = (size_t)(x1 - x0) * (y1 - y0);
    port_s3d_lab *l;
    uint8_t *lm;
    if (x1 <= x0 || y1 <= y0 || f->lab_n >= PORT_S3D_LABS)
        return NULL;
    if (!f->lab_m) {
        f->lab_m = lab_pool[(f - slot_of(0)->frames) % 3];
        f->lab_cap = sizeof lab_pool[0];
    }
    if (f->lab_used + n > f->lab_cap)
        return NULL;
    l = &f->lab[f->lab_n++];
    l->x = x0;
    l->y = y0;
    l->w = x1 - x0;
    l->h = y1 - y0;
    l->m = f->lab_used;
    lm = f->lab_m + f->lab_used;
    memset(lm, 0, n);
    f->lab_used += n;
    return lm;
}

static int sharp_label(int font, char *s, int x, int y, int ax, int ay, int centre)
{
    port_s3d_frame *f = slot_of(0)->making;
    grs_canvas *cv = grd_canvas, tc;
    int onhud = cv == &hud_cv, kx = f->kx, ky = f->ky, hw, hh, tw = 0, th = 0, i, j, X, Y, X0, Y0, gx, gy;
    short w, h;
    long ox, oy;
    if (!f->valid || kx < 2 || ky < 2)
        return 0;
    if (onhud)                          /* the layer's column 0: the view's -ex / kx */
        ox = -f->ex / kx, oy = 0;
    else {                              /* the canvas on the game's screen, from the view */
        long off = cv->bm.bits - tnui_back_canvas.bm.bits, row = tnui_back_canvas.bm.row;
        if (off < 0 || off >= row * tnui_back_canvas.bm.h)
            return 0;
        ox = off % row - f->x;
        oy = off / row - f->y;
    }
    gr_font_string_size(ResGet((Id)font), s, &w, &h);
    gx = x - ax * w / 2;                /* the game's text: its box's corner */
    gy = y - ay * h / 2;
    memset(lbuf, 0, sizeof lbuf);
    memset(lmask, 0, sizeof lmask);
    gr_init_canvas(&tc, lmask, BMT_FLAT8, LW, LH);
    tc.gc.fcolor = 1;
    gr_push_canvas(&tc);
    label_text(font, s, 0, 0, centre);
    gr_pop_canvas();
    gr_init_canvas(&tc, lbuf, BMT_FLAT8, LW, LH);
    tc.gc.fcolor = cv->gc.fcolor;
    tc.gc.fill_type = cv->gc.fill_type;
    tc.gc.fill_parm = cv->gc.fill_parm;
    gr_push_canvas(&tc);
    label_text(font, s, 0, 0, centre);
    gr_pop_canvas();
    for (j = 0; j < LH; j++)
        for (i = 0; i < LW; i++)
            if (lmask[j * LW + i]) {
                if (i >= tw)
                    tw = i + 1;
                th = j + 1;
            }
    /* the game's own text on its screen counts as the 3D's (f->low), so the display shows the
     * sharp picture there: where the canvas's clip let it draw */
    if (!onhud)
        for (j = 0; j < th; j++) {
            long cy = gy + j, vy = cy + oy;
            if (cy < cv->gc.clip.i.top || cy >= cv->gc.clip.i.bot || vy < 0 || vy >= f->h)
                continue;
            for (i = 0; i < tw; i++) {
                long cx = gx + i, vx = cx + ox;
                if (lmask[j * LW + i] && cx >= cv->gc.clip.i.left && cx < cv->gc.clip.i.right &&
                    vx >= 0 && vx < f->w)
                    f->low[vy * f->w + vx] = lbuf[j * LW + i];
            }
        }
    /* the sharp text: its box's point (ax, ay) where the game's text has it */
    hw = f->w * kx + 2 * f->ex;
    hh = f->h * ky;
    X0 = (int)((x + ox) * kx + f->ex) - ax * w * kx * LABEL_Q / 8;
    Y0 = (int)((y + oy) * ky) - ay * h * ky * LABEL_Q / 8;
    {
        int x0 = X0 < 0 ? 0 : X0, y0 = Y0 < 0 ? 0 : Y0, x1 = X0 + tw * kx * LABEL_Q / 4,
            y1 = Y0 + th * ky * LABEL_Q / 4;
        uint8_t *lm;
        x1 = x1 > hw ? hw : x1;
        y1 = y1 > hh ? hh : y1;
        lm = lab_box(f, x0, y0, x1, y1);        /* where its text is */
        for (Y = y0; Y < y1; Y++) {
            const uchar *m = lmask + (Y - Y0) * 4 / (ky * LABEL_Q) * LW, *b = lbuf + (Y - Y0) * 4 / (ky * LABEL_Q) * LW;
            uchar *d = f->high + (size_t)Y * hw;
            uint32_t *tr = f->tc && f->tc_rows[Y] ? f->tc + (size_t)Y * hw : NULL;
            for (X = x0; X < x1; X++) {
                int t = (X - X0) * 4 / (kx * LABEL_Q);
                if (m[t]) {
                    d[X] = b[t];
                    if (tr)             /* the game's colours there (True colour) */
                        tr[X] = 0;
                    if (lm)
                        lm[(size_t)(Y - y0) * (x1 - x0) + (X - x0)] = 1;
                }
            }
        }
    }
    return onhud;
}

/* the HUD's marks drawn sharp (mapcone.c): the main view's picture being made and where point
 * (x, y) of the current canvas is in it */
int port_s3d_paint_at(double x, double y, port_s3d_paint *p)
{
    port_s3d_frame *f = slot_of(0)->making;
    grs_canvas *cv = grd_canvas;
    long ox, oy;
    if (!port_s3d_wanted() || !f->valid || f->kx < 2 || f->ky < 1 || !f->high)
        return 0;                       /* (2x in 320x400: ky 1, as the maps' sharp pictures) */
    if (cv == &hud_cv)                  /* the layer's column 0: the view's -ex / kx */
        ox = -f->ex / f->kx, oy = 0;
    else {                              /* the canvas on the game's screen, from the view */
        long off = cv->bm.bits - tnui_back_canvas.bm.bits, row = tnui_back_canvas.bm.row;
        if (off < 0 || off >= row * tnui_back_canvas.bm.h)
            return 0;
        ox = off % row - f->x;
        oy = off / row - f->y;
    }
    p->high = f->high;
    p->tc = f->tc;
    p->tc_rows = f->tc_rows;
    p->w = f->w * f->kx + 2 * f->ex;
    p->h = f->h * f->ky;
    p->kx = f->kx;
    p->ky = f->ky;
    p->X = (x + ox) * f->kx + f->ex;
    p->Y = (y + oy) * f->ky;
    p->f = f;
    return 1;
}

uint8_t *port_s3d_paint_sharp(const port_s3d_paint *p, int x0, int y0, int x1, int y1)
{
    return lab_box(p->f, x0, y0, x1, y1);
}

void port_label_piece(int font, char *s, int x, int y, int ax, int ay, int centre)
{
    short w, h;
    if (!*s)
        return;
    if (port_s3d_wanted() && sharp_label(font, s, x, y, ax, ay, centre))
        return;
    gr_font_string_size(ResGet((Id)font), s, &w, &h);
    label_text(font, s, x - ax * w / 2, y - ay * h / 2, centre);
}

const port_s3d_frame *port_s3d_take(int slot)
{
    Slot *sl;
    if (!port_s3d_wanted())
        return NULL;
    spin_lock();
    sl = slot_of(slot);
    if (sl->done->serial > sl->shown->serial) {
        port_s3d_frame *t = sl->shown;
        sl->shown = sl->done;
        sl->done = t;
    }
    spin_unlock();
    return sl->shown->valid ? sl->shown : NULL;
}
