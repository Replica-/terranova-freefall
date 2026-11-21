/* haze.c -- Terra Nova module M095 (0x2B3980-0x2B5C90; source file name unknown, the assert
 * names the "hazeRadius" table and checkHazingOffset): the terrain ray caster of the 3D view
 * (community name RAY_SITE).  Decompiled from __FF.EXE v1.09.
 *
 * haze_2B5484 walks the map in square rings around the viewer, nearest first.  Along the main
 * axis of the view (x or y, hz_xmajor) every ring is one row of map squares; the row is clipped
 * to the view by bisection (haze_2B458C ...) and then walked square by square (haze_2B4A70 ...):
 * each square is projected (thunder_2B2930), the visible part of the terrain silhouette is
 * drawn as a span of the column buffer (haze_2B3E9C -> strip_2AC19C) and the objects on the
 * square go into the strip lists (haze_2B44FC -> viewdist_2A13B8).  Moving objects (the cell
 * grid of M071) are collected per ring (haze_2B3BA4); those farther away than the ring are
 * parked in hz_head buckets by distance and drawn when the ring gets there (haze_2B3B20).  From
 * the ring distances in hz_bands on, the step doubles (2, 4 ...); from step 4 on the coarse map
 * grid is used (hz_coarse).
 *
 * Functions (names ours): 2B3980 object visible, 2B399C draw one object, 2B3A54 / 2B3A7C reset
 * the parked lists, 2B3A94 park an object, 2B3B20 draw a bucket, 2B3BA4 objects of a ring row,
 * 2B3D44 tile objects of a square, 2B3D7C square has objects, 2B3E9C draw a span, 2B3FD0 cell
 * pointer, 2B402C translation, 2B40C8 point in view, 2B4198 project a point, 2B42C0 start a walk,
 * 2B4428 step the walk, 2B44FC objects between two columns, 2B458C .. 2B4674 bisection / search,
 * 2B46D0 .. 2B4748 averaged shade, 2B4780 below the silhouette, 2B47C0 .. 2B49C4 first visible
 * point, 2B4A70 .. 2B4D88 walk a span, 2B4E8C / 2B4ED0 haze radius table, 2B4EFC / 2B4F68 span
 * ends, 2B4FD0 .. 2B52D0 clip and walk, 2B53B0 band distances, 2B5448 debug pixel, 2B5484 the
 * caster, 2B5B70 / 2B5BA4 camera playback (unreferenced debug code).
 */
#include "g5_a.h"
#include "g5_c.h"      /* TerrBin, viewdist_2A1160 / 2A13B8 */
#include "g6_a.h"     /* added at the G5-G8 merge */
#include "g7_b.h"     /* added at the G5-G8 merge */

#define STRIP ((TerrBin *)hview_strip)
#define ENTP(i) (&game_entities[(short)(i)])
#define SHADE(p) ((*(ushort *)(p) >> 6) & 0xE)
#ifndef TN_WATCOM_ABI
/* the GPU ground (port/video/gpuland.c): the far land's pixels noted while it is drawn */
extern unsigned short *port_gl_shc;
extern const unsigned char *port_gl_sh_base;
#define GL_NOTE_FAR(q) do { if (port_gl_shc) port_gl_shc[(q) - port_gl_sh_base] = (unsigned short)(0x100 | *(q)); } while (0)
#else
#define GL_NOTE_FAR(q) ((void)0)
#endif

static fix fmul(fix a, fix b)
{
    return (fix)(((long long)a * b) >> 16);
}

/* The original's idiv (edx:eax = a << 16): a quotient that does not fit 32 bits raises the
 * divide error as in the original (found at the G5-G8 merge: mission 25, frame 47, where the
 * far terrain projects a point at d just above 0x10 in haze_2B4198 and the original faults;
 * a 64-bit C division went on with a truncated value). */
static fix fdiv(fix a, fix b)
{
    fix r;
    long hi = a >> 16;
    __asm__ volatile("idivl %3" : "=a"(r), "+d"(hi) : "0"((long)((ulong)a << 16)), "rm"(b) : "cc");
    return r;
}

static long iabs(long v) { return v < 0 ? -v : v; }

uchar haze_2B3980(short ent)
{
    return viewdist_2A0E54(ENTP(ent));
}

void haze_2B399C(short ent)
{
    GameEntity *e = ENTP(ent);
    fix sx, sy;
    long col;
    if (!viewdist_2A0E54(e))
        return;
    if (haze_2B4198(e->x >> 16, e->y >> 16, &sx, &sy)) {
        col = sx >> 16;
        if (col < 0)
            col = 0;
        else if (col >= hz_ncols)
            col = hz_ncols - 1;
        viewdist_2A1160(STRIP, ent, col, hview_cols[col]);
    } else
        ai_seen_bits[ent >> 3] |= 1 << (ent & 7);
}

#ifndef TN_WATCOM_ABI
/* the sharp view with more terrain detail (port/video/sharp3d.c): its far bands reach past the 0x80
 * parking buckets of the game; a bucket is the distance along a grid line (up to 0x200).  Its
 * parked objects: room for every entity (the game's 63 run out with Object distance), set up
 * again for every drawing. */
static short big_head[0x200];
static HzNode big_nodes[0x140];
#define HZ_HEAD (port_s3d_on ? big_head : hz_head)
#define HZ_HEAD_SIZE (port_s3d_on ? (long)sizeof big_head : 0x100)
#define HZ_NODES (port_s3d_on ? big_nodes : hz_nodes)
/* "Object distance" (port/video/screen.c): the moving objects collected out to the land's end, not
 * the game's 120 squares */
#define HZ_OBJ_RING (port_s3d_on && (port_obj_dist || port_enemy_dist) ? 0x1FF : 0x78)
#else
#define HZ_HEAD hz_head
#define HZ_HEAD_SIZE 0x100
#define HZ_NODES hz_nodes
#define HZ_OBJ_RING 0x78
#endif

#ifndef TN_WATCOM_ABI
#include "mapedge.h"
/* The sharp view's far shores.  A span is the side of the land between two points of a ring,
 * filled with one square's texture; where a water or shore square (a texture with water
 * colours) meets the land one of its points has the land's height, so the game fills the slope
 * up to the land with that texture: water running up the far hills in sloped patches (in the
 * original too; the reflection of "Water reflection: Medium / High" shows the sky there).  In
 * the sharp view such a span is filled in two parts: its own texture up to the line of its
 * points at the water's height, then the land's (the square of the higher point, or of the
 * other, if it has no water colours) up to the real side.  The silhouette ends the same, so
 * what the view sees of objects doesn't change. */
static uchar hz_wtex[0x40];             /* the texture has water colours (water, shore) */
static fix hz_pvy, hz_pvz, hz_lvy, hz_lvz;      /* a point's view y / z without its height */
static long hz_ph, hz_lh;               /* its height (map cells: 1/24 square) */
static uchar *hz_pcell, *hz_lcell;      /* its cell: p_ the point just projected, l_ the last */

static void hz_water_tex(void)
{
    long t, i, n;
    for (t = 0; t < 0x40; t++) {
        uchar *row = t < planet_tex_count ? planet_tex_rows[t] : NULL;     /* a table that long */
        for (n = i = 0; row && i < 0x100; i++)
            n += sky_water[planet_light[(8 << 8) + row[i]]];
        hz_wtex[t] = n > 0;
    }
}

/* the screen y of a point (view y / z without its height) at height h, as thunder_2B2930
 * projects it; 0 when it is too close */
static int hz_proj_y(fix vy, fix vz, long h, fix *sy)
{
    fix d = hz_mat[7] * h + vz, sx = hz_sx, y = hz_sy, v = hz_vy;
    if (d < 0x10000)
        return 0;
    hz_vy = vy;
    thunder_2B2930(h, 0, d, h);
    *sy = hz_sy;
    hz_sx = sx;
    hz_sy = y;
    hz_vy = v;
    return 1;
}

static int hz_is_water(uchar *c)
{
    return !(*(long *)c & 0x40 && !hz_coarse) && hz_wtex[*c & 0x3F];
}

#ifndef TN_WATCOM_ABI
/* the GPU ground's far land (port/video/gpufar.c): the cell is water, as the caster has it */
int port_hz_water(uchar *c)
{
    return hz_wtex[*c & 0x3F] && !(*c & 0x40);
}
#endif

/* "Fog" (port/video/fog.c): this drawing is fogged; the fog of the last point and of the point just
 * projected (0..0x1000), the last ring drawn */
static int hz_fog;
static long hz_lf, hz_pf, hz_fog_end;
static long hz_pmx, hz_pmy;             /* the point just projected, on the map (haze_2B4198) */

static long hz_fog_point(void)
{
    return port_fog_at((hz_pmx << 16) - hz_view.x, (hz_pmy << 16) - hz_view.y);
}

/* the fog of the columns of the span to col (colras.c draws them): from the last point's to
 * this one's */
static void hz_fog_cols(long col)
{
    long c0 = hz_col, x0 = c0 < 0 ? 0 : c0, x1 = col > hz_ncols ? hz_ncols : col, x, f, df;
    if (col <= c0)
        return;
    if (x1 > 4096)
        x1 = 4096;
    df = ((hz_pf - hz_lf) << 8) / (col - c0);
    f = (hz_lf << 8) + (x0 - c0) * df + df / 2;
    for (x = x0; x < x1; x++, f += df)
        port_fog_f[x] = (ushort)(f >> 8);
}

/* the span to col in the texture of cell cc, split at the water's height when cc is water and
 * a point of the side is above it */
static void hz_span(long col, uchar *cc, long sh)
{
    long hw;
    uchar *land = NULL;
    fix y0, y1, ly = hz_lasty, sy = hz_sy;
    if (hz_fog)
        hz_fog_cols(col);
    if (port_s3d_on && hz_lcell && hz_is_water(cc)) {
        hw = MAP_CELL_H(cc);
        if (hz_lh < hw)
            hw = hz_lh;
        if (hz_ph < hw)
            hw = hz_ph;
        if (hz_lh > hw || hz_ph > hw) {
            /* the land: the higher point's square, else the other one's */
            land = hz_ph > hz_lh ? hz_pcell : hz_lcell;
            if (hz_is_water(land))
                land = hz_ph > hz_lh ? hz_lcell : hz_pcell;
            if (hz_is_water(land))
                land = NULL;
            if (land && (hz_lh <= hw ? (y0 = ly, 1) : hz_proj_y(hz_lvy, hz_lvz, hw, &y0)) &&
                (hz_ph <= hw ? (y1 = sy, 1) : hz_proj_y(hz_pvy, hz_pvz, hw, &y1))) {
                /* screen y grows downwards: the water line is never above the side */
                hz_lasty = y0 > ly ? y0 : ly;
                hz_sy = y1 > sy ? y1 : sy;
                haze_2B3E9C(col, *(long *)cc, sh);
                hz_lasty = ly;
                hz_sy = sy;
                haze_2B3E9C(col, *(long *)land, sh);
                return;
            }
        }
    }
    haze_2B3E9C(col, *(long *)cc, sh);
}

/* "Terrain smoothing" (port/video/screen.c): the far land textured like the near ground, in the sharp
 * view.  The game fills the side of the land between two rings with one square's texture row (a
 * pixel a texel, running on down the column, not tied to the land: noise at the sharp view's
 * size), one light level and the ring's haze level, so the far hills show as flat noisy blocks
 * with edges along the squares and the rings (stairs).  Here every point of a ring keeps its
 * place on the map, light and haze, per column for the ring's line; a pixel of the side between
 * the ring's line and the last ring's line gets its place on the map between the four points
 * around it, the texel there of its square's texture (planet_tex, 64 x 64 a square, turned by
 * the square's variant as weather_2B12D0 does for the near ground) from the copy whose texels
 * are about a pixel across (hz_mip: 32 x 32 .. 1 x 1, error-diffused), its light between the
 * points (an 8 x 8 ordered dither between levels) and haze (the nearest level).  The outline, the
 * column buffer and so what the view sees of objects stay the game's.  A water point is a water
 * texture's point at its square's lowest height; where the points say land, a square with water
 * colours (a shore square up a slope, G18) takes the land point's texture.  The pixels are drawn
 * on the drawing threads while the walk goes on (hz_runs_part). */
int hz_smooth;                          /* this drawing is smooth (haze_2B5484) */

typedef struct {
    long l;                             /* light level << 8 */
    long f;                             /* fog (hz_fog) */
    uchar c, w;                         /* typical colour (off the textures), water */
    fix mx, my;                         /* where it is on the map */
    uchar t;                            /* its texture (a land point by water: the land's) */
    long h;                             /* its height (map cells) */
} HzVert;

typedef struct {
    fix y;                              /* the ring's line in this column (not clipped) */
    long l, h, f;                       /* light, haze level << 8, fog */
    uchar wa, wb;                       /* the two points' water */
    fix mx, my;                         /* where the line is on the map here */
    uchar ta, tb;                       /* the two points' textures */
    long ha, hb, hg;                    /* the two points' heights; the line's here << 8 */
} HzLine;

/* a ring's line from one point to the next, as hz_smooth_record found it: its HzLine in a column
 * worked out when a run needs it (hz_line_at; the rings' lines cover many more columns than the
 * runs read, about 286,000 a drawing at 6x in 4K, and were written column by column in the walk:
 * speed, the same values) */
typedef struct {
    long c0, ds16;                      /* its left column, the right point's share a column << 16 */
    fix ly, dv;                         /* the line's row at c0, and a column more */
    long h;                             /* hz_hf */
    HzVert l, p;                        /* its left and right points */
} HzSeg;

/* a column's line: which ring's (key: hz_gen << 12 | the ring) and which piece of it */
typedef struct {
    long key, seg;
} HzLink;

static uchar hz_mip[0x40][1365];         /* per texture 32 x 32, 16 x 16 .. 1 x 1 */
static ulong hz_mip_key;
static const ushort hz_mip_at[7] = { 0, 0, 1024, 1280, 1344, 1360, 1364 };

#define HZ_COLS 4096                    /* sharp3d.c MAX_COLS */
#define HZ_SEGS 0x8000
static HzLink hz_line[HZ_COLS], hz_older[HZ_COLS];
static HzSeg hz_segs[HZ_SEGS];
static long hz_nseg;                    /* (from 0 each drawing) */
static long hz_gen, hz_hf;              /* this drawing; the ring's haze level << 8 */
static long hz_r0;                      /* the first ring drawn smooth everywhere */
static int hz_dpos;                     /* the rings move towards + on the main axis */
static HzVert hz_lv, hz_pv;             /* the last point, the point just projected */
extern ushort port_poly_have[PF_W][PF_W], port_poly_gen;       /* terrstrp.c ([y + PF_M][x + PF_M]) */
/* per texture its typical texel: of all its texels, of its water ones (a shore's water), of its
 * others (a shore's land); the same of the textures with blast marks, made when needed */
static uchar hz_med[3][0x40], hz_bmed[0x40], hz_bmed_ok[0x40];
static ulong hz_med_key;

static const uchar hz_bayer[8][8] = {
    {  0, 32,  8, 40,  2, 34, 10, 42 }, { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44,  4, 36, 14, 46,  6, 38 }, { 60, 28, 52, 20, 62, 30, 54, 22 },
    {  3, 35, 11, 43,  1, 33,  9, 41 }, { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47,  7, 39, 13, 45,  5, 37 }, { 63, 31, 55, 23, 61, 29, 53, 21 },
};

/* hz_bayer four ways (bytes 0 and 3 for the light and the fog), per column and row, x 4 + 2 (in
 * 0x100ths) */
static ulong hz_dd[8][8];                /* the four in bytes 0, 1, 2, 3 */

static void hz_dither_tables(void)
{
    long x, r;
    for (x = 0; x < 8; x++)
        for (r = 0; r < 8; r++)
            hz_dd[x][r] = (hz_bayer[r][x] * 4 + 2) | (ulong)(hz_bayer[(x + 3) & 7][(r + 5) & 7] * 4 + 2) << 8 |
                          (ulong)(hz_bayer[(r + 2) & 7][(x + 6) & 7] * 4 + 2) << 16 |
                          (ulong)(hz_bayer[(r + 4) & 7][(x + 2) & 7] * 4 + 2) << 24;
}

static void hz_rgb(uchar c, long *r, long *g, long *b)
{
    grs_rgb w = grd_bpal[c];
    *r = (long)(w & 0x3FF) >> 2;
    *g = (long)(w >> 13) & 0xFF;
    *b = (long)(w >> 24) & 0xFF;
}

/* the texel of a texture row nearest the average colour (at the middle light level) of its
 * texels of a kind: 0 all, 1 water, 2 the others; all when it has none of that kind */
static uchar hz_medoid(const uchar *row, int kind)
{
    long sr = 0, sg = 0, sb = 0, n = 0, i, r, g, b, best = 0x7FFFFFFF;
    uchar pick = row[0];
    for (i = 0; i < 0x100; i++) {
        uchar c = planet_light[(8 << 8) + row[i]];
        if (kind && sky_water[c] != (kind == 1))
            continue;
        hz_rgb(c, &r, &g, &b);
        sr += r;
        sg += g;
        sb += b;
        n++;
    }
    if (!n)
        return kind ? hz_medoid(row, 0) : pick;
    sr /= n;
    sg /= n;
    sb /= n;
    for (i = 0; i < 0x100; i++) {
        uchar c = planet_light[(8 << 8) + row[i]];
        long d;
        if (kind && sky_water[c] != (kind == 1))
            continue;
        hz_rgb(c, &r, &g, &b);
        d = (r - sr) * (r - sr) * 3 + (g - sg) * (g - sg) * 4 + (b - sb) * (b - sb) * 2;
        if (d < best)
            best = d, pick = row[i];
    }
    return pick;
}

/* the smaller copies of texture t: each texel the one of the texture's colours nearest the
 * average of the four it stands for (at the middle light level, as hz_medoid) */
static void hz_mip_make(long t)
{
    static long rgb[0x1000][3], err[2][34][3];
    uchar cols[0x100], seen[0x100];
    long crgb[0x100][3];
    long ncol = 0, i, k, size;
    const uchar *src = planet_tex[t];
    uchar *dst;
    memset(seen, 0, sizeof seen);
    for (i = 0; i < 0x1000; i++)
        if (!seen[src[i]]) {
            seen[src[i]] = 1;
            hz_rgb(planet_light[(8 << 8) + src[i]], &crgb[ncol][0], &crgb[ncol][1], &crgb[ncol][2]);
            cols[ncol++] = src[i];
        }
    for (i = 0; i < 0x1000; i++)
        hz_rgb(planet_light[(8 << 8) + src[i]], &rgb[i][0], &rgb[i][1], &rgb[i][2]);
    for (k = 1, size = 32; k < 7; k++, size >>= 1) {
        long step = 64 / size, x, y, n = step * step;
        dst = hz_mip[t] + hz_mip_at[k];
        memset(err, 0, sizeof err);
        for (y = 0; y < size; y++) {
            long (*e0)[3] = err[y & 1], (*e1)[3] = err[(y + 1) & 1];
            memset(e1, 0, sizeof err[0]);
            for (x = 0; x < size; x++) {
                long s[3] = { 0, 0, 0 }, a, b, j, best = 0x7FFFFFFF, pick = 0, d;
                for (b = 0; b < step; b++)
                    for (a = 0; a < step; a++)
                        for (j = 0; j < 3; j++)
                            s[j] += rgb[(y * step + b) * 64 + x * step + a][j];
                for (j = 0; j < 3; j++)
                    s[j] = s[j] / n + e0[x + 1][j] / 16;
                for (i = 0; i < ncol; i++) {
                    d = (crgb[i][0] - s[0]) * (crgb[i][0] - s[0]) * 3 + (crgb[i][1] - s[1]) * (crgb[i][1] - s[1]) * 4 +
                        (crgb[i][2] - s[2]) * (crgb[i][2] - s[2]) * 2;
                    if (d < best)
                        best = d, pick = i;
                }
                dst[y * size + x] = cols[pick];
                for (j = 0; j < 3; j++) {       /* Floyd-Steinberg: the error to the next ones */
                    long q = s[j] - crgb[pick][j];
                    e0[x + 2][j] += q * 7;
                    e1[x][j] += q * 3;
                    e1[x + 1][j] += q * 5;
                    e1[x + 2][j] += q;
                }
            }
        }
    }
}

static void hz_smooth_frame(void)
{
    ulong key = 0;
    long t, i, k;
    for (t = 0; t < planet_tex_count; t++)
        key = key * 31 + (ulong)planet_tex_rows[t];
    for (i = 0; i < 0xFC; i += 7)       /* (not the cycling colours 0xFC..0xFE: port/video/fog.c) */
        key = key * 31 + grd_bpal[i] + planet_light[(8 << 8) + i];
    if (key != hz_med_key) {
        hz_med_key = key;
        for (k = 0; k < 3; k++)
            for (t = 0; t < 0x40; t++)
                hz_med[k][t] = t < planet_tex_count ? hz_medoid(planet_tex_rows[t], k) : 0;
    }
    if (key != hz_mip_key) {
        hz_mip_key = key;
        for (t = 0; t < planet_tex_count && t < 0x40; t++)
            hz_mip_make(t);
    }
    memset(hz_bmed_ok, 0, sizeof hz_bmed_ok);   /* the blast marks change */
    if (!hz_dd[0][0])
        hz_dither_tables();
    hz_gen = (hz_gen + 1) & 0xFFFFF;
    hz_nseg = 0;                        /* (the older drawings' links are never followed) */
    /* the smooth ground covers the rings nearer than the first column band: there a side is
     * drawn as the game does where its square has polygons (they are drawn over it), smooth
     * where it has none (the polygons' cone leaves out squares the view sees) */
    hz_r0 = hz_cam.band[hz_cam.band0].a - 2;
}

/* the haze level of a ring, << 8: hz_rad1's (whole levels) unrounded, half a level less (it is
 * rounded, the game's whole levels are cut) */
static long hz_haze(long ring)
{
    long h = (hviewdiv_2AB3CC(ring, *(long *)&hz_cam.g_20[0]) >> 4) - 0x80;
    return h < 0 ? 0 : h > 0xF00 ? 0xF00 : h;
}

static int hz_on_map(long x, long y)
{
    if (hz_coarse)
        return x >= hz_ca4 && x < hz_c9c && y >= hz_ca8 && y < hz_ca0;
    return PF_IN1(x, y);                /* the land around the map too (port/video/mapedge.c) */
}

/* a point's colour, light and water: (mx, my) on the map, its cell c */
static void hz_vert(HzVert *v, uchar *c, long mx, long my)
{
    static const signed char around[8][2] = {
        { 1, 0 }, { 0, 1 }, { 1, 1 }, { -1, 0 }, { 0, -1 }, { -1, -1 }, { 1, -1 }, { -1, 1 } };
    long t = *c & 0x3F, h = MAP_CELL_H(c), k, st = hz_step, kind = 0;
    v->mx = mx << 16;
    v->my = my << 16;
    v->h = h;
    v->l = (15 - SHADE(c)) << 8;
    v->f = hz_fog ? port_fog_at((mx << 16) - hz_view.x, (my << 16) - hz_view.y) : 0;
    v->w = 0;
    if (hz_is_water(c)) {
        /* water when no other corner of its square is lower */
        v->w = 1;
        kind = 1;
        for (k = 0; k < 3 && v->w; k++) {
            long x = mx + around[k][0] * st, y = my + around[k][1] * st;
            if (hz_on_map(x, y) && MAP_CELL_H(haze_2B3FD0(x, y)) < h)
                v->w = 0;
        }
        if (!v->w) {            /* up on the land: the highest land square around, else its land */
            long best = -0x10000;
            kind = 2;
            for (k = 0; k < 8; k++) {
                long x = mx + around[k][0] * st, y = my + around[k][1] * st;
                uchar *n;
                if (!hz_on_map(x, y))
                    continue;
                n = haze_2B3FD0(x, y);
                if (!hz_is_water(n) && MAP_CELL_H(n) > best) {
                    best = MAP_CELL_H(n);
                    c = n;
                    t = *n & 0x3F;
                    kind = 0;
                }
            }
        }
    }
    v->t = (uchar)t;
    if (!hz_coarse && (*c & 0x40) && t < planet_tex_count) {    /* blast marks (strip_2AC128; never water) */
        if (!hz_bmed_ok[t]) {
            hz_bmed[t] = hz_medoid(strip_2AC128(t), 0);
            hz_bmed_ok[t] = 1;
        }
        v->c = hz_bmed[t];
    } else
        v->c = hz_med[kind][t];
}

/* A column's part of a smooth span as it is drawn (hz_run_make, on the drawing threads): the
 * rows yt .. top - 1 of column x; on top (this ring's line) the map place tx / ty, light lt and
 * haze ht, plus dx / dy, dl / dh at the bottom (the last ring's line); the bottom's share tw16
 * (<< 16) at row yt and dtw16 more a row.  Water: the last point's wl and this one's wp on
 * top, wa / wb at the bottom.  Each pixel belongs to one run (the column buffer), so the runs can
 * be drawn in any order. */
typedef struct {
    short x, yt, top;
    uchar lc, mixed, wl, wp, wa, wb;    /* lc: the colour off the textures */
    long lt, dl, ht, dh, tw16, dtw16;
    long ft, df;                        /* fog on top, more at the bottom (hz_fog) */
    fix tx, ty, dx, dy;
    long lvl;                           /* the texture's copy (0 the texture) */
    long land;                          /* a land point's texture, or -1 */
    long hg, dhg, hw;                   /* the land's height on top, more at the bottom, the
                                         * water's height (<< 8) */
} HzRun;

/* what the walk keeps of a span (hz_smooth_span) for its columns' runs */
typedef struct {
    HzVert l, p;                        /* the last point and this one (hz_lv, hz_pv) */
    long hf, lvl;                       /* hz_hf; the texture's copy */
    fix v0, dv;                         /* this ring's line at column x0, and a column more */
    long x0, st0, dst16;                /* the second point's share at x0, a column more (<< 16) */
} HzSpan;

/* ... and of a column of it: the run's rows, its span, the last ring's line (a piece, or -1) */
typedef struct {
    short x, yt, top;
    long span, seg;
} HzCol;

#define HZ_RUNS 0x20000
#define HZ_SPANS 0x4000
static HzCol hz_runs[HZ_RUNS];
static HzSpan hz_spans[HZ_SPANS];
static long hz_nruns, hz_nspans;

/* what the runs read that stays the same through a drawing (hz_runs_begin), in cache lines of its
 * own: the walk writes the game's variables beside hz_ca4 .. hz_c9c and strip_bits all the time,
 * and the drawing threads reading them there while the walk went on took longer than the pixels */
static _Alignas(64) struct {
    ulong dd[8][8];                     /* hz_dd */
    uchar wtex[0x40];                   /* hz_wtex */
    GameMap *m;
    long ca4, c9c, ca8, ca0, ntex;      /* the coarse map's limits, planet_tex_count */
    uchar *bits;                        /* strip_bits, strip_rows */
    const long *rows;
    const uchar *shade;                 /* planet_shade */
    int fog, tc;                        /* hz_fog, port_tc_on */
    uchar *tc_base, *tc_row;            /* port_tc_base, port_tc_row, port_tc_info, port_tc_n */
    ulong *tc_info, tc_n;
    char pad[64];
} hz_k;

/* a square's texture as the runs find it (hz_sq) */
typedef struct {
    const uchar *base;
    long ax, ay;
    int ok;
} HzSq;

/* per copy (64 >> lvl texels a side) and variant: where the texel (0, 0) is in the copy and the
 * steps of the texel's place for fx and fy, turned by the square's variant as weather_2B12D0
 * does (a table: the variant changes square by square, a guess the processor gets wrong half
 * the time) */
#define HZ_TURN(s) { { 0, 1, s }, { ((s) - 1) * (s), -(s), 1 }, { ((s) - 1) * (s) + (s) - 1, -1, -(s) }, \
                     { (s) - 1, s, -1 } }
static const long hz_turn[7][4][3] = {
    HZ_TURN(64), HZ_TURN(32), HZ_TURN(16), HZ_TURN(8), HZ_TURN(4), HZ_TURN(2), HZ_TURN(1) };
#undef HZ_TURN

/* the texture of the square (sx, sy) at the copy lvl (*tex >= 0: that texture): the texel of
 * (fx, fy) at q->base[fx * q->ax + fy * q->ay]; q->ok 0 off the map */
static inline __attribute__((always_inline)) void hz_sq(HzSq *q, long sx, long sy, long lvl, long *tex)
{
    GameMap *m = hz_k.m;
    long t, var = 0;
    const uchar *c;
    const long *tn;
    q->ok = 0;
    if (PF_IN(sx, sy)) {                /* the map and the land around it (port/video/mapedge.c) */
        c = PF_CELL(sx, sy);
        var = (*(ushort *)(c + 1) >> 2) & 3;
    } else if (sx >= hz_k.ca4 && sx < hz_k.c9c && sy >= hz_k.ca8 && sy < hz_k.ca0)
        c = (uchar *)TN_LOWMEM(m->rows1[(sy >> 2) + m->y0]) + ((sx >> 2) + m->x0) * 3;
    else
        return;
    t = *tex >= 0 ? *tex : *c & 0x3F;
    if (t >= hz_k.ntex)
        return;
    *tex = t;
    tn = hz_turn[lvl][var];
    q->base = (lvl ? hz_mip[t] + hz_mip_at[lvl] : planet_tex[t]) + tn[0];
    q->ax = tn[1];
    q->ay = tn[2];
    q->ok = 1;
}

/* a run: each pixel's place on the map between the run's top and bottom, its square's texel, the
 * light dithered between levels, the haze rounded, the fog dithered.  One copy for each case
 * (fog, True colour, a shore) with what it reads in locals, a shore square's two textures looked
 * up once a square (speed step B): the same pixels */
static inline __attribute__((always_inline)) void hz_run_body(const HzRun *u, const int FOG, const int TC,
                                                              const int SHORE)
{
    long r = u->yt, top = u->top, tw16 = u->tw16, dtw = u->dtw16;
    long lvl = u->lvl, sh = 10 + lvl, csx = 0x7FFFFFFF, csy = 0, land = u->land;
    /* the place, light, haze and fog at the first row, and a row down (while above the last
     * ring's line) */
    fix x = u->tx + (fix)(((long long)u->dx * tw16) >> 24), y = u->ty + (fix)(((long long)u->dy * tw16) >> 24);
    fix ddx = (fix)(((long long)u->dx * dtw) >> 24), ddy = (fix)(((long long)u->dy * dtw) >> 24);
    long L = u->lt + (long)(((long long)u->dl * tw16) >> 24), dL = (long)(((long long)u->dl * dtw) >> 24);
    long H = u->ht + 0x80 + (long)(((long long)u->dh * tw16) >> 24), dH = (long)(((long long)u->dh * dtw) >> 24);
    long F = 0, dF = 0, G = 0, dG = 0, hw = 0;
    int mixed = u->mixed, wl = u->wl, wtex = 0, land_ok = 0;
    const ulong *dd = hz_k.dd[u->x & 7];
    const uchar *shade = hz_k.shade, *wt = hz_k.wtex;
    uchar *bits = hz_k.bits + u->x, *tcb = hz_k.tc_base, *tcr = hz_k.tc_row, lc = u->lc;
    const long *rows = hz_k.rows;
    ulong *tci = hz_k.tc_info, tcn = hz_k.tc_n;
    HzSq q = { NULL, 0, 0, 0 }, ql = { NULL, 0, 0, 0 };
    if (FOG) {
        F = u->ft + (long)(((long long)u->df * tw16) >> 24);
        dF = (long)(((long long)u->df * dtw) >> 24);
    }
    if (SHORE) {
        G = u->hg + (long)(((long long)u->dhg * tw16) >> 24);
        dG = (long)(((long long)u->dhg * dtw) >> 24);
        hw = u->hw;
    }
    for (; r < top; r++) {
        long li, hi, sx = x >> 16, sy = y >> 16;
        ulong d = dd[r & 7];
        const HzSq *p = &q;
        uchar c;
        if (sx != csx || sy != csy) {
            long t = -1;
            csx = sx;
            csy = sy;
            hz_sq(&q, sx, sy, lvl, &t);
            /* a water texture where the points around say land (a shore square up a slope:
             * G18): the land's texture */
            wtex = SHORE && q.ok && wt[t];
            land_ok = 0;
        }
        /* where water meets land: water up to the water's height (the land's height between
         * the four points), so the shore runs where the land leaves the water */
        if (SHORE && wtex && !(mixed ? G <= hw : wl)) {
            if (!land_ok) {
                long t = land;
                hz_sq(&ql, sx, sy, lvl, &t);
                land_ok = 1;
            }
            p = &ql;
        }
        c = p->ok ? p->base[((x & 0xFFFF) >> sh) * p->ax + ((y & 0xFFFF) >> sh) * p->ay] : lc;
        li = (L + (long)(d & 0xFF)) >> 8;
        hi = H >> 8;
        li = li > 23 ? 23 : li < 0 ? 0 : li;
        hi = hi > 15 ? 15 : hi < 0 ? 0 : hi;
        c = shade[(hi << 8) + planet_light[(li << 8) + c]];
        if (FOG) {
            long fi = (F + (long)(d >> 24)) >> 8, a = F >> 4;
            uchar c0 = c;
            c = port_fog_tab[fi > 16 ? 16 : fi < 0 ? 0 : fi][c];
            bits[rows[r]] = c;
            if (TC && (ulong)r < 2048) {        /* "True colour" (port/video/truecol.c): port_tc_mark */
                ulong i = (ulong)(bits + rows[r] - tcb);
                if (i < tcn)
                    tci[i] = (ulong)PORT_TC_FOG << 24 | (ulong)(a < 0 ? 0 : a > 255 ? 255 : a) << 16 | (ulong)c0 << 8 | c;
                if (!tcr[r])                    /* (written once: the lines stay shared) */
                    tcr[r] = 1;
            }
        } else
            bits[rows[r]] = c;
        GL_NOTE_FAR(bits + rows[r]);
        if (tw16 < 0x1000000) {         /* (the values may go a row past the bottom's: kept in
                                         * their ranges above) */
            tw16 += dtw;
            x += ddx;
            y += ddy;
            L += dL;
            H += dH;
            if (FOG)
                F += dF;
            if (SHORE)
                G += dG;
        }
    }
}

/* the line of the piece seg in column x (as hz_smooth_record once wrote it column by column) */
static inline __attribute__((always_inline)) void hz_line_at(long seg, long x, HzLine *r)
{
    const HzSeg *g = &hz_segs[seg];
    const HzVert *l = &g->l, *p = &g->p;
    long s = ((x - g->c0) * g->ds16 + g->ds16 / 2) >> 16;
    r->y = g->ly + (x - g->c0) * g->dv;
    r->l = l->l + ((p->l - l->l) * s >> 8);
    r->h = g->h;
    r->f = l->f + ((p->f - l->f) * s >> 8);
    r->wa = l->w;
    r->wb = p->w;
    r->ha = l->h;
    r->hb = p->h;
    r->hg = (l->h << 8) + (p->h - l->h) * s;
    r->mx = l->mx + ((p->mx - l->mx) >> 8) * s;
    r->my = l->my + ((p->my - l->my) >> 8) * s;
    r->ta = l->t;
    r->tb = p->t;
}

/* the run of the column c (as hz_smooth_span once worked it out in the walk) */
static inline __attribute__((always_inline)) void hz_run_make(const HzCol *c, HzRun *u)
{
    const HzSpan *sp = &hz_spans[c->span];
    const HzVert *l = &sp->l, *p = &sp->p;
    long n = c->x - sp->x0, yt = c->yt, st = (sp->st0 + n * sp->dst16) >> 16;
    fix vx = sp->v0 + n * sp->dv, den;
    const HzLine *B = NULL;
    HzLine line;
    if (st > 0x100)
        st = 0x100;
    u->x = c->x;
    u->yt = c->yt;
    u->top = c->top;
    u->lc = l->c;
    u->wl = l->w;
    u->wp = p->w;
    u->lt = l->l + ((p->l - l->l) * st >> 8);
    u->ht = sp->hf;
    u->ft = l->f + ((p->f - l->f) * st >> 8);
    u->tx = l->mx + ((p->mx - l->mx) >> 8) * st;
    u->ty = l->my + ((p->my - l->my) >> 8) * st;
    if (c->seg >= 0) {
        hz_line_at(c->seg, c->x, &line);
        B = &line;
    }
    u->hg = (l->h << 8) + (p->h - l->h) * st;
    if (!B) {                           /* the first ring: its own line all down */
        u->wa = u->wl;
        u->wb = u->wp;
        u->dl = u->dh = u->df = 0;
        u->tw16 = u->dtw16 = 0;
        u->dx = u->dy = 0;
        u->dhg = 0;
    } else {
        u->wa = B->wa;
        u->wb = B->wb;
        u->dl = B->l - u->lt;
        u->dh = B->h - sp->hf;
        u->df = B->f - u->ft;
        u->dx = B->mx - u->tx;
        u->dy = B->my - u->ty;
        u->dhg = B->hg - u->hg;
        /* the bottom's share: 0 on this ring's line, 1 on the last one's */
        den = B->y - vx;
        if (den <= 0)
            u->tw16 = u->dtw16 = 0;
        else {
            long long t;
            if (den < 0x1000)
                den = 0x1000;
            u->dtw16 = (long)(0xFFFFFFFFul / (ulong)(den >> 8));     /* 2^40 / den */
            t = ((long long)((yt << 16) + 0x8000 - vx) * u->dtw16) >> 16;
            u->tw16 = t < 0 ? 0 : t > 0x1000000 ? 0x1000000 : (long)t;
        }
    }
    u->mixed = u->wl != u->wp || u->wa != u->wb || u->wa != u->wl;
    if (u->mixed) {                     /* the water's height: its lowest water point's */
        long hw = 0x7FFFFFFF;
        if (u->wl)
            hw = l->h;
        if (u->wp && p->h < hw)
            hw = p->h;
        if (B && u->wa && B->ha < hw)
            hw = B->ha;
        if (B && u->wb && B->hb < hw)
            hw = B->hb;
        u->hw = (hw << 8) + 0x80;
    }
    u->land = !u->wl ? l->t : !u->wp ? p->t : B && !u->wa ? B->ta : B && !u->wb ? B->tb : -1;
    u->lvl = sp->lvl;
}

static void hz_run_draw(const HzCol *c)
{
    HzRun r;
    int shore;
    hz_run_make(c, &r);
    shore = r.land >= 0;
    if (!hz_k.fog)
        shore ? hz_run_body(&r, 0, 0, 1) : hz_run_body(&r, 0, 0, 0);
    else if (!hz_k.tc)
        shore ? hz_run_body(&r, 1, 0, 1) : hz_run_body(&r, 1, 0, 0);
    else
        shore ? hz_run_body(&r, 1, 1, 1) : hz_run_body(&r, 1, 1, 0);
}

/* The runs are drawn on the drawing threads in parts of HZ_CHUNK while the walk goes on
 * (port_stream, speed step C): a part as soon as the walk has made it, the rest at the end of the
 * drawing or when the list is full.  What a run draws comes from the run and from tables that
 * stay the same through the drawing, its pixels are its own (the column buffer: what the game's
 * spans and colras.c's columns draw later is above them), so the picture is the one drawing them
 * all at the end gives. */
#define HZ_CHUNK 1024

static void hz_runs_part(void *arg, int part, int thread)
{
    long i = part * HZ_CHUNK, e = i + HZ_CHUNK;
    (void)arg;
    (void)thread;
    for (; i < e; i++)
        hz_run_draw(&hz_runs[i]);
}

static void hz_runs_begin(void)
{
    memcpy(hz_k.dd, hz_dd, sizeof hz_k.dd);
    memcpy(hz_k.wtex, hz_wtex, sizeof hz_k.wtex);
    hz_k.m = GMAP;
    hz_k.ca4 = hz_ca4;
    hz_k.c9c = hz_c9c;
    hz_k.ca8 = hz_ca8;
    hz_k.ca0 = hz_ca0;
    hz_k.ntex = planet_tex_count;
    hz_k.bits = strip_bits;
    hz_k.rows = strip_rows;
    hz_k.shade = planet_shade;
    hz_k.fog = hz_fog;
    hz_k.tc = port_tc_on;
    hz_k.tc_base = port_tc_base;
    hz_k.tc_row = port_tc_row;
    hz_k.tc_info = port_tc_info;
    hz_k.tc_n = port_tc_n;
    hz_nruns = 0;
    port_stream_begin(hz_runs_part, NULL);
}

/* the runs kept so far all drawn (at the end of the drawing, or when the list is full): the
 * last part's on the game thread */
static void hz_runs_draw(void)
{
    long i;
    for (i = hz_nruns / HZ_CHUNK * HZ_CHUNK; i < hz_nruns; i++)
        hz_run_draw(&hz_runs[i]);
    port_stream_end((int)(hz_nruns / HZ_CHUNK));
    hz_nruns = 0;
    hz_nspans = 0;
}

/* the span to col as haze_2B3E9C draws it, smooth: its runs kept */
static void hz_smooth_span(long col, long sh)
{
    long c0 = hz_col, x0, x1, x, n, span, cnt, lvl = 0;
    fix y = hz_sy, ly, dv, v0;
    long *yb = hview_cols;
    if (!(col > 0 && c0 < hz_ncols && c0 < col))
        return;
    if (y > 0x3F000000)
        y = 0x3F000000;
    else if (y < (fix)0xC1000000)
        y = (fix)0xC1000000;
    if (hz_lasty > 0x3F000000)
        hz_lasty = 0x3F000000;
    else if (hz_lasty < (fix)0xC1000000)
        hz_lasty = (fix)0xC1000000;
    ly = hz_lasty;
    span = col - c0;
    dv = (y - ly) / span;
    x1 = col > hz_ncols ? hz_ncols : col;
    x0 = c0 < 0 ? 0 : c0;
    cnt = x1 - x0;
    v0 = ly + (x0 - c0) * dv;
    strip_shade = 15 - sh;
    /* strip_2AC19C draws a run as wide as the view only when its top leaves the view */
    if (!(v0 < 0 || ly + (x1 - c0) * dv < 0 || cnt < port_s3d_cols))
        return;
    {                                   /* the texture copy whose texels are about a pixel
                                         * across (a row down they are more: seen aslant) */
        long fp = ((iabs(hz_pv.mx - hz_lv.mx) + iabs(hz_pv.my - hz_lv.my)) / span) << 6;
        while (lvl < 6 && fp > (0x18000 << lvl))
            lvl++;
    }
    if (hz_nruns + cnt > HZ_RUNS || hz_nspans == HZ_SPANS) {
        hz_runs_draw();
        hz_runs_begin();
    }
    {
        HzSpan *g = &hz_spans[hz_nspans];
        g->l = hz_lv;
        g->p = hz_pv;
        g->hf = hz_hf;
        g->lvl = lvl;
        g->v0 = v0;
        g->dv = dv;
        g->x0 = x0;
        g->dst16 = 0x1000000 / span;    /* the second point's share, << 16 */
        g->st0 = (x0 - c0) * g->dst16 + g->dst16 / 2;
    }
    for (x = x0, n = 0; x < x1; x++, n++) {
        fix vx = v0 + n * dv;
        long yt = vx < 0 ? 0 : vx >> 16, top = yb[x];
        const HzLink *k;
        HzCol *u;
        if (yt >= top)
            continue;
        u = &hz_runs[hz_nruns++];
        u->x = (short)x;
        u->yt = (short)yt;
        u->top = (short)top;
        u->span = hz_nspans;
        k = &hz_line[x];
        if ((k->key >> 12) != hz_gen || k->key == ((hz_gen << 12) | hz_ring))
            k = (hz_older[x].key >> 12) == hz_gen ? &hz_older[x] : NULL;
        u->seg = k ? k->seg : -1;
        yb[x] = yt;
        if (!(hz_nruns % HZ_CHUNK))
            port_stream_ready((int)(hz_nruns / HZ_CHUNK));
    }
    hz_nspans++;
}

/* the line pieces the columns' links still name packed to the front (the list is full) */
static void hz_segs_pack(void)
{
    static long to[HZ_SEGS];
    long i, n = 0;
    HzLink *k;
    memset(to, 0xFF, sizeof to);
    for (k = hz_line; k < hz_line + HZ_COLS; k++)
        if ((k->key >> 12) == hz_gen)
            to[k->seg] = 0;
    for (k = hz_older; k < hz_older + HZ_COLS; k++)
        if ((k->key >> 12) == hz_gen)
            to[k->seg] = 0;
    for (i = 0; i < hz_nseg; i++)       /* (in order: each moves down or stays) */
        if (!to[i]) {
            hz_segs[n] = hz_segs[i];
            to[i] = n++;
        }
    for (k = hz_line; k < hz_line + HZ_COLS; k++)
        if ((k->key >> 12) == hz_gen)
            k->seg = to[k->seg];
    for (k = hz_older; k < hz_older + HZ_COLS; k++)
        if ((k->key >> 12) == hz_gen)
            k->seg = to[k->seg];
    hz_nseg = n;
}

/* this ring's line from the last point to the one just projected (column col), kept per column
 * for the next ring's sides: a piece of line, and the columns' links to it */
static void hz_smooth_record(long col)
{
    long c0 = hz_col, a, b, x, span, key = (hz_gen << 12) | hz_ring, ds16, n;
    fix ly = hz_lasty, y = hz_sy, dv;
    const HzVert *l = &hz_lv, *p = &hz_pv;
    HzSeg *g;
    if (col == c0)
        return;
    if (col < c0) {
        long t = c0;
        fix f = ly;
        const HzVert *q = l;
        c0 = col;
        col = t;
        ly = y;
        y = f;
        l = p;
        p = q;
    }
    span = col - c0;
    dv = (y - ly) / span;
    a = c0 < 0 ? 0 : c0;
    b = col > hz_ncols ? hz_ncols : col;
    if (b > HZ_COLS)
        b = HZ_COLS;
    ds16 = 0x1000000 / span;
    if (a >= b)
        return;
    if (hz_nseg == HZ_SEGS) {           /* (the runs kept name pieces: drawn first) */
        hz_runs_draw();
        hz_runs_begin();
        hz_segs_pack();
    }
    n = hz_nseg++;
    g = &hz_segs[n];
    g->c0 = c0;
    g->ds16 = ds16;
    g->ly = ly;
    g->dv = dv;
    g->h = hz_hf;
    g->l = *l;
    g->p = *p;
    for (x = a; x < b; x++) {
        HzLink *r = &hz_line[x];
        if (r->key != key)
            hz_older[x] = *r;
        r->key = key;
        r->seg = n;
    }
}
/* a nearer ring's side from the last point to the one just projected: no polygon on (one of)
 * its squares, between the last ring's line and this one's */
static int hz_open(void)
{
    long a, b, m, i;
    if (hz_step != 1)
        return 1;
    if (hz_xmajor) {                    /* the rings are rows along x, one after another in y */
        m = (hz_pv.my >> 16) - hz_dpos;
        a = hz_lv.mx >> 16;
        b = hz_pv.mx >> 16;
    } else {
        m = (hz_pv.mx >> 16) - hz_dpos;
        a = hz_lv.my >> 16;
        b = hz_pv.my >> 16;
    }
    if (a > b) {
        i = a;
        a = b;
        b = i;
    }
    if (b == a)
        b++;
    if (!PF_IN(m, a) || !PF_IN(m, b) || !PF_IN(a, m) || !PF_IN(b, m))
        return 1;
    for (i = a; i < b; i++)
        if (hz_xmajor ? port_poly_have[m + PF_M][i + PF_M] != port_poly_gen :
                        port_poly_have[i + PF_M][m + PF_M] != port_poly_gen)
            return 1;
    return 0;
}
#define HZ_SPAN(col, cc, sh) \
    (hz_smooth && (hz_ring >= hz_r0 || hz_open()) ? hz_smooth_span(col, sh) : hz_span(col, cc, sh))
#else
#define HZ_SPAN(col, cc, sh) haze_2B3E9C(col, *(long *)(cc), sh)
#endif

void haze_2B3A54(void)
{
    long i;
    for (i = 0; i < 0x3F; i++)
        hz_nodes[i].next = i + 1;
    hz_nodes[i].next = 0;
}

#ifndef TN_WATCOM_ABI
static void big_nodes_init(void)
{
    long i;
    for (i = 0; i < (long)(sizeof big_nodes / sizeof big_nodes[0]) - 1; i++)
        big_nodes[i].next = (short)(i + 1);
    big_nodes[i].next = 0;
}
#endif

void haze_2B3A7C(void)
{
    memset(HZ_HEAD, 0, HZ_HEAD_SIZE);
}

void haze_2B3A94(short ent, long bucket)
{
    short n;
    ai_seen_bits[ent >> 3] |= 1 << (ent & 7);
    if (!viewdist_2A0E54(ENTP(ent)))
        return;
    n = HZ_NODES[0].next;
    if (!n)
        return;
    HZ_NODES[0].next = HZ_NODES[n].next;
    HZ_NODES[n].ent = ent;
    HZ_NODES[n].next = HZ_HEAD[bucket];
    HZ_HEAD[bucket] = n;
}

void haze_2B3B20(long bucket)
{
    short n, last;
    if (bucket >= hz_nheads)
        return;
    n = HZ_HEAD[bucket];
    if (!n)
        return;
    do {
        short ent = HZ_NODES[n].ent;
        ai_seen_bits[ent >> 3] &= ~(1 << (ent & 7));
        haze_2B399C(ent);
        last = n;
        n = HZ_NODES[n].next;
    } while (n);
    HZ_NODES[last].next = HZ_NODES[0].next;
    HZ_NODES[0].next = HZ_HEAD[bucket];
    HZ_HEAD[bucket] = 0;
}

/* the moving objects of grid line x (along y, hz_xmajor) or y: those within d of the ring are
 * drawn now, the others parked by their distance */
void haze_2B3BA4(long x, long y, long d)
{
    short r;
    haze_2B3B20(d);
    if (hz_xmajor) {
        if (y < 0 || y >= 0x200)
            return;
        for (r = grid_cells[1][y]; r; r = grid_cellrecs[1][r].next) {
            short ent = grid_cellrecs[1][r].ent;
            long k;
            if (ai_seen_bits[ent >> 3] & (1 << (ent & 7)))
                continue;
            if (ent == *(short *)hz_view.g_5a)
                continue;
            k = iabs((ENTP(ent)->x >> 16) - x);
            if (k <= d)
                haze_2B399C(ent);
            else
                haze_2B3A94(ent, k);
        }
    } else {
        r = grid_cells[0][x];
        if (x < 0 || x >= 0x200)
            return;
        for (; r; r = grid_cellrecs[0][r].next) {
            short ent = grid_cellrecs[0][r].ent;
            long k;
            if (ai_seen_bits[ent >> 3] & (1 << (ent & 7)))
                continue;
            if (ent == *(short *)hz_view.g_5a)
                continue;
            k = iabs((ENTP(ent)->y >> 16) - y);
            if (k <= d)
                haze_2B399C(ent);
            else
                haze_2B3A94(ent, k);
        }
    }
}

void haze_2B3D44(long x, long y)
{
    TerrBin *b = STRIP;
    short sx = b->x, sy = b->y;
    b->y = (short)y;
    b->x = (short)x;
    viewdist_2A13B8(b, 0);
    b = STRIP;
    b->x = sx;
    b->y = sy;
}

long haze_2B3D7C(long x, long y)
{
    short h = (short)((((x << 2) + y) & 0x1FF) + 0x200);
    if (grid_hash[h].sq == 0)
        return game_3C5E1C[x >> 2][y >> 2] != 0;
    hz_key.x = (short)x;
    hz_key.y = (short)y;
    grid_hash_cur = (short)((((x << 2) + y) & 0x1FF) + 0x200);
    if (grid_hash[grid_hash_cur].sq == 0)
        h = 0;
    else {
        GridSquare *q = &grid_squares[grid_hash[grid_hash_cur].sq];
        if (q->x == hz_key.x && q->y == hz_key.y)
            h = grid_hash_cur;
        else
            h = grid_295CFC(hz_key, 0, grid_hash_cur);
    }
    if (grid_hash[h].sq != 0)
        return 1;
    return game_3C5E1C[x >> 2][y >> 2] != 0;
}

/* a terrain span from column hz_col to col, the screen y going from hz_lasty to hz_sy */
void haze_2B3E9C(long col, long b, long shade)
{
    long c0 = hz_col;
    fix y = hz_sy;
    if (col > 0 && c0 < hz_ncols && c0 < col) {
        strip_neg = y < 0;
        if (y > 0x3F000000)
            y = 0x3F000000;
        else if (y < (fix)0xC1000000)
            y = (fix)0xC1000000;
        if (hz_lasty > 0x3F000000)
            hz_lasty = 0x3F000000;
        else if (hz_lasty < (fix)0xC1000000)
            hz_lasty = (fix)0xC1000000;
        strip_y0 = hz_lasty;
        strip_dy = (y - hz_lasty) / (col - c0);
        if (col > hz_ncols) {
            fix v = (hz_ncols - c0) * strip_dy + strip_y0;
            col = hz_ncols;
            strip_neg = v < 0;
        }
        if (hz_coarse)
            b &= ~0x40;
        strip_dst = hview_cols + c0;
        strip_shade = 15 - hz_shade;
        hz_col = c0;
        strip_2AC19C(c0, col, b, shade);
        c0 = hz_col;
    }
    hz_col = c0;
}

/* The cell at x, y.  Near the map's edge the walk asks for row -1, whose pointer (the entry
 * before the row table) is 0: the original then read DOS low memory (TN_LOWMEM). */
uchar *haze_2B3FD0(long x, long y)
{
    GameMap *m = GMAP;
    if (hz_coarse)
        return (uchar *)TN_LOWMEM(m->rows1[(y >> 2) + m->y0]) + ((x >> 2) + m->x0) * 3;
#ifndef TN_WATCOM_ABI
    /* G35: the near rings go on past the map's edge, on the land around it (port/video/mapedge.c) */
    if (PF_IN(x, y))
        return PF_CELL(x, y);
    return PF_CELL(x < -PF_M ? -PF_M : x > 0x200 + PF_M ? 0x200 + PF_M : x,
                   y < -PF_M ? -PF_M : y > 0x200 + PF_M ? 0x200 + PF_M : y);
#else
    return (uchar *)TN_LOWMEM(m->rows2[y]) + x * 3;
#endif
}

void haze_2B402C(void)
{
    hz_ca90 = hz_view.mz[0] - fmul(hz_view.y, hz_mat[0]) - fmul(hz_view.x, hz_mat[2]);
    hz_ca94 = hz_view.mz[1] - fmul(hz_view.y, hz_mat[3]) - fmul(hz_view.x, hz_mat[5]);
    hz_ca98 = hz_view.mz[2] - fmul(hz_view.y, hz_mat[6]) - fmul(hz_view.x, hz_mat[8]);
}

uchar haze_2B40C8(long x, long y)
{
    long h, a, d;
    if (hz_coarse) {
        if (x < hz_ca4 || x >= hz_c9c || y < hz_ca8 || y >= hz_ca0)
            return 0;
    }
#ifndef TN_WATCOM_ABI
    else if (!PF_IN1(x, y))             /* G35: the land around the map (port/video/mapedge.c) */
        return 0;
#else
    else if (x < 0 || y < 0 || x >= 0x200 || y >= 0x200)
        return 0;
#endif
    h = (short)*(ushort *)(haze_2B3FD0(x, y) + 1) >> 4;
    a = hz_mat[0] * y + hz_mat[1] * h + hz_mat[2] * x + hz_ca90;
    d = hz_mat[6] * y + hz_mat[7] * h + hz_mat[8] * x + hz_ca98;
    if (d <= 0x10)
        return 0;
    if (a < 0)
        return a > -d;
    return a < d;
}

uchar haze_2B4198(long x, long y, fix *sx, fix *sy)
{
    fix ey = (y << 16) - hz_view.y;
    fix ex = (x << 16) - hz_view.x;
    long h = (short)*(ushort *)(haze_2B3FD0(x, y) + 1) >> 4;
    fix a = hz_mat[1] * h + fmul(ey, hz_mat[0]) + fmul(ex, hz_mat[2]) + hz_view.mz[0];
    fix b = hz_mat[4] * h + fmul(ey, hz_mat[3]) + fmul(ex, hz_mat[5]) + hz_view.mz[1];
    fix d = fmul(ey, hz_mat[6]) + hz_mat[7] * h + fmul(ex, hz_mat[8]) + hz_view.mz[2];
    if (d < 0x10)
        return 0;
#ifndef TN_WATCOM_ABI
    hz_pvy = b - hz_mat[4] * h;
    hz_pvz = d - hz_mat[7] * h;
    hz_ph = h;
    hz_pcell = haze_2B3FD0(x, y);
    hz_pmx = x;
    hz_pmy = y;
#endif
    *sx = hz_cam.half_w * (fdiv(a, d) + 0x10000);
    *sy = (fdiv(b, d) + 0x10000) * hz_cam.half_h + 0x10000;
    return 1;
}

void haze_2B42C0(long x, long y, long dx, long dy)
{
    fix ex, ey;
    long k;
    hz_px = x;
    hz_py = y;
    hz_wx = dx;
    ex = (x << 16) - hz_view.x;
    ey = (y << 16) - hz_view.y;
    hz_wy = dy;
    hz_vx = fmul(ey, hz_mat[0]) + fmul(ex, hz_mat[2]) + hz_view.mz[0];
    hz_vy = fmul(ey, hz_mat[3]) + fmul(ex, hz_mat[5]) + hz_view.mz[1];
    hz_vz = fmul(ey, hz_mat[6]) + fmul(ex, hz_mat[8]) + hz_view.mz[2];
    hz_ca78 = hz_mat[0] * dy + hz_mat[2] * dx;
    hz_ca8c = hz_mat[3] * dy + hz_mat[5] * dx;
    hz_ca7c = hz_mat[6] * dy + hz_mat[8] * dx;
    hz_cell = haze_2B3FD0(x, y);
    k = dx + dy * hz_stride;
    hz_ca6c = k;
    if (hz_coarse)
        hz_ca6c = k >> 2;
    hz_cell -= hz_ca6c * 3;
}

uchar haze_2B4428(void)
{
    uchar *p = hz_cell + hz_ca6c * 3;
    long h = (short)*(ushort *)(p + 1) >> 4;
    fix d = hz_mat[7] * h + hz_vz;
    uchar r;
    hz_cell = p;
    if (d < 0x10000)
        r = 0;
    else {
        thunder_2B2930(h, (long)p, d, h);
#ifndef TN_WATCOM_ABI
        hz_pvy = hz_vy;
        hz_pvz = hz_vz;
        hz_ph = h;
        hz_pcell = p;
        hz_pmx = hz_px;
        hz_pmy = hz_py;
#endif
        STRIP->x = (short)hz_px;
        STRIP->y = (short)hz_py;
        r = 1;
    }
    hz_px += hz_wx;
    hz_py += hz_wy;
    hz_vx += hz_ca78;
    hz_vy += hz_ca8c;
    hz_vz += hz_ca7c;
    return r;
}

/* the tile objects of square (hz_px + dx, hz_py + dy), seen between columns c0 and c1 */
void haze_2B44FC(long c0, long c1, long dx, long dy)
{
    long x = dx + hz_px, y = dy + hz_py;
#ifndef TN_WATCOM_ABI
    if (!hz_coarse && ((ulong)x >= 0x200 || (ulong)y >= 0x200))
        return;                         /* the land around the map has no objects (G35) */
#endif
    if (c0 < 0)
        c0 = 0;
    if (c1 >= hz_ncols)
        c1 = hz_ncols - 1;
#ifndef TN_WATCOM_ABI
    /* G41: the square's columns are where the walk's last two points fell on the screen, and a
     * point beside the eye falls far off it (-29233 in a run): only the left one was kept at or
     * after the first column and the right one at or before the last, so the other read the
     * column table from far outside it */
    if (c0 >= hz_ncols)
        c0 = hz_ncols - 1;
    if (c1 < 0)
        c1 = 0;
#endif
    vd_center_col = (c0 + c1) >> 1;
    if (haze_2B3D7C(x, y)) {
        TerrBin *b;
        short sx, sy;
        long v = (hview_cols[c0] + hview_cols[c1]) >> 1;
        b = STRIP;
        sy = b->y;
        b->col = (short)v;
        sx = b->x;
        b->y = (short)y;
        b->x = (short)x;
        viewdist_2A13B8(b, 0);
        b = STRIP;
        b->x = sx;
        b->y = sy;
    }
}

/* bisection between a (in view) and b (not) on line y: the last x in view, to hz_step */
long haze_2B458C(long a, long b, long y)
{
    while (iabs(a - b) > hz_step) {
        long m = ((a + b) / 2) & hz_mask;
        if (haze_2B40C8(m, y))
            a = m;
        else
            b = m;
    }
    return b;
}

long haze_2B45D8(long a, long b, long y)
{
    while (haze_2B40C8(b, y))
        b += b - a;
    return haze_2B458C(a, b, y);
}

long haze_2B4630(long a, long b, long x)
{
    while (iabs(a - b) > hz_step) {
        long m = ((a + b) / 2) & hz_mask;
        if (haze_2B40C8(x, m))
            a = m;
        else
            b = m;
    }
    return b;
}

long haze_2B4674(long a, long b, long x)
{
    while (haze_2B40C8(x, b))
        b += b - a;
    return haze_2B4630(a, b, x);
}

long haze_2B46D0(uchar *cell)
{
    long s = SHADE(cell) + SHADE(cell + 3);
    cell += hz_stride * 3;
    s += SHADE(cell);
    return (SHADE(cell + 3) + s) >> 2;
}

long haze_2B4724(uchar *cell)
{
    return (SHADE(cell) + SHADE(cell + 3)) >> 1;
}

long haze_2B4748(uchar *cell)
{
    return (SHADE(cell) + SHADE(cell + hz_stride * 3)) >> 1;
}

uchar haze_2B4780(long col, fix y)
{
    long v = y >> 16;
    if (col < 0)
        return v <= hview_cols[0];
    if (col >= hz_ncols)
        return v <= hview_cols[hz_ncols - 1];
    return v <= hview_cols[col];
}

/* the first square from a towards b (step hz_step) on line y / x that projects; it starts the
 * ring's silhouette */
static void first_point(fix sx, fix sy, uchar *cell, uchar avg)
{
    hz_col = sx >> 16;
    hz_lasty = sy;
    hz_shade = avg ? haze_2B46D0(cell) : (long)SHADE(cell);
    hz_vis = haze_2B4780(hz_col, hz_lasty);
#ifndef TN_WATCOM_ABI
    hz_lvy = hz_pvy;
    hz_lvz = hz_pvz;
    hz_lh = hz_ph;
    hz_lcell = hz_pcell;
    if (hz_fog)
        hz_lf = hz_fog_point();
    if (hz_smooth)
        hz_vert(&hz_lv, hz_pcell, hz_pmx, hz_pmy);
#endif
}

long haze_2B47C0(long a, long b, long y)
{
    long s;
    fix sx, sy;
    if (hz_vis)
        return a;
    for (s = a; s <= b; s += hz_step)
        if (haze_2B4198(s, y, &sx, &sy)) {
            first_point(sx, sy, hz_fine ? haze_2B3FD0(s - 1, y - 1) : haze_2B3FD0(s, y), hz_fine);
            return s;
        }
    return s;
}

long haze_2B486C(long a, long b, long y)
{
    long s;
    fix sx, sy;
    if (hz_vis)
        return a;
    for (s = a; s >= b; s -= hz_step)
        if (haze_2B4198(s, y, &sx, &sy)) {
            first_point(sx, sy, haze_2B3FD0(s, y), hz_fine);
            return s;
        }
    return s;
}

long haze_2B4918(long a, long b, long x)
{
    long s;
    fix sx, sy;
    if (hz_vis)
        return a;
    for (s = a; s <= b; s += hz_step)
        if (haze_2B4198(x, s, &sx, &sy)) {
            first_point(sx, sy, haze_2B3FD0(x, s), hz_fine);
            return s;
        }
    return s;
}

long haze_2B49C4(long a, long b, long x)
{
    long s;
    fix sx, sy;
    if (hz_vis)
        return a;
    for (s = a; s >= b; s -= hz_step)
        if (haze_2B4198(x, s, &sx, &sy)) {
            first_point(sx, sy, hz_fine ? haze_2B3FD0(x, s - 1) : haze_2B3FD0(x, s), hz_fine);
            return s;
        }
    return s;
}

/* one walked square: objects, silhouette span */
static void walk_square(long dx, long dy, long kind)
{
    long col = hz_sx >> 16, sh;
    uchar vis;
    uchar *p = hz_cell;
    long st = hz_stride * 3;
    if (hz_near)
        haze_2B44FC(col, hz_col, dx, dy);
#ifndef TN_WATCOM_ABI
    if (hz_fog)
        hz_pf = hz_fog_point();
    if (hz_smooth)
        hz_vert(&hz_pv, hz_pcell, hz_pmx, hz_pmy);
#endif
    vis = haze_2B4780(col, hz_sy);
    if (hz_vis || vis) {
        p = hz_cell;
        switch (kind) {
        case 0:     /* 2B4A70 */
            sh = hz_fine ? haze_2B4748(p) : (long)SHADE(p);
            HZ_SPAN(col, p - 3, sh);
            break;
        case 1:     /* 2B4B6C */
            sh = hz_fine ? haze_2B4748(p - st) : haze_2B46D0(p);
            HZ_SPAN(col, hz_cell - st, sh);
            break;
        case 2:     /* 2B4C78 */
            sh = hz_fine ? haze_2B4724(p - 3) : (long)SHADE(p);
            HZ_SPAN(col, hz_cell - 3 - st, sh);
            break;
        default:    /* 2B4D88 */
            sh = hz_fine ? haze_2B4724(p) : (long)SHADE(p);
            HZ_SPAN(col, hz_cell, sh);
            break;
        }
        hz_shade = sh;
    } else
        hz_shade = 0x10;
#ifndef TN_WATCOM_ABI
    if (hz_smooth) {
        hz_smooth_record(col);
        hz_lv = hz_pv;
    }
#endif
    hz_lasty = hz_sy;
    hz_col = col;
    hz_vis = vis;
#ifndef TN_WATCOM_ABI
    hz_lvy = hz_pvy;
    hz_lvz = hz_pvz;
    hz_lh = hz_ph;
    hz_lcell = hz_pcell;
    hz_lf = hz_pf;
#endif
}

void haze_2B4A70(long a, long b, long y)
{
    long s = haze_2B47C0(a, b, y);
    haze_2B42C0(s, y, hz_step, 0);
    for (; s <= b; s += hz_step)
        if (haze_2B4428())
            walk_square(-1, 0, 0);
}

void haze_2B4B6C(long a, long b, long y)
{
    long s = haze_2B486C(b, a, y);
    haze_2B42C0(s, y, -hz_step, 0);
    for (; s >= a; s -= hz_step)
        if (haze_2B4428())
            walk_square(0, -1, 1);
}

void haze_2B4C78(long a, long b, long x)
{
    long s = haze_2B4918(a, b, x);
    haze_2B42C0(x, s, 0, hz_step);
    for (; s <= b; s += hz_step)
        if (haze_2B4428())
            walk_square(-1, -1, 2);
}

void haze_2B4D88(long a, long b, long x)
{
    long s = haze_2B49C4(b, a, x);
    haze_2B42C0(x, s, 0, -hz_step);
    for (; s >= a; s -= hz_step)
        if (haze_2B4428())
            walk_square(0, 0, 3);
}

void haze_2B4E8C(void)
{
    long i;
    for (i = 0; i < 0x400; i++) {
        long v = (long)((ulong)hviewdiv_2AB3CC(i, *(long *)&hz_cam.g_20[0]) << 4) >> 16;
        if (v > 15)
            v = 15;
        hz_rad1[i] = (uchar)v;
        hz_rad2[i] = (uchar)v;
    }
    ffpipe_43CC11 = 0;
}

void haze_2B4ED0(void)
{
    long i;
    for (i = 0; i < 0x400; i++)
        if (hz_rad1[i] != hz_rad2[i])
            DbgReportError(1, "Memory trash: hazeRadius in checkHazingOffset.  Type 'tncrash'\n");
}

void haze_2B4EFC(long a, long b, long c, uchar up)
{
    if (a == b)
        return;
    if (a > b) {
        long t = a;
        a = b;
        b = t;
    }
    if (hz_first) {
        hz_ends[2] = up ? a : b;
        hz_ends[3] = c;
        hz_first = 0;
    }
    hz_ends[0] = up ? b : a;
    hz_ends[1] = c;
    if (up)
        haze_2B4A70(a, b, c);
    else
        haze_2B4B6C(a, b, c);
}

void haze_2B4F68(long a, long b, long c, uchar up)
{
    if (a == b)
        return;
    if (a > b) {
        long t = a;
        a = b;
        b = t;
    }
    if (hz_first) {
        hz_ends[2] = c;
        hz_ends[3] = up ? a : b;
    }
    hz_ends[0] = c;
    hz_ends[1] = up ? b : a;
    if (up)
        haze_2B4C78(a, b, c);
    else
        haze_2B4D88(a, b, c);
}

#ifndef TN_WATCOM_ABI
/* the fine walks' limits: the map and the land around it (port/video/mapedge.c, G35) */
#define FLO (-PF_M)
#define FHI (0x200 + PF_M)
#else
#define FLO 0
#define FHI 0x200
#endif

/* the ring row y from a to c: clip both ends to the view (lo / hi), walk it; returns 1 when the
 * start moved, 2 when the end moved */
long haze_2B4FD0(long a, long b, long c, uchar lo, long hi, long y, uchar up)
{
    long s, e;
    if (!hz_coarse) {
        if (y < FLO || y >= FHI || b < FLO || b >= FHI)
            return 0;
        if (a < FLO)
            a = FLO;
        if (c >= FHI)
            c = FHI - hz_step;
    } else {
        if (y < hz_ca8 || y >= hz_ca0 || b < hz_ca4 || b >= hz_c9c)
            return 0;
        if (a < hz_ca4)
            a = hz_ca4;
        if (c >= hz_c9c)
            c = hz_c9c - hz_step;
    }
    s = lo ? haze_2B458C(b, a, y) : a;
    e = hi ? haze_2B458C(b, c, y) : c;
    haze_2B4EFC(s, e, y, up);
    return (e != c ? 2 : 0) + (a != s);
}

/* the ring row y from b towards a, b in view: walk it to where the view ends */
long haze_2B50DC(long a, long b, long y, uchar up)
{
    if (!hz_coarse) {
        if (y < FLO || y >= FHI)
            return 0;
        if (b < FLO || b >= FHI)
            return 0;
        if (a < FLO)
            a = FLO;
        else if (a >= FHI)
            a = FHI - hz_step;
    } else {
        if (y < hz_ca8 || y >= hz_ca0)
            return 0;
        if (b < hz_ca4 || b >= hz_c9c)
            return 0;
        if (a < hz_ca4)
            a = hz_ca4;
        else if (a >= hz_c9c)
            a = hz_c9c - hz_step;
    }
    if (!haze_2B40C8(b, y))
        return 0;
    a = haze_2B45D8(b, a, y);
    haze_2B4EFC(a, b, y, up);
    return a != b;
}

long haze_2B51AC(long a, long b, long c, uchar lo, long hi, long x, uchar up)
{
    long s, e;
    if (!hz_coarse) {
        if (b < FLO || b >= FHI)
            return 0;
        if (x < FLO || x >= FHI)
            return 0;
        if (a < FLO)
            a = FLO;
        if (c >= FHI)
            c = FHI - hz_step;
    } else {
        if (b < hz_ca8 || b >= hz_ca0)
            return 0;
        if (x < hz_ca4 || x >= hz_c9c)
            return 0;
        if (a < hz_ca8)
            a = hz_ca8;
        if (c >= hz_ca0)
            c = hz_ca0 - hz_step;
    }
    s = lo ? haze_2B4630(b, a, x) : a;
    e = hi ? haze_2B4630(b, c, x) : c;
    haze_2B4F68(s, e, x, up);
    return (e != c ? 2 : 0) + (a != s);
}

long haze_2B52D0(long a, long b, long x, uchar up)
{
    if (!hz_coarse) {
        if (b < FLO || b >= FHI)
            return 0;
        if (x < FLO || x >= FHI)
            return 0;
        if (a < FLO)
            a = FLO;
        else if (a >= FHI)
            a = FHI - hz_step;
    } else {
        if (b < hz_ca8 || b >= hz_ca0)
            return 0;
        if (x < hz_ca4 || x >= hz_c9c)
            return 0;
        if (a < hz_ca8)
            a = hz_ca8;
        else if (a >= hz_ca0)
            a = hz_ca0 - hz_step;
    }
    if (!haze_2B40C8(x, b))
        return 0;
    a = haze_2B4674(b, a, x);
    haze_2B4F68(a, b, x, up);
    return a != b;
}

void haze_2B53B0(void)
{
    long i = hz_cam.band0, n = 1;
    hz_bands[0] = 1;
    for (; i < hz_cam.band_end; i++)
        hz_bands[n++] = hz_cam.band[i].b;
    if (n == 5) {
        hz_bands[n] = hz_bands[n - 1] * 3 / 2;
        n++;
    }
    hz_nbands = n;
    hz_bands[n] = 999;
}

void haze_2B5448(long x, long y)
{
    fix sx, sy;
    if (haze_2B4198(x, y, &sx, &sy))
        WC3(grd_pixel_table[1], 0xFC, (short)(sx >> 16), (short)(sy >> 16));
}

#ifndef TN_WATCOM_ABI
/* the end of the drawing: the smooth runs, the fogged columns off */
static void hz_end(void)
{
    if (hz_smooth)
        hz_runs_draw();
    port_fog_cols = 0;
}

/* "Object distance" (port/video/screen.c): the objects that come with map tiles (trees, rocks ...).
 * The game collects them on the squares it walks in its near rings (walk_square, out to
 * dist[4], 20 squares) and on the polygons' strips (weather_2B1814, 12 to 48 squares with Terrain
 * detail); here also on every square of the rings past those out to 48 (Far) or 96 (Farthest)
 * squares, a ring before the caster draws it, like the moving objects (haze_2B3BA4).  The rings
 * are counted from the viewer's own square (the caster's centre moves to the coarse grid). */
static int hz_tiles;                    /* this drawing collects them */
static long hz_tat;                     /* the next ring of tile objects */

static long hz_tile_end(void)
{
    return port_obj_dist == 1 ? 48 : port_obj_dist >= 2 ? 96 : 0;
}

/* the tile objects of square (x, y), when it is in the view */
static void hz_tile_square(long x, long y)
{
    long h, col, m = hz_ncols / 32;
    fix ex, ey, a, d;
    long long q;
    uchar v;
    TerrBin *b;
    short sx, sy;
    if ((ulong)x >= 0x200 || (ulong)y >= 0x200)
        return;
    v = game_3C5E1C[x >> 2][y >> 2];
    if (!v || !tile_groups[v].cell[x & 3][y & 3])
        return;
    /* its column (haze_2B4198 without its side effects, a 64-bit division) */
    ey = (y << 16) - hz_view.y + 0x8000;
    ex = (x << 16) - hz_view.x + 0x8000;
    h = (short)*(ushort *)(haze_2B3FD0(x, y) + 1) >> 4;
    a = hz_mat[1] * h + fmul(ey, hz_mat[0]) + fmul(ex, hz_mat[2]) + hz_view.mz[0];
    d = fmul(ey, hz_mat[6]) + hz_mat[7] * h + fmul(ex, hz_mat[8]) + hz_view.mz[2];
    if (d < 0x10)
        return;
    q = ((long long)a << 16) / d;
    q = ((long long)hz_cam.half_w * (q + 0x10000)) >> 16;
    if (q < -m || q >= hz_ncols + m)
        return;
    col = q < 0 ? 0 : q >= hz_ncols ? hz_ncols - 1 : (long)q;
    vd_center_col = col;
    b = STRIP;
    sx = b->x;
    sy = b->y;
    b->col = (short)hview_cols[col];
    b->x = (short)x;
    b->y = (short)y;
    viewdist_2A13B8(b, 0);
    b = STRIP;
    b->x = sx;
    b->y = sy;
}

/* the rings of tile objects up to (not including) ring */
static void hz_tile_rings(long ring, uchar pos)
{
    long end = hz_tile_end(), vx = hz_view.x >> 16, vy = hz_view.y >> 16, dir = pos ? 1 : -1, j;
    if (hz_tat <= hz_dist4)
        hz_tat = hz_dist4 + 1;
    for (; hz_tat < ring && hz_tat <= end; hz_tat++) {
        long at = hz_tat;
        if (hz_xmajor) {
            for (j = -at; j <= at; j++)
                hz_tile_square(vx + j, vy + dir * at);
            for (j = 0; j < at; j++) {
                hz_tile_square(vx - at, vy + dir * j);
                hz_tile_square(vx + at, vy + dir * j);
            }
        } else {
            for (j = -at; j <= at; j++)
                hz_tile_square(vx + dir * at, vy + j);
            for (j = 0; j < at; j++) {
                hz_tile_square(vx + dir * j, vy - at);
                hz_tile_square(vx + dir * j, vy + at);
            }
        }
    }
}

/* the fog's end (haze_2B5484): the moving objects the rings from at out to 120 squares would
 * still have collected (the rings' collection below, from the centre as it is now) */
static void hz_fog_objects(long at, long mainc, long latc, uchar pos)
{
    long xm = mainc - at, xp = at + mainc, yp = at + latc, ym = latc - at;
    for (; at <= 0x78; at++) {      /* not those Object distance adds: they fade out */
        if (hz_xmajor)
            haze_2B3BA4(mainc, pos ? yp : ym, at);
        else
            haze_2B3BA4(pos ? xp : xm, latc, at);
        xm--;
        xp++;
        yp++;
        ym--;
    }
}
#endif

void haze_2B5484(FfCamera *cam, FfView *view, void *strips)
{
    GameMap *m;
    long x0, y0, i;
    uchar pos;                 /* the ring moves towards + on the main axis */
    long dir, ring, side, mainc, latc, step, band, at;
    fix slope, cross;

    StripCheckForMemoryTrash(cam);
#ifndef TN_WATCOM_ABI
    port_pf_sync();                     /* the map with the land around it (port/video/mapedge.c) */
    if (port_s3d_on)
        hz_water_tex();
    hz_lcell = NULL;
    hz_smooth = 0;
    hz_fog = 0;
#endif
#ifndef TN_WATCOM_ABI
    if (port_s3d_on)
        big_nodes_init();
    else
#endif
    if (hz_init) {
        for (i = 0; i < 0x3F; i++)
            hz_nodes[i].next = i + 1;
        hz_nodes[i].next = 0;
        hz_init = 0;
    }
    memset(HZ_HEAD, 0, HZ_HEAD_SIZE);
    m = GMAP;
    x0 = m->x0;
    y0 = m->y0;
    hz_ca4 = -x0 * 4;
    hz_ca8 = -y0 * 4;
    hz_c9c = (0x100 - x0) * 4;
    hz_coarse = 0;
    hz_ca0 = (0x100 - y0) * 4;
    memcpy(&hz_view, view, 0x5C);
    memcpy(&hz_cam, cam, 0xBA);
    memcpy(hz_log2, hz_cam.log2, 0x100);
    memcpy(hz_divx, hz_cam.div_x, 0x400);
    memcpy(hz_divy, hz_cam.div_y, 0x400);
    hz_cx = hz_cam.half_w << 16;
    hz_cy = (hz_cam.half_h << 16) + 0x10000;
    if (ffpipe_43CC11)
        haze_2B4E8C();
    else
        haze_2B4ED0();
    haze_2B402C();
    hz_dist4 = (ushort)hz_cam.dist[4];
    hz_dist5 = (ushort)hz_cam.dist[5];
    hz_dist6 = (ushort)hz_cam.dist[6];
    hz_ncols = hz_cam.half_w * 2;
    hz_clut = grd_canvas->gc.fill_type == 1 ? grd_canvas->gc.fill_parm : 0;
    haze_2B53B0();
#ifndef TN_WATCOM_ABI
    if (hz_clut && port_s3d_on)         /* Infrared: Sci-fi, the ground never hot (infrared.c) */
        hz_clut = (long)port_ir_land_clut((const uchar *)hz_clut);
    /* the far land smooth (above): not with the game's own colour table (hz_clut) nor in the
     * dark (game_35CADC, one light level) */
    hz_smooth = port_s3d_on && port_far_smooth && !hz_clut && game_35CADC < 0x10 && !strip_mark &&
                hz_ncols <= HZ_COLS;
    if (hz_smooth)
        hz_smooth_frame();
    /* "Fog" (port/video/fog.c): through the fog colours, no farther than the fog's end (in the
     * infrared too: there through the camera's colour table, port_fog_level) */
    hz_fog = port_fog_on && (!hz_clut || port_fog_ir) && !strip_mark && hz_ncols <= 4096;
    if (hz_fog) {
        port_fog_view(&hz_cam);
        hz_fog_end = port_fog_ring();
        port_fog_cols = 1;
    }
    if (hz_smooth)                      /* (with hz_fog) */
        hz_runs_begin();
    hz_tiles = port_s3d_on && port_obj_dist && !strip_mark;
    hz_tat = 0;
#endif

    if (iabs(hz_mat[2]) > iabs(hz_mat[0])) {
        hz_xmajor = 1;
        pos = hz_mat[6] > 0;
        hz_pos = hz_mat[2] > 0;
    } else {
        hz_xmajor = 0;
        pos = hz_mat[8] > 0;
        hz_pos = hz_mat[0] > 0;
    }
    dir = pos ? 1 : -1;
#ifndef TN_WATCOM_ABI
    hz_dpos = pos;
#endif
    if (hz_xmajor) {
        slope = fdiv(hz_view.cos_h, hz_view.sin_h);
        if (hz_pos)
            slope = -slope;
        cross = view->x + slope;
    } else {
        slope = fdiv(hz_view.sin_h, hz_view.cos_h);
        if (!hz_pos)
            slope = -slope;
        cross = view->y + slope;
    }
    if (ff_ybuf_set && ff_bolt_dist >> 16 < 8)
        ff_ybuf_set = 0;

    ring = 5;
    side = dir * 5;
    mainc = view->x >> 16;      /* viewer square x */
    latc = view->y >> 16;       /* viewer square y */
    step = 1;
    band = 1;
    at = 0;
    hz_mask = -1;
    hz_nheads = hz_bands[3];
#ifndef TN_WATCOM_ABI
    if (port_s3d_on && (port_obj_dist || port_enemy_dist))
        hz_nheads = 0x200;              /* objects parked out to the land's end too */
#endif
    hz_ends[2] = mainc;
    hz_ends[3] = latc;
    hz_near = 1;
    hz_ends[0] = hz_ends[2];
    hz_ends[1] = hz_ends[3];
    for (;;) {
        long cr;
        if (ff_ybuf_set && ff_bolt_dist >> 16 < ring)
            ffpipe_29E234(hview_cols);
        hz_level = hz_rad1[ring];
        hz_shade_row = planet_shade + (hz_level << 8);
        strip_2AC33C();
        hz_ring = ring;
#ifndef TN_WATCOM_ABI
        if (hz_smooth)
            hz_hf = hz_haze(ring);
        if (hz_fog)
            port_fog_level(hz_level);
#endif
        hz_vis = 0;
        hz_step = step;
        hz_fine = step == 1;
        /* the objects of the rings inside the first one drawn */
        if (!hz_near && at <= HZ_OBJ_RING) {
            long xm = mainc - at, xp = at + mainc, yp = at + latc, ym = latc - at;
            for (; at < ring; at++) {
                if (hz_xmajor)
                    haze_2B3BA4(mainc, pos ? yp : ym, at);
                else
                    haze_2B3BA4(pos ? xp : xm, latc, at);
                xm--;
                xp++;
                yp++;
                ym--;
            }
        }
#ifndef TN_WATCOM_ABI
        if (!hz_near && hz_tiles)
            hz_tile_rings(ring, pos);
#endif
        hz_first = 1;
#ifndef TN_WATCOM_ABI
        hz_stride = hz_coarse ? hz_strides[hz_coarse] : PF_W;   /* the map's copy (mapedge.c) */
#else
        hz_stride = hz_strides[hz_coarse];
#endif
        cr = (cross >> 16) & hz_mask;
        if (hz_xmajor) {
            long ly = latc + side, xh = mainc + ring, xl = mainc - ring;
            if (hz_pos) {
                long r = haze_2B52D0(latc, ly, xl, 0);
                if (!(haze_2B4FD0(xl, cr, xh, r == 0, 1, ly, 1) & 2))
                    haze_2B52D0(latc, ly, xh, 1);
            } else {
                long r = haze_2B52D0(latc, ly, xh, 1);
                haze_2B4FD0(xl, cr, xh, 1, r == 0, ly, 0);
                haze_2B52D0(latc, ly, xl, 0);
            }
        } else {
            long lx = mainc + side, yh = latc + ring, yl = latc - ring;
            if (hz_pos) {
                long r = haze_2B50DC(mainc, lx, yl, 1);
                if (!(haze_2B51AC(yl, cr, yh, r == 0, 1, lx, 1) & 2))
                    haze_2B50DC(mainc, lx, yh, 0);
            } else {
                long r = haze_2B50DC(mainc, lx, yh, 0);
                if (!(haze_2B51AC(yl, cr, yh, 1, r == 0, lx, 0) & 1))
                    haze_2B50DC(mainc, lx, yl, 1);
            }
        }
#ifndef TN_WATCOM_ABI
        if (hz_fog && ring >= hz_fog_end) {
            /* the fog's end: past it all is the fog's colour, the sky's fill below the horizon.
             * The moving objects out to the game's 120 squares are still drawn (fogged). */
            hz_fog_objects(at, mainc, latc, pos);
            hz_end();
            if (ff_ybuf_set)
                ffpipe_29E234(hview_cols);
            return;
        }
#endif
        /* next ring; the step doubles at the band distances */
        while (ring >= hz_bands[band]) {
            band++;
            if (band == hz_nbands) {
#ifndef TN_WATCOM_ABI
                hz_end();
#endif
                if (ff_ybuf_set)
                    ffpipe_29E234(hview_cols);
                return;
            }
            hz_mask += hz_mask;
            step += step;
            dir += dir;
            slope += slope;
            if (step == 4)
                hz_coarse = 1;
            ring = hz_mask & (ring + step - 1);
            side = dir > 0 ? ring : -ring;
            latc &= hz_mask;
            mainc &= hz_mask;
        }
        ring += step;
        side += dir;
        cross += slope;
        if (ring > hz_dist4)
            hz_near = 0;
    }
}

void haze_2B5B70(FfView *v)
{
    v->x = hz_play[0];
    v->y = hz_play[1];
    v->z = hz_play[2];
    v->pitch = hz_play_p;
    v->heading = hz_play_h;
}

long haze_2B5BA4(char *name)
{
    void *fp = fopen(name, "r");
    long n, a, b;
    uchar *cam;
    if (!fp)
        return -1;
    if (crt_2FB71F(fp, "%i\n", &n) != 1)
        return -1;
    if (n == 3)
        cam = game_camera;
    else if (n > 3)
        return -1;
    else
        cam = ff_cams_390010[n];
    if (*(long *)(cam + 0x40) == 0)
        return -1;
    hviewdiv_2AB448();
    hz_playing = 1;
    hz_play_cam = n;
    while (crt_2FB71F(fp, "%x:%x:%x:%x:%x\n", &hz_play[0], &hz_play[1], &hz_play[2], &b, &a) == 5) {
        hz_play_h = (short)b;
        hz_play_p = (short)a;
        HelmTaskHook();
    }
    fclose(fp);
    hz_playing = 0;
    return 0;
}
