/* truecol.c -- Freefall: "True colour" on the Detail page (Off / On / On + sun glow), in the
 * main sharp 3D view.
 *
 * The game draws in 256 colours: its sky fades to the horizon in steps, its clouds are blocks of
 * a few colours, and the fog (fog.c) is a palette colour dithered between 16 levels.  With True
 * colour the picture is still drawn as before (everything that reads it, the water's passes and
 * the HUD, sees the same colours), but while the main view is drawn the pixels that are sky or
 * fogged are noted (port_tc_mark: what they are, their colour before the fog, how much fog),
 * and after the drawing a pass works out their colours in full colour:
 *   - the clouds (rndsky.c, port_tc_sky_rows): each pixel the four texels around its point on
 *     a softened copy of the cloud texture mixed (soft clouds), with a finer layer of the
 *     clouds' own light so they don't turn blurry, then toward the haze by where the row is
 *     between its two distance steps (a smooth fade to the horizon instead of the steps);
 *     worked out on every second row and column, the pixels between mixed from their neighbours;
 *   - the fog: the colour before the fog mixed with the fog colour by the exact amount (no
 *     dither, no levels);
 *   - On + sun glow: the sky around the sun lighter (a small bright halo and a wide soft one),
 *     warmer and wider as the sun gets low, and with the fog on the fog colour too: lighter and
 *     warmer toward the sun along the horizon (sun-side fog), the land fading into it.
 * A pixel drawn over since it was noted (a unit, a tree, a shot, the rain) keeps the game's
 * colour, but the smoke's see-through discs over the sky are mixed with its full colour
 * (rndsky.c, port_tc_smoke_rgb).  The display (platform.c) shows the pass's colours where it made one (port_s3d_frame
 * tc, with the colour settings), the game's elsewhere.  Not in the game's infrared (the I key). */
#include <math.h>
#include <stddef.h>
#include <string.h>
#ifndef _WIN32
#include <sys/mman.h>
#else
void *malloc(size_t n);                 /* (<stdlib.h>'s valloc is the game's, 2d.h) */
void free(void *p);
#endif
#include "g7_a.h"
#include "g5_c.h"
#include "port.h"
#include "platform.h"
#include "sharp3d.h"
#include "gpuland.h"

int port_truecol;                       /* 0 Off, 1 On, 2 On + sun glow (screen.c) */

/* the True colour in use: GPU rendering (gpuland.c) has no palette, so it is On there at least */
int port_tc_mode(void)
{
    return port_truecol ? port_truecol : port_gl ? 1 : 0;
}
int port_tc_on;                         /* this drawing is noted: 1, 2 with the glow */
uchar *port_tc_base;
ulong *port_tc_info;
ulong port_tc_n;
uchar *port_tc_row;
ulong port_tc_pal[256];

#define TC_COLS 4096                    /* sharp3d.c MAX_COLS */
#define TC_ROWS 2048
#define HALO_N 1024
#define HALO_D2 0.64f                   /* the halo's reach (radians squared) */

/* the pictures' planes, one for each of the view's three pictures: while one is drawn, what
 * each pixel is (kind << 24 | fog << 16 | colour before the fog << 8 | the colour drawn; the sky
 * kinds: smoke discs << 16 | their table << 8 instead, rndsky.c tc_smoke_note); after
 * the pass its full colour (0xFE000000 | RGB) or 0 (the game's colour).  From the system, not
 * the heap: the game thread's heap stays as without it (frame comparisons, lockstep.c) */
static ulong *plane[3];
static size_t plane_n[3];
static uchar rows_of[3][TC_ROWS];       /* the rows with full colours (the others: none at all) */
static ulong *cur;
static int cur_w, cur_h;
static long tc_fog = -1;                /* the fog colour (fog.c), -1: no fog this drawing */

/* the glow: the sun's place on the picture (angles from the view, radians), the colour */
static int glow_on;
static float sun_a, sun_b, sun_low, horizon_b, glow_k;
static long cam_hw, cam_hh, cam_zx, cam_zy;
static float col_da[TC_COLS], col_az[TC_COLS];  /* per column: angle from the sun, its side's glow */
static float row_db[TC_ROWS], row_hz[TC_ROWS];  /* per row: angle from the sun, near the horizon */
static float halo[HALO_N + 1];
static int glow_r, glow_g, glow_b;

static void *plane_alloc(size_t n)
{
#ifdef _WIN32
    return malloc(n);
#else
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

static void plane_free(void *p, size_t n)
{
#ifdef _WIN32
    (void)n;
    free(p);
#else
    if (p)
        munmap(p, n);
#endif
}

static ulong mix(ulong a, ulong b, long f)    /* a to b, f 0..256 */
{
    ulong rb = (((a & 0xFF00FF) * (ulong)(256 - f) + (b & 0xFF00FF) * (ulong)f) >> 8) & 0xFF00FF;
    ulong g = (((a & 0xFF00) * (ulong)(256 - f) + (b & 0xFF00) * (ulong)f) >> 8) & 0xFF00;
    return rb | g;
}

/* the main view's picture high (W x H, picture idx of three) is about to be drawn */
void port_tc_begin(int idx, uchar *high, int W, int H)
{
    size_t n = (size_t)W * H;
    int i;
    port_tc_on = 0;
    cur = NULL;
    glow_on = 0;
    if (!port_tc_mode() || game_35ADE0 == 2 || idx < 0 || idx > 2 || W <= 0 || W > TC_COLS || H <= 0 || H > TC_ROWS)
        return;
    if (plane_n[idx] < n) {
        plane_free(plane[idx], plane_n[idx] * sizeof(ulong));
        plane[idx] = plane_alloc(n * sizeof(ulong));
        plane_n[idx] = plane[idx] ? n : 0;
        if (!plane[idx])
            return;
    }
    for (i = 0; i < 256; i++) {        /* the colours the display shows (platform.c) */
        int sh = port_video.dac_bits == 8 ? 0 : 2;
        ulong r = (ulong)port_video.dac[i * 3] << sh, g = (ulong)port_video.dac[i * 3 + 1] << sh,
              b = (ulong)port_video.dac[i * 3 + 2] << sh;
        if (sh) {                       /* 6-bit DAC: 63 -> 255 */
            r |= r >> 6;
            g |= g >> 6;
            b |= b >> 6;
        }
        port_tc_pal[i] = (r & 0xFF) << 16 | (g & 0xFF) << 8 | (b & 0xFF);
    }
    cur = plane[idx];
    port_tc_row = rows_of[idx];
    memset(port_tc_row, 0, (size_t)H);
    cur_w = W;
    cur_h = H;
    port_tc_base = high;
    port_tc_info = cur;
    port_tc_n = (ulong)n;
    port_tc_on = port_tc_mode();
}

void port_tc_sun(int ok, long a, long b, long elev, long horizon, long half_w, long half_h, long zx, long zy)
{
    const float rad = 6.2831853f / 65536.0f;
    if (port_tc_on < 2 || !ok || zx <= 0 || zy <= 0)
        return;
    sun_a = (short)a * rad;
    sun_b = (short)b * rad;
    horizon_b = (short)horizon * rad;
    sun_low = 1.0f - (short)elev * rad / 0.5f;  /* 1 at the horizon, 0 from 29 degrees up */
    sun_low = sun_low < 0 ? 0 : sun_low > 1 ? 1 : sun_low;
    cam_hw = half_w;
    cam_hh = half_h;
    cam_zx = zx;
    cam_zy = zy;
    glow_on = (short)elev > -0x200;     /* not below the land */
}

/* the glow's tables for the picture: per column and row, the halo by the distance from the sun */
static void glow_tables(void)
{
    float low = sun_low * sun_low, s1 = 0.045f + 0.03f * low, s2 = 0.20f + 0.15f * low;
    int x, y, i;
    for (i = 0; i <= HALO_N; i++) {
        float d2 = HALO_D2 * i / HALO_N;
        halo[i] = 0.45f * expf(-d2 / (2 * s1 * s1)) + (0.22f + 0.12f * low) * expf(-d2 / (2 * s2 * s2));
    }
    halo[HALO_N] = 0;
    for (x = 0; x < cur_w; x++) {
        float ang = atanf((float)(x + 0.5f - cam_hw) * 65536.0f / cam_zx), d = ang - sun_a;
        if (d > 3.14159265f)
            d -= 6.2831853f;
        if (d < -3.14159265f)
            d += 6.2831853f;
        col_da[x] = d;
        col_az[x] = sun_low * 0.42f * expf(-d * d / (2 * 0.55f * 0.55f));
    }
    for (y = 0; y < cur_h; y++) {
        float ang = atanf((float)(y + 0.5f - cam_hh) * 65536.0f / cam_zy), el = horizon_b - ang;
        row_db[y] = ang - sun_b;
        /* the sun side's light along the horizon: on the fog (below it) and the sky just above */
        row_hz[y] = el <= 0 ? 1.0f : expf(-el * el / (2 * 0.10f * 0.10f));
    }
    /* white-yellow by day, orange low down */
    glow_r = 255;
    glow_g = (int)(250 - 80 * low);
    glow_b = (int)(232 - 150 * low);
    glow_k = game_35CADA != 0xFF ? 0.45f : 1.0f;    /* rain, snow: through the clouds */
}

/* how much glow at (x, y): sky 1 (the halo and the horizon's), else the fog colour's: all of it
 * when the fog is on, none below the horizon without (the land there is not fogged) */
static float glow_at(int x, int y, int sky)
{
    float dx = col_da[x], dy = row_db[y], d2 = dx * dx + dy * dy, g = 0;
    int i = (int)(d2 * (HALO_N / HALO_D2));
    if (i < HALO_N)
        g = halo[i];
    if (tc_fog >= 0)
        g += col_az[x] * row_hz[y];
    else if (sky) {
        float el = horizon_b - (row_db[y] + sun_b);
        if (el < 0.02f)                 /* fading out at the horizon */
            g *= el <= 0 ? 0 : el / 0.02f;
        g += col_az[x] * row_hz[y] * (el <= 0 ? 0 : el < 0.02f ? el / 0.02f : 1);
    } else
        return 0;
    g *= glow_k;
    return g > 0.85f ? 0.85f : g;
}

/* lighter by the glow's light (a screen, so the sky's colour stays in it and never greys), and
 * tinted toward the light's colour by half as much */
static ulong glow_px(ulong c, float g)
{
    long f = (long)(g * 256), r, gg, b;
    if (f <= 0)
        return c;
    r = (long)(c >> 16 & 0xFF);
    gg = (long)(c >> 8 & 0xFF);
    b = (long)(c & 0xFF);
    r += (255 - r) * f * glow_r >> 16;
    gg += (255 - gg) * f * glow_g >> 16;
    b += (255 - b) * f * glow_b >> 16;
    return mix((ulong)r << 16 | (ulong)gg << 8 | (ulong)b, (ulong)glow_r << 16 | (ulong)glow_g << 8 | (ulong)glow_b, f / 2);
}

/* a row with any glow: near the sun's height, or along the horizon with the sun low */
static int glow_row(int y)
{
    return row_db[y] * row_db[y] < HALO_D2 || row_hz[y] * sun_low * 0.42f > 0.002f;
}

/* the rows of part: in place, each noted pixel the picture still has to its full colour, every
 * other one 0 (the clouds first, rndsky.c: to 0xFD000000 | RGB, before the glow) */
static void tc_part(void *arg, int part, int thread)
{
    int y0 = part * 16, y1 = y0 + 16 < cur_h ? y0 + 16 : cur_h, y, x, W = cur_w;
    ulong fogc = tc_fog >= 0 ? port_tc_pal[tc_fog] : 0;
    (void)arg;
    port_tc_sky_rows(y0, y1, cur, port_tc_base, W, port_tc_row, thread);
    for (y = y0; y < y1; y++) {
        ulong *o = cur + (size_t)y * W;
        const uchar *hi = port_tc_base + (size_t)y * W;
        int gr = glow_on && glow_row(y);
        if (!port_tc_row[y])            /* nothing noted: the display does not look (tc_rows) */
            continue;
        for (x = 0; x < W; x++) {
            ulong v = o[x], k = v >> 24, c;
            if (k == 0xFD) {            /* the clouds */
                c = v & 0xFFFFFF;
                o[x] = 0xFE000000u | (gr ? glow_px(c, glow_at(x, y, 1)) : c);
                continue;
            }
            if (k - PORT_TC_FOG > PORT_TC_FILL - PORT_TC_FOG ||
                (k == PORT_TC_FILL && (v & 0xF0000) ? port_tc_smoke_px(v) : (uchar)v) != hi[x]) {
                if (v)                  /* (most are 0 already: not written again) */
                    o[x] = 0;
                continue;
            }
            if (k == PORT_TC_FOG && tc_fog >= 0) {
                c = gr ? glow_px(fogc, glow_at(x, y, 0)) : fogc;
                o[x] = 0xFE000000u | mix(port_tc_pal[(v >> 8) & 0xFF], c, (long)((v >> 16) & 0xFF) + ((v >> 23) & 1));
            } else if (k == PORT_TC_FILL) {
                c = gr ? glow_px(port_tc_pal[v & 0xFF], glow_at(x, y, 1)) : port_tc_pal[v & 0xFF];
                o[x] = 0xFE000000u | (v & 0xF0000 ? port_tc_smoke_rgb(v, c) : c);    /* (the smoke over it) */
            }
            else                        /* (clouds not of the smooth kind) */
                o[x] = 0;
        }
    }
}

/* after the drawing: the full colours, the picture's plane for the display (NULL: none), *rows
 * which of its rows have them */
ulong *port_tc_end(const uchar **rows)
{
    ulong *p = cur;
    *rows = port_tc_row;
    if (!port_tc_on || !cur) {
        port_tc_on = 0;
        cur = NULL;
        return NULL;
    }
    tc_fog = port_fog_on ? port_fog_colour() : -1;
    if (glow_on)
        glow_tables();
    port_parallel(tc_part, NULL, (cur_h + 15) / 16);
    port_tc_on = 0;
    port_tc_info = NULL;
    port_tc_n = 0;
    cur = NULL;
    glow_on = 0;
    return p;
}
