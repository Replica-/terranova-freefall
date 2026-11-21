/* fog.c -- Freefall: "Fog" on the Distance page (On / Off), in the sharp 3D view.
 *
 * The land fades into the sky's colour at the horizon (the colour rndsky.c gives the rows below
 * the horizon: the fog colour) with its distance from the viewer, and past the fog's end it is
 * not drawn at all: there the sky's fill below the horizon shows, the same colour, and the far
 * terrain caster stops (haze.c), so the fog makes the drawing faster too.  The fog is by the true
 * distance on the map (not the caster's square rings), so it does not move as the head turns.
 *
 * Every pixel's colour goes through port_fog_tab[f] last: the palette colour nearest to the mix
 * of the colour and the fog colour, f / 16 of the way, f 0..16 (dithered between two levels).
 * Water colours stay water colours and the land's stay the land's: the water passes (rndsky.c)
 * find the water by its colours.  The fog colour itself is the first water colour (0xA2) under
 * a day sky; while the fog is on it is the land's (sky_water[0xA2] = 0 during the sharp view),
 * and the water's 0xA2 takes the next water colour.
 *   - haze.c: the caster's columns (per point of each ring, the columns between two points
 *     in between; colras.c draws them through port_fog_rows), the smooth far land of Terrain
 *     detail High per pixel, the end;
 *   - weather.c: the smooth ground per square (port_fog_ltab);
 *   - viewdist.c: the objects (units, trees, buildings) per object, at most 3/4 fogged so a unit
 *     in the fog can still be seen; the moving units out to the game's own 120 squares are all
 *     still drawn (haze.c), past the land's end too.
 * In the game's infrared (the I key) the 3D is drawn through the camera's colour table (the
 * canvas's CLUT, planet_tab3b) into the infrared ramp 0xB0..0xE6 (infrared.c): the fog is then
 * the ramp's colour of the sky's haze above the horizon (rndsky_2A7654, through planet_tab3c;
 * the haze's last rows and the fill below the horizon drawn in it too, rndsky_2A7078),
 * port_fog_tab mixes along the ramp, and the land's and the objects' colour tables go through it
 * (port_fog_clut) in place of their light tables.
 */
#include <math.h>
#include <string.h>
#include "g7_a.h"
#include "g5_c.h"
#include "port.h"
#include "sharp3d.h"

int port_fog;                           /* 0 Off, 1 On (screen.c) */
int port_fog_on;                        /* this sharp drawing is fogged */
int port_fog_ir;                        /* ... and it is the game's infrared (fog_clut) */
long port_fog_obj = -1;                 /* the infrared: the object being drawn's fog, -1 none */
uchar port_fog_tab[17][256];
const uchar *port_fog_rows[17];         /* the caster's ring: its haze row through each level */
const uchar *port_fog_row0;             /* ... the haze row itself (True colour) */
volatile int port_fog_cols;             /* colras.c draws the caster's columns fogged */
ushort port_fog_f[4096];                /* per column (sharp3d.c MAX_COLS) its fog, 0..0x1000 */
uchar port_fog_dd[8][8];                /* 8 x 8 dither thresholds, 0..255 */

/* start and end of the fog in squares at the game's zoom: Off, On */
static const long fog_start[2] = { 0, 80 }, fog_end[2] = { 0, 240 };

static ulong fog_key;                   /* the tables' version (a new palette, fog colour, planet) */
static ulong fog_key_base;              /* the fog colour and the planet's tables */
static grs_rgb fog_pal[256];            /* the palette the tables were made for */
static uchar fog_cyc[256];              /* the palette entries that cycle */
static long fog_fade = -1;
static uchar fog_was_water;             /* sky_water[fade] before the drawing */
static long fog_e;                      /* the end (fix, zoomed) */
static unsigned long fog_e2;            /* (end in 1/64 squares)^2 */
static unsigned long fog_mul;           /* 2^42 / fog_e2: (d2 * fog_mul) >> 32 is d2 * 1024 / fog_e2 */
static ushort fog_q[1025];              /* fog by distance^2 / end^2 */
static double fog_S = 0, fog_E = 1;     /* the start and end the curve has (squares; port_fog_gpu) */
static long fog_view_b = -1, fog_view_e = -1, fog_view_lvl = -1, fog_view_wx = -1;
static const uchar *fog_clut;           /* the infrared: the camera's colour table */
#define IR_HOT 0xB0                     /* the infrared ramp (infrared.c): the hottest colour ... */
#define IR_COLD 0xE6                    /* ... the coldest */

/* the composite tables, made when first needed after a change of fog_key */
static uchar fog_rows[16][17][256];     /* planet_shade row lvl, then the fog */
static ulong fog_rows_key[16];
#define FOG_LTABS 20                    /* the 16 haze levels' light tables, then the objects' 4 */
static uchar fog_lt[FOG_LTABS][17][0x1800];
static ulong fog_lt_key[FOG_LTABS][17];
#define FOG_CLUTS 8                     /* the infrared: colour tables through the fog */
static const uchar *fc_clut[FOG_CLUTS];
static uchar fc_tab[FOG_CLUTS][17][256];
static ulong fc_key[FOG_CLUTS][17];
static int fc_next;

static void rgb(uchar c, long *r, long *g, long *b)
{
    grs_rgb w = grd_bpal[c];
    *r = (long)(w & 0x3FF) >> 2;
    *g = (long)(w >> 13) & 0xFF;
    *b = (long)(w >> 24) & 0xFF;
}

static int in_water(long c)
{
    return c >= 0xA2 && c <= 0xAF;
}

static void fog_tables(long fade)
{
    long pr[256], pg[256], pb[256], fr, fg, fb, f, c, k;
    uchar kw[256];                      /* a water colour for the fog (the fog colour is not) */
    for (c = 0; c < 256; c++) {
        rgb((uchar)c, &pr[c], &pg[c], &pb[c]);
        kw[c] = in_water(c) && c != fade;
    }
    fr = pr[fade];
    fg = pg[fade];
    fb = pb[fade];
    for (f = 0; f <= 16; f++)
        for (c = 0; c < 256; c++) {
            long tr = pr[c] + (fr - pr[c]) * f / 16, tg = pg[c] + (fg - pg[c]) * f / 16,
                 tb = pb[c] + (fb - pb[c]) * f / 16, best = 0x7FFFFFFF, w = in_water(c);
            uchar pick = (uchar)c;
            if (c == 0 || c == 0xFF || (!f && kw[c] == w)) {    /* the shade tables keep 0 / 0xFF */
                port_fog_tab[f][c] = (uchar)c;
                continue;
            }
            if (!w && f == 16) {
                port_fog_tab[f][c] = (uchar)fade;
                continue;
            }
            for (k = 1; k < 0xFF; k++) {
                long d;
                if (kw[k] != w || fog_cyc[k])
                    continue;
                d = (pr[k] - tr) * (pr[k] - tr) * 3 + (pg[k] - tg) * (pg[k] - tg) * 4 +
                    (pb[k] - tb) * (pb[k] - tb) * 2;
                if (d < best)
                    best = d, pick = (uchar)k;
            }
            port_fog_tab[f][c] = pick;
        }
}

/* the infrared: the ramp's colours f / 16 of the way to the fog's, the rest as they are */
static void ir_tables(long fade)
{
    long f, c;
    for (f = 0; f <= 16; f++)
        for (c = 0; c < 256; c++)
            port_fog_tab[f][c] = (uchar)(c >= IR_HOT && c <= IR_COLD ? (c * (16 - f) + fade * f + 8) >> 4 : c);
}

/* the fog colour: what the sky fills the rows below the horizon with (rndsky_2A7A6C): the
 * colour of the haze at the horizon (0xA2, white-blue, under a day sky), black under the stars */
static long fog_colour(const FfCamera *c)
{
    if (FF_CAM_NOSKY(c))
        return sky_none.color & 0xFF;
    if (game_35CADC >= 1)
        return 0;                       /* sky_flat's (the fog is off from 0x10 on) */
    if (c->sky && c->sky->stars)        /* rndsky_2A6BE0 */
        return 0;
    if (c->sky && sky_force)
        return c->sky->color & 0xFF;
    return (game_442448 >> 24) - 1 == 1 ? 0x20 : 0xA2;     /* rndsky_2A7078 */
}

/* at the start of a sharp drawing (sharp3d.c), with the game's camera and its canvas's colour
 * table (the infrared) or NULL */
void port_fog_begin(const void *cam, const void *clut)
{
    long fade, i, ch = 0, fresh = 0;
    ulong key;
    port_fog_on = 0;
    port_fog_ir = 0;
    port_fog_obj = -1;
    port_fog_cols = 0;
    if (!port_fog || game_35CADC >= 0x10 || !cam)
        return;
    fade = fog_colour(cam);
    if (clut) {
        /* the infrared: the colour of the sky's haze above the horizon through the sky's table
         * (its last rows, the haze's lightest, come out warmer: a pink in Sci-fi's thermal
         * colours, so the far land faded into pink under a purple sky); rndsky_2A7078 draws
         * those rows in it too */
        if (fade == 0xA2 || fade == 0x20)
            fade += 3;
        fade = (clut == planet_tab3b ? planet_tab3c : planet_tab3f)[fade];
        if (fade < IR_HOT || fade > IR_COLD)
            return;
    }
    fog_clut = port_ir_land_clut(clut);  /* (Sci-fi's ground never hot, infrared.c) */
    key = (ulong)fade * 0x9E3779B1u ^ (ulong)planet_shade ^ (ulong)planet_light * 7 ^ (ulong)fog_clut * 13;
    for (i = 0; i < 0x1000; i += 61)
        key = key * 31 + planet_shade[i] + planet_light[i] * 0x100;
    for (i = 0; fog_clut && i < 0x100; i += 5)
        key = key * 31 + fog_clut[i];
    /* a few entries changing alone are the palette's colour cycling (0xFC..0xFE every few
     * frames in the missions): never a fog colour, and no new tables for them */
    for (i = 0; i < 0x100; i++)
        if (grd_bpal[i] != fog_pal[i]) {
            ch++;
            fresh += !fog_cyc[i];
        }
    if (ch && ch <= 8)
        for (i = 0; i < 0x100; i++)
            if (grd_bpal[i] != fog_pal[i] && !fog_cyc[i])
                fog_cyc[i] = 1;
    if (fresh || key != fog_key_base || fade != fog_fade) {
        memcpy(fog_pal, grd_bpal, sizeof fog_pal);
        fog_key_base = key;
        fog_key++;
        fog_fade = fade;
        if (clut)
            ir_tables(fade);
        else
            fog_tables(fade);
    }
    if (!port_fog_dd[0][0]) {
        static const uchar bayer[8][8] = {
            {  0, 32,  8, 40,  2, 34, 10, 42 }, { 48, 16, 56, 24, 50, 18, 58, 26 },
            { 12, 44,  4, 36, 14, 46,  6, 38 }, { 60, 28, 52, 20, 62, 30, 54, 22 },
            {  3, 35, 11, 43,  1, 33,  9, 41 }, { 51, 19, 59, 27, 49, 17, 57, 25 },
            { 15, 47,  7, 39, 13, 45,  5, 37 }, { 63, 31, 55, 23, 61, 29, 53, 21 },
        };
        long x, y;
        for (x = 0; x < 8; x++)
            for (y = 0; y < 8; y++)
                port_fog_dd[x][y] = (uchar)(bayer[(y + 1) & 7][(x + 4) & 7] * 4 + 2);
    }
    if (!clut) {
        fog_was_water = sky_water[fade];
        sky_water[fade] = 0;            /* the fog colour is the land's (above) */
    }
    fog_view_lvl = -1;
    port_fog_ir = clut != NULL;
    port_fog_on = 1;
}

/* the fog colour of this drawing (True colour, truecol.c) */
long port_fog_colour(void)
{
    return fog_fade;
}

void port_fog_end(void)
{
    if (port_fog_on && !port_fog_ir)
        sky_water[fog_fade] = fog_was_water;
    port_fog_on = 0;
    port_fog_ir = 0;
    port_fog_obj = -1;
    port_fog_cols = 0;
}

/* the fog's distances for a camera: the bands' zoom scale (FfCameraSetup) */
void port_fog_view(const FfCamera *c)
{
    long k = c->band_end - 1, b, e, i;
    double s, S, E;
    if (k < 0 || k > 5)
        return;
    b = c->band[k].b;
    e = c->band[k].e;
    if (b == fog_view_b && e == fog_view_e && port_fog == fog_view_lvl && game_35CADA == fog_view_wx)
        return;
    fog_view_b = b;
    fog_view_e = e;
    fog_view_lvl = port_fog;
    fog_view_wx = game_35CADA;
    s = e > 0 && b > 0 ? (double)b / e : 1.0;
    S = fog_start[port_fog] * s;
    E = fog_end[port_fog] * s;
    if (game_35CADA != 0xFF) {          /* weather fog: rain or snow, nearer and thicker */
        S *= 0.55;
        E *= 0.7;
    }
    fog_e = (long)(E * 65536.0);
    E = E < 1.0 ? 1.0 : E > 500.0 ? 500.0 : E;
    fog_e2 = (unsigned long)(E * 64.0) * (unsigned long)(E * 64.0);
    fog_mul = (unsigned long)((1ull << 42) / fog_e2);
    /* the gentler curve: t^2 (2 - t) from the start to the end, the near land clear longer than
     * an even fade (3/8 fogged half-way), thickening faster toward the end */
    fog_S = S;
    fog_E = E;
    for (i = 0; i <= 1024; i++) {
        double d = E * sqrt(i / 1024.0), t = (d - S) / (E - S);
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        fog_q[i] = (ushort)(t * t * (2 - t) * 0x1000 + 0.5);
    }
}

/* the fog of a point (dx, dy) from the viewer (fix, squares): 0 .. 0x1000 (16 levels << 8).
 * In 1/64 squares and without a division: it is asked for every ring point, square and object */
long port_fog_at(fix dx, fix dy)
{
    long x = dx >> 10, y = dy >> 10;
    unsigned long d2;
    if (x < -32767 || x > 32767 || y < -32767 || y > 32767)
        return 0x1000;
    d2 = (unsigned long)(x * x) + (unsigned long)(y * y);
    if (d2 >= fog_e2)
        return 0x1000;
    return fog_q[(unsigned long)(((unsigned long long)d2 * fog_mul) >> 32)];
}

/* the GPU ground (gpuland.c), which fogs each pixel by its distance: this drawing's fog, its start
 * and end (squares, the curve t^2 (2 - t) between) and colour (the palette's); 0: none here
 * (off, or the infrared's, which the game's own drawing keeps) */
int port_fog_gpu(float *start, float *end, int *colour)
{
    if (!port_fog_on || port_fog_ir || fog_fade < 0)
        return 0;
    *start = (float)fog_S;
    *end = (float)fog_E;
    *colour = (int)(fog_fade & 0xFF);
    return 1;
}

/* the last ring the caster draws (squares): everything farther is all fog */
long port_fog_ring(void)
{
    return (fog_e + 0xFFFF) >> 16;
}

/* the caster's ring: its haze level's row through each fog level, for colras.c (the infrared:
 * the camera's colour table, strip.c's columns' last table there) */
void port_fog_level(long lvl)
{
    long f, c;
    lvl &= 15;
    if (fog_rows_key[lvl] != fog_key) {
        const uchar *row = port_fog_ir ? fog_clut : planet_shade + (lvl << 8);
        for (f = 0; f <= 16; f++)
            for (c = 0; c < 256; c++)
                fog_rows[lvl][f][c] = port_fog_tab[f][row[c]];
        fog_rows_key[lvl] = fog_key;
    }
    for (f = 0; f <= 16; f++)
        port_fog_rows[f] = fog_rows[lvl][f];
    port_fog_row0 = planet_shade + (lvl << 8);
}

/* a light table (0x1800 bytes: 24 light rows) through fog level f (also 0: the water's fog
 * colour; the tables follow fog_key); slot: 0..15 the haze levels'
 * (sharp3d.c), 16..19 the objects' (planet_light, planet_shaded[0..2]) */
const uchar *port_fog_ltab(int slot, const uchar *base, int f)
{
    long j;
    uchar *d;
    const uchar *t;
    if (slot < 0 || slot >= FOG_LTABS || port_fog_ir)
        return base;
    f = f < 0 ? 0 : f > 16 ? 16 : f;
    d = fog_lt[slot][f];
    if (fog_lt_key[slot][f] != fog_key) {
        t = port_fog_tab[f];
        for (j = 0; j < 0x1800; j++)
            d[j] = t[base[j]];
        fog_lt_key[slot][f] = fog_key;
    }
    return d;
}

/* the infrared: the colour table clut through fog level f (the land's squares, weather.c; the
 * objects, drawobj.c), the tables made when first needed after a change of fog_key */
const uchar *port_fog_clut(const uchar *clut, long f)
{
    int i;
    long c;
    uchar *d;
    if (!port_fog_on || !port_fog_ir || !clut || f <= 0)
        return clut;
    f = f > 16 ? 16 : f;
    for (i = 0; i < FOG_CLUTS && fc_clut[i] != clut; i++)
        ;
    if (i == FOG_CLUTS) {
        i = fc_next;
        fc_next = (fc_next + 1) % FOG_CLUTS;
        fc_clut[i] = clut;
        memset(fc_key[i], 0, sizeof fc_key[i]);
    }
    d = fc_tab[i][f];
    if (fc_key[i][f] != fog_key) {
        for (c = 0; c < 256; c++)
            d[c] = port_fog_tab[f][clut[c]];
        fc_key[i][f] = fog_key;
    }
    return d;
}
