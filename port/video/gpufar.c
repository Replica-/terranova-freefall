/* gpufar.c -- Freefall: the GPU ground's far land (gpuland.c).
 *
 * Beyond the terrain's polygons (the smooth bands, at most 48 squares out: the ring patterns end
 * there) the game draws the land with its ray caster (haze.c, colras.c), column by column in
 * screen space: nothing there is a polygon the GPU ground could catch.  So the far land is made
 * here from the map, in the main view's sharp drawing after the game drew it: for each of the
 * camera's column bands, the squares of its rings at its step (1, 2, 4, 8 squares), each turned
 * into the game's view space the way terrrast.c turns the near ground's corners (the ff_view
 * matrix, a cell 1.0, the height times its height column), lit by its corners' shade as the near
 * ground is, through the light table of its haze level and fog as weather.c's squares are, with
 * its square's texture.  The caster's pixels are noted as the ground's (colras.c, haze.c), so the
 * card's far land shows where the game's does; where the card has none (water, a crack between
 * two steps) the game's pixel stays (gpuland_mix).
 * Left out for now: water (the game's), the textures' turn by the square's variant, the infrared. */
#include <math.h>
#include "g5_a.h"
#include "g5_c.h"
#include "g6_b.h"
#include "mapedge.h"
#include "gpuland.h"

extern ushort port_poly_have[PF_W][PF_W], port_poly_gen;   /* terrstrp.c: squares with polygons */
int port_hz_water(uchar *c);                                /* haze.c */
int port_fog_gpu(float *start, float *end, int *colour);    /* fog.c */
extern unsigned long tn_ticks;                              /* the game's 250 Hz clock */

#define NEAR_Z 0.05f
#define SIDE 1.15f                      /* x / z and y / z past the view's edge still kept */
static const float hmax = 512.0f;      /* a square's height around its middle's, at most (map units;
                                         * the quick test's room: generous) */

static uchar *far_cell(long x, long y)
{
    x = x < -PF_M ? -PF_M : x > 0x200 + PF_M ? 0x200 + PF_M : x;
    y = y < -PF_M ? -PF_M : y > 0x200 + PF_M ? 0x200 + PF_M : y;
    return PF_CELL(x, y);
}

/* the haze level of a square d squares away (weather.c bin_haze) */
static long far_haze(long d, long bv)
{
    long v;
    d = d < 0 ? 0 : d > 0x3FF ? 0x3FF : d;
    v = (long)((ulong)hviewdiv_2AB3CC(d, bv) << 4) >> 16;
    return v > 15 ? 15 : v < 0 ? 0 : v;
}

void port_gl_far(void)
{
    const fix *m = ff_view.m;
    float M[9], mz[3], vx = ff_view.x / 65536.0f, vy = ff_view.y / 65536.0f;
    long cx = ff_view.x >> 16, cy = ff_view.y >> 16, bv = *(long *)&ff_cam.g_20[0], k, i;
    int storm = game_35CADC >= 0x10, fog_c = 0, gpu_fog;
    float fog_s = 0, fog_e = 1;
    static long haze_r[512];

    if (!port_gl)
        return;
    for (i = 0; i < 512; i++)
        haze_r[i] = -1;
    for (i = 0; i < 9; i++)
        M[i] = m[i] / 65536.0f;
    for (i = 0; i < 3; i++)
        mz[i] = ff_view.mz[i] / 65536.0f;
    if (port_fog_on)
        port_fog_view(&ff_cam);
    /* the view and the fog, for the card to fog each pixel by its distance on the map */
    gpu_fog = port_fog_gpu(&fog_s, &fog_e, &fog_c);
    port_gl_view(M, mz, gpu_fog, fog_s, fog_e, fog_c);
    if (!grd_canvas->gc.fill_type && (game_35CADA == 0 || game_35CADA == 1)) {
        /* the weather (weather.c: rain 0, snow 1; not in the infrared, as the game's drops): its
         * drop colour, how many drops the views have against the usual, its drift across the
         * view against its fall */
        const WxType *t = &wx_types[game_35CADA];
        long most = 0, fy = t->dy0 + t->dy1;
        for (i = 0; i < 4; i++)
            if (wx_views[i].drops && wx_views[i].drops != (WxDrop *)-1 && wx_views[i].count > most)
                most = wx_views[i].count;
        port_gl_weather(game_35CADA, t->color, most > 0 ? (most < 60 ? 0.5f : most > 400 ? 2.0f : most / 200.0f) : 1.0f,
                        fy ? (float)(t->dx0 + t->dx1) / fy : 0.0f, tn_ticks / 250.0f, vx, vy);
    }
    if (ff_cam.g_5c != 1 && ff_cam.g_5c != 2)  /* (the ground textured only, as the near ground) */
        return;
    if (grd_canvas->gc.fill_type)       /* the infrared: the game's */
        return;

    /* k = band0 - 1: the rings nearer than the column bands, square by square where the near
     * ground has no polygons (the caster draws those: the polygons' cone leaves squares out) */
    for (k = ff_cam.band0 - 1; k < ff_cam.band_end && k < 6; k++) {
        long a, b, s, gx, gy, x0, y0;
        if (k < ff_cam.band0) {
            a = 1;
            b = ff_cam.band0 < 6 ? ff_cam.band[ff_cam.band0].a : 0;
            s = 1;
        } else {
            a = ff_cam.band[k].a;
            b = ff_cam.band[k].b;
            s = ff_cam.band[k].step;
        }
        if (s < 1)
            s = 1;
        if (b <= a)
            continue;
        x0 = (cx - b) & ~(s - 1);
        y0 = (cy - b) & ~(s - 1);
        for (gy = y0; gy <= cy + b; gy += s)
            for (gx = x0; gx <= cx + b; gx += s) {
                long dx = gx + s / 2 - cx, dy = gy + s / 2 - cy;
                long r = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
                static const long cor[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
                PortGlVert v[4];
                uchar *cell = far_cell(gx, gy), *tex;
                const uchar *lt;
                int out_l = 0, out_r = 0, out_t = 0, out_b = 0, behind = 0, j;
                long lv, t;
                if (r < a || r >= b)
                    continue;
                {   /* the square's middle at its height, with room for its size: off the view
                     * (behind, or well past a side), before its corners are worked out */
                    float ex = gx + s * 0.5f - vx, ey = gy + s * 0.5f - vy;
                    float h = (float)((short)*(ushort *)(cell + 1) >> 4);
                    float zc = ey * M[6] + ex * M[8] + h * M[7] + mz[2];
                    float xc = ey * M[0] + ex * M[2] + h * M[1] + mz[0];
                    float rz = s * (fabsf(M[6]) + fabsf(M[8])) + hmax * fabsf(M[7]);
                    float rx = s * (fabsf(M[0]) + fabsf(M[2])) + hmax * fabsf(M[1]);
                    if (zc + rz < NEAR_Z || xc - rx > SIDE * (zc + rz) || xc + rx < -SIDE * (zc + rz))
                        continue;
                }
                if (s == 1 && PF_IN(gx, gy) && port_poly_have[gy + PF_M][gx + PF_M] == port_poly_gen)
                    continue;           /* the near ground's polygons are there */
                if (port_hz_water(cell))
                    continue;           /* the game's water */
                for (j = 0; j < 4; j++) {
                    long col = gx + cor[j][0] * s, row = gy + cor[j][1] * s;
                    uchar *c = far_cell(col, row);
                    float ex = col - vx, ey = row - vy, h = (float)((short)*(ushort *)(c + 1) >> 4);
                    float x = ey * M[0] + ex * M[2] + h * M[1] + mz[0];
                    float y = ey * M[3] + ex * M[5] + h * M[4] + mz[1];
                    float z = ey * M[6] + ex * M[8] + h * M[7] + mz[2];
                    v[j].x = x;
                    v[j].y = y;
                    v[j].z = z;
                    v[j].u = (float)(cor[j][0] * 64 * s);
                    v[j].v = (float)(cor[j][1] * 64 * s);
                    v[j].shade = storm ? 15.5f : (float)(15 - ((*(ushort *)c >> 6) & 0xE)) + 0.5f;
                    behind += z < NEAR_Z;
                    out_l += x < -SIDE * z;
                    out_r += x > SIDE * z;
                    out_t += y < -SIDE * z;
                    out_b += y > SIDE * z;
                }
                if (behind || out_l == 4 || out_r == 4 || out_t == 4 || out_b == 4)
                    continue;
                t = *cell & 0x7F;
                tex = s == 1 && (t & 0x40) ? BlastFindTmap(t & 0x3F) : planet_tex[t & 0x3F];
                if (!tex)
                    continue;
                if (r >= 0 && r < 512) {    /* (about the square's distance; once a ring) */
                    if (haze_r[r] < 0)
                        haze_r[r] = far_haze(r * 1414 / 1000, bv);
                    lv = haze_r[r];
                } else
                    lv = far_haze(r * 1414 / 1000, bv);
                lt = port_s3d_haze_ltab ? port_s3d_haze_ltab + lv * 0x1800 : grd_screen->ltab;
                if (port_fog_on && !gpu_fog) {
                    long f = (port_fog_at(((gx << 16) + (s << 15)) - ff_view.x,
                                          ((gy << 16) + (s << 15)) - ff_view.y) + 0x80) >> 8;
                    lt = port_fog_ltab((int)lv, lt, (int)f);
                }
                port_gl_poly(v, 4, tex, 0, lt);
            }
    }
}
