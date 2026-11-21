/* weather.c -- Terra Nova module M092 (0x2AF4E0-0x2B26D4; r:\prj\tn\v109\tn\src\tn\weather.c):
 * rain / snow drops over the camera views, the thunder storm, the water texture animation,
 * the terrain triangle clipper and the terrain bin drawing.
 * Decompiled from __FF.EXE v1.09.  Names are ours except back_intersect (error string).
 *
 * Drops (per view, wx_views): weather_2AFF54 reserves a view for a camera, weather_2AFE38 sizes
 * it, weather_2AFD4C (kind) builds the drops (weather_2AFBD8), weather_2AF4E0 moves them with
 * the viewer and weather_2AFA5C draws the ones of a depth band while the terrain bins are drawn
 * far to near (weather_2B1DCC).
 * Storm (game_35CADC): 1 waiting, 0x11 .. 0x10 counting down to a strike (weather_2B0178),
 * 2..15 bolt growing / shown.
 * Clipper: weather_2B1218 projects a vertex and sets its outcodes (0x80 behind, 1 / 2 left /
 * right, 8 / 4 top / bottom); weather_2B0548 clips a polygon against the planes in the order
 * 0x80, 1, 8, 2, 4 (new vertices come from wx_clip_pool: 0x10 / 0x20 mark x / y clipped ones).
 * Bins: weather_2B16B0 sorts the strips' bins into 9 groups around the mesh rectangle;
 * weather_2B1814 builds and clips the two triangles of each bin; weather_2B1F24 /
 * weather_2B1DCC draw them with the band's polygon routine (terr_poly_fns).
 *
 * Quirks kept:
 *  - weather_2AF4E0: the first update of a view (lastx == 0xFFFF0000) uses the sideways drift
 *    from a stack slot the original never writes; the entry stub reads that slot (see there).
 *  - weather_2AF964 fills all 32 entries of wx_sin / wx_cos with the same value; only [0] is
 *    read.
 *  - weather_2AFF54 looks at a fifth view (past wx_views) when all four are in use.
 *  - weather_2AFE38 frees drops == 0 too (f_free(0)).
 *  - weather_2B0548: new vertices get sx from the division unless only x-clipped, but sy from
 *    the division only when x-clipped and not y-clipped (screen edge otherwise).
 *  - weather_2B16B0 does not terminate the group lists (weather_2B1814 clears every bin's next).
 *  - weather_2B1F24 with wx_region > 8 uses an uninitialised order row in the original; we
 *    draw nothing.
 */
#include "g6_b.h"

#ifndef TN_WATCOM_ABI
extern int port_highfps_fix;            /* port/core/platform.c: above 25 frames a second */
extern int port_s3d_on;                 /* port/video/sharp3d.c: drawing the sharp view */
void port_s3d_drop(long c, const void *bm, long x16, long y16);
extern uchar **port_sky_refl_row;       /* sky_refl_row, or the sharp view's longer one */
uchar *port_marks_tex(long tex, int big);   /* port/mods/marks.c: the effects pack's BlastMarks */
#include "gpuland.h"                    /* port/video/gpuland.c: the GPU ground's capture */
static const uchar *gl_haze_lt;         /* ... the square's light table before the fog (draw_tri) */
#endif

#ifndef TN_LINK_ORIGINAL_DATA
long wx_snd_chan;
ulong wx_thunder_wait;
WxType wx_types[2];
RndStream wx_rnd[1];
fix wx_snd_speed;
long wx_tex_acc;
ulong wx_last_time;
fix wx_near_z;
long wx_band_dist[3];
long wx_draw_order[9][9];
long wx_rows[17];
void *terr_poly_fns[8];
grs_bitmap terr_tex_bm;
grs_bitmap wx_bm[2];
ulong wx_bolt_tick;
fix wx_cos[32];
long wx_h16;
fix wx_sin[32];
long wx_cx16, wx_cy3;
WxView wx_views[4];
ushort wx_heading;
uchar wx_bolt_done, wx_thunder_vol;
TrVert *wx_clip_next, *wx_clip_prev, *wx_clip_pool, *wx_clip_new;
fix wx_clip_d[3];
fix wx_clip_t;
long wx_clip_count;
uchar wx_region;
TerrBin *wx_bins[9];
uchar wx_grid_mode, wx_in_center;
uchar game_39A404;
#endif

#define VIEW(i) ((WxView *)((uchar *)wx_views + (i) * (long)sizeof(WxView)))

/* ------------------------------------------------------------------ drops */

/* The original reads the sideways drift of a view's first update from a stack slot it never
 * writes ([esp + 4] of its frame: 44 bytes below the return address).  Called through the
 * Watcom thunk (W_weather_2AF4E0: 3 saved registers, 2 arguments, the C return address), that
 * slot is 20 bytes below our entry esp and still holds what the caller's earlier calls left
 * there, so the entry stub saves it before our own frame overwrites it.  Called from C the value
 * is whatever lies there (as in the original, where it depends on the caller). */
long wx_stale_vx;                    /* (ours, not an original variable) */
void weather_2AF4E0_c(FfView *pos, long idx) __attribute__((used));
#ifdef TN_WATCOM_ABI
__asm__("\t.text\n\t.globl weather_2AF4E0\nweather_2AF4E0:\n"
        "\tmovl -20(%esp), %eax\n\tmovl %eax, wx_stale_vx\n\tjmp weather_2AF4E0_c\n");
#else
void weather_2AF4E0(FfView *pos, long idx) { weather_2AF4E0_c(pos, idx); }
#endif

void weather_2AF4E0_c(FfView *pos, long idx)
{
    long shift = 4 - ((game_35A540 >> 1) != 0);
    WxView *W = VIEW(idx);
    ulong dt;
    long vx = wx_stale_vx, vy, hx;
    ushort hd;
    WxDrop *p;
#ifndef TN_WATCOM_ABI
    ulong ms;
#endif

    if (!W->count)
        return;
#ifndef TN_WATCOM_ABI
    /* above 25 frames a second the drops move by the time passed (ms, 40 = one 25 fps frame)
     * instead of at least one step a frame, which made the rain 2.4 times too fast at 60 */
    ms = game_time - W->time;
    if (ms > 120)
        ms = 120;
#endif
    dt = (game_time - W->time) >> 5;
    if (dt < 1)
        dt = 1;
    else if (dt > 3)
        dt = 3;
    W->time = game_time;
    if (W->lastx != (fix)0xFFFF0000) {
        fix dx = pos->x - W->lastx, dy = pos->y - W->lasty, a, b, t;
        if (game_35CADA == 1)
#ifndef TN_WATCOM_ABI
            dx += port_highfps_fix ? (fix)(ms * 0x1249 / 40) : dt * 0x1249;
#else
            dx += dt * 0x1249;
#endif
        a = fix_mul(dx, pos->cos_h) + fix_mul(dy, pos->sin_h);
        b = fix_mul(dx, pos->sin_h) - fix_mul(dy, pos->cos_h);
        t = b * 40;
        if (b > 0)
            vx = (W->x1 * t / 0x140) >> 16;
        else
            vx = (short)((W->x1 * t / 0x140 + 0xFFFF) >> 16);
        if (a > 0)
            vy = (a * 4) >> 16;
        else
            vy = (short)((a * 4 + 0xFFFF) >> 16);
        if (vy || vx) {
            W->lastx = pos->x;
            W->lasty = pos->y;
            if (vy > 2)
                vy = 2;
            else if (vy < -2)
                vy = -2;
        }
    } else {
        W->lastx = pos->x;
        vy = 0;
        W->lasty = pos->y;
    }
    hd = (ushort)(pos->heading - W->heading);
    W->heading = pos->heading;
    if (hd < 0x8000) {
        fix t = fix_mul(hd, 0x6487F);
        hx = -(fix_mul(t, W->turn << 4) >> 16);
    } else {
        fix t = fix_mul(hd, 0x6487F);
        hx = fix_mul(0x6487F - t, W->turn << 4) >> 16;
    }
    for (p = W->drops; p < W->drops + W->count; p++) {
        long d = p->depth, nd;
        ulong s;
        long X;

        p->depth = (signed char)(d + vy);
        nd = p->depth;
        if (nd < 16 || nd > 32) {
            p->state = 2;
            p->depth = nd < 16 ? 32 : 16;
            nd = p->depth;
            p->x = (short)(RndRange(wx_rnd, W->x0, W->x1) << 4);
            p->y = (short)(RndRange(wx_rnd, W->y0, W->y1) << 4);
        }
        if (nd >= 0x1C) {
            if (d < 0x1C)
                p->state = RndRange(wx_rnd, 0, 1) == 0;
        } else
            p->state = 2;
        s = ((ulong)(dt * nd) << 4) >> 5;
#ifndef TN_WATCOM_ABI
        if (port_highfps_fix)
            s = ((ulong)(ms * nd) << 4) / (40 * 32);
#endif
        p->x += (short)(RndRange(wx_rnd, W->t.dx0, W->t.dx1) * s);
        p->y += (short)(RndRange(wx_rnd, W->t.dy0, W->t.dy1) * s);
        p->x += (short)(p->depth * vx + hx);
        X = p->x >> 4;
        if (X < W->x0) {
            p->x = (short)(RndRange(wx_rnd, (3 * W->x1) >> 2, W->x1) << 4);
            p->y = (short)(RndRange(wx_rnd, W->y0, W->y1) << shift);
        } else if (X > W->x1) {
            p->x = (short)(RndRange(wx_rnd, W->x0, W->x0 + ((W->x1 - W->x0) >> 2)) << 4);
            p->y = (short)(RndRange(wx_rnd, W->y0, W->y1) << shift);
        } else {
            long lim = W->y1 - 2 * (0x20 - nd), Y = p->y >> 4;
            if (Y > lim) {
                p->x = (short)(RndRange(wx_rnd, W->x0, W->x1) << 4);
                p->y = (short)(RndRange(wx_rnd, W->y0, W->y0 + ((W->y1 - W->y0) >> 2)) << shift);
            } else if (Y < W->y0) {
                p->x = (short)(RndRange(wx_rnd, W->x0, W->x1) << 4);
                p->y = (short)(RndRange(wx_rnd, (3 * W->y1) >> 2, W->y1) << shift);
            }
        }
    }
}

void weather_2AF958(void)
{
    wx_snd_chan = -1;
}

void weather_2AF964(long h)
{
    long i;
    wx_heading = (ushort)h;
    for (i = 0; i < 32; i++)
        fix_sincos((fixang)h, &wx_sin[i], &wx_cos[i]);
}

void weather_2AF9B8(long *ox, long *oy, WxDrop *p)
{
    short h = (short)ff_view.pitch;
    long x = p->x, y = p->y;
    long ah = h < 0 ? -h : h;

    if (ah > 0x1000) {
        long q;
        if ((ushort)h != wx_heading)
            weather_2AF964(h);
        q = (long)(((long long)wx_sin[0] * (y - wx_cy3)) / wx_h16);
        x = wx_cx16 + fix_mul(x - wx_cx16, 0x10000 - q);
    }
    *ox = x >> 4;
    *oy = (game_35A540 >> 1) ? y >> 3 : y >> 4;
}

void weather_2AFA5C(long a, long b)
{
    WxView *W;
    long lo, hi;
    WxDrop *p;

    if (grd_canvas->gc.fill_type)
        return;
    W = VIEW(a);
    wx_h16 = (W->y1 - W->y0) << 4;
    wx_cx16 = ((W->x1 + W->x0) << 4) / 2;
    wx_cy3 = wx_h16 / 3;
    lo = ((3 - b) << 4) / 4 + 0x10;
    hi = lo + 3;
    if (!W->drops)
        return;
    for (p = W->drops; p < W->drops + W->count; p++) {
        long sx, sy;
        if (p->depth < lo || p->depth > hi)
            continue;
#ifndef TN_WATCOM_ABI
        if (port_s3d_on) {              /* the sharp view: the same drops, k times the pixels */
            long x16, y16;
            weather_2AF9B8(&sx, &sy, p);
            x16 = p->x;
            if (((short)ff_view.pitch < 0 ? -(short)ff_view.pitch : (short)ff_view.pitch) > 0x1000)
                x16 = wx_cx16 + fix_mul(x16 - wx_cx16,
                                        0x10000 - (long)(((long long)wx_sin[0] * (p->y - wx_cy3)) / wx_h16));
            y16 = (game_35A540 >> 1) ? p->y * 2 : p->y;
            port_s3d_drop(W->t.color, p->state == 2 || !wx_bm[0].bits ? NULL :
                          p->state ? &wx_bm[1] : &wx_bm[0], x16, y16);
            continue;
        }
#endif
        weather_2AF9B8(&sx, &sy, p);
        if (p->state == 2)
            gen_fill_pixel(W->t.color, (short)sx, (short)sy);
        else if (!wx_bm[0].bits) {
            gen_fill_pixel(W->t.color, (short)sx, (short)sy);
            gen_fill_pixel(W->t.color, (short)sx, (short)sy);
        } else
            WATCOM_CALL(grd_canvas_table[147], (long)(p->state ? &wx_bm[1] : &wx_bm[0]),
                        (short)sx, (short)sy);
    }
}

void weather_2AFBD8(WxView *w, long scale)
{
    WxDrop *p;

    memcpy(&w->t, &wx_types[(signed char)game_35CADA], sizeof(WxType));
    w->t.dx0 = (signed char)(w->t.dx0 * w->x1 / 300);
    w->t.dy0 = (signed char)(w->t.dy0 * w->x1 / 300);
    w->t.dx1 = (signed char)(w->x1 * w->t.dx1 / 300);
    w->t.dy1 = (signed char)(w->x1 * w->t.dy1 / 300);
    w->count = (short)((w->x1 - w->x0) * (w->y1 - w->y0) / scale);
    w->drops = f_malloc(w->count * 6);
    for (p = w->drops; p < w->drops + w->count; p++) {
        p->x = (short)(RndRange(wx_rnd, w->x0, w->x1) << 4);
        p->y = (short)(RndRange(wx_rnd, w->y0, w->y1) << 4);
        p->depth = (signed char)RndRange(wx_rnd, 0x10, 0x20);
        if (p->depth >= 0x1C)
            p->state = (uchar)RndRange(wx_rnd, 0, 1);
        else
            p->state = 2;
    }
}

void weather_2AFD4C(long kind, long scale)
{
    WxView *W;
    uchar k = (uchar)kind;

    game_35CADA = k;
    if (wx_snd_chan >= 0)
        snd_chan_stop(wx_snd_chan);
    if (k != 0xFF && wx_types[(signed char)game_35CADA].bits_a) {
        WxType *t = &wx_types[(signed char)game_35CADA];
        gr_init_bitmap(&wx_bm[0], t->bits_a, 2, 1, t->size, t->size);
        t = &wx_types[(signed char)game_35CADA];
        gr_init_bitmap(&wx_bm[1], t->bits_b, 2, 1, t->size, t->size);
    } else {
        wx_bm[0].bits = 0;
        wx_bm[1].bits = 0;
    }
    if (k == 0xFF)
        return;
    for (W = wx_views; W < wx_views + 4; W++)
        if (W->drops == (WxDrop *)-1)
            weather_2AFBD8(W, scale);
}

void weather_2AFE38(long idx, FfCamera *cam)
{
    WxView *W = VIEW(idx);
    long h = cam->half_h, hh, w3;

    if (game_35A540 >> 1)
        h >>= 1;
    hh = (h * 2) >> 4;
    w3 = (cam->half_w * 3) >> 4;
    if (W->drops != (WxDrop *)-1) {
        long area, scale;
        f_free(W->drops);
        area = (W->y1 - W->y0) * (W->x1 - W->x0);
        W->drops = (WxDrop *)-1;
        scale = area / W->count;
        W->x0 = (short)-w3;
        W->y0 = (short)-hh;
        W->y1 = (short)(h * 2 + hh);
        W->x1 = (short)((ushort)cam->half_w * 2 + w3);
        weather_2AFBD8(W, scale);
        return;
    }
    W->x0 = (short)-w3;
    W->y0 = (short)-hh;
    W->x1 = (short)((ushort)cam->half_w * 2 + w3);
    W->y1 = (short)(h * 2 + hh);
}

long weather_2AFF54(FfCamera *cam, long b)
{
    long i = 0;
    WxView *W;

    for (;;) {
        if (!VIEW(i)->drops)
            break;
        i++;
        if (i * (long)sizeof(WxView) >= 0xCC)
            break;
    }
    W = VIEW(i);
    W->drops = (WxDrop *)-1;
    W->count = 0;
    W->heading = 0;
    W->lastx = (fix)0xFFFF0000;
    W->lasty = (fix)0xFFFF0000;
    W->turn = cam->zx;
    W->x0 = (short)(-cam->half_w >> 3);
    W->y0 = (short)(-cam->half_h >> 3);
    W->x1 = (short)((cam->half_w >> 3) + cam->half_w * 2);
    W->y1 = (short)((cam->half_h >> 3) + cam->half_h * 2);
    W->f32 = (uchar)b;
    return i;
}

void weather_2AFFFC(long idx)
{
    WxView *W = VIEW(idx);
    if (W->drops == (WxDrop *)-1)
        W->drops = 0;
    if (W->drops) {
        f_free(W->drops);
        W->drops = 0;
    }
}

/* ------------------------------------------------------------------ storm */

void weather_2B003C(void)
{
    if (wx_thunder_wait)
        return;
    game_35CADC = 0x11;
    wx_thunder_wait = (ulong)RndRange(game_rnd_text, 0, 0x7FFF) % 2000 + 1000;
    wx_thunder_vol = (uchar)(100000UL / wx_thunder_wait);
}

void weather_2B0090(fix x, fix y)
{
    GameEntity *pl = &game_entities[game_player_entity];
    fix d = fix_safe_pyth_dist(x - pl->x, y - pl->y, "r:\\prj\\tn\\v109\\tn\\src\\tn\\weather.c",
                               0x2E5);
    long n = fix_div(d, wx_snd_speed) >> 16;
    long id;

    if (n > 0xDAC)
        id = RndRange(game_rnd_text, 0, 0x7FFF) % 3 + 0x771;
    else if (n > 1000)
        id = 0x774;
    else {
        wx_thunder_vol = 0x78;
        id = 0x775;
    }
    SndSchedule(n, (ushort)id, 0, 0x14, 1, wx_thunder_vol, 0x40, 0);
}

void weather_2B0158(ulong dt)
{
    ulong w = wx_thunder_wait;
    if (w)
        w = dt > w ? 0 : w - dt;
    wx_thunder_wait = w;
}

void weather_2B0178(void)
{
    fix to[3];
    long a = RndRange(game_rnd_text, 0, 0x7FFF) & 0x1FF;
    long b = RndRange(game_rnd_text, 0, 0x7FFF) & 0x1FF;

    ff_bolt_pos[0] = a << 16;
    ff_bolt_pos[2] = 0xA00000;
    ff_bolt_pos[1] = b << 16;
    to[1] = b << 16;
    to[0] = a << 16;
    to[2] = 0;
    thunder_2B32BC(0x60000, 0x60000);
    ff_bolt = thunder_2B3404(ff_bolt_pos, to);
    if (!ff_bolt)
        game_35CADC = 1;
    wx_bolt_done = 0;
    ff_bolt_time = game_time;
    wx_bolt_tick = game_time;
    weather_2B0090(ff_bolt_pos[0], ff_bolt_pos[1]);
}

void weather_2B0238(void)
{
    ff_bolt = 0;
}

void weather_2B0244(void)
{
    long st = game_35CADC;
    ulong t = game_time;

    if (st == 1) {
        if (!(RndRange(game_rnd_text, 0, 0x7FFF) & 0x3F))
            weather_2B003C();
        return;
    }
    if (st >= 0x10) {
        game_35CADC = st - 1;
        if (st - 1 < 0x10)
            weather_2B0178();
        return;
    }
    if (!wx_bolt_done) {
        long d = (long)((t - wx_bolt_tick) >> 3);
        if (d < 1)
            d = 1;
        else if (d > 0xF)
            d = 0xF;
        wx_bolt_done = thunder_2B33E4(ff_bolt, d);
        wx_bolt_tick = t;
        return;
    }
    if (!ff_bolt) {
        game_35CADC = 1;
        return;
    }
    if (t - ff_bolt_time > 0x400) {
        thunder_2B2DBC(ff_bolt);
        game_35CADC = 1;
        ff_bolt = 0;
        return;
    }
    if (st == 0xF)
        game_35CADC = 0xE;
}

long weather_2B033C(void)
{
    return RndRange(wx_rnd, 0, 0x500) & 0x3F;
}

void weather_2B0358(long dt)
{
    wx_tex_acc += dt;
    if (wx_tex_acc <= 0)
        return;
    do {
        long i;
        wx_tex_acc -= RndRange(wx_rnd, 0x2D, 0x46);
        for (i = 0; i < 0x41; i++) {
            long off = RndRange(wx_rnd, 0, 0x500) & 0x3F;
            long v;
            off += weather_2B033C() << 6;
            v = RndRange(wx_rnd, 0x1D, 0x20);
            if (i == 0x40)
                continue;
            planet_tex[i][off] = (uchar)v;
            if (RndRange(wx_rnd, 0, 0xF) == 1) {
                uchar *row = planet_tex_rows[i];
                row[RndRange(wx_rnd, 0, 0xFF)] = (uchar)v;
            }
        }
    } while (wx_tex_acc > 0);
}

void weather_2B0434(void)
{
    ulong t = game_time;
    ulong dt = t - wx_last_time;

    if (wx_thunder_wait) {
        if (dt > wx_thunder_wait)
            wx_thunder_wait = 0;
        else
            wx_thunder_wait -= dt;
    }
    if (game_35CADA == 0 && !game_35CD9C)
        weather_2B0358(t - wx_last_time);
    wx_last_time = t;
    if (game_35CADA != 0xFF && wx_types[(signed char)game_35CADA].snd && wx_snd_chan < 0) {
        long ch = SndPlay((ushort)wx_types[(signed char)game_35CADA].snd, 0, 10, 0, -1, 0x40, 0);
        wx_snd_chan = ch;
        if (ch >= 0)
            snd_chan_add_callback(ch, WATCOM_FN(weather_2AF958), 0);
    }
    if (game_35CADC)
        weather_2B0244();
    rndsky_2A6DEC();
}

/* ------------------------------------------------------------------ clipper */

void weather_2B0520(void)
{
#ifdef TN_WATCOM_ABI
    wx_clip_pool = ffmem_2C4F40(0x79E0);
#else
    wx_clip_pool = ffmem_2C4F40(0x79E0 * PORT_S3D_MUL);   /* widescreen, detail: more terrain */
#endif
    wx_clip_count = 0;
}

void weather_2B053C(void)
{
    ffmem_2C4FE4(wx_clip_pool);
}

/* recompute the or of the outcodes */
static uchar codes_or(TrVert **v, long n)
{
    uchar o = 0;
    long i;
    for (i = 0; i < n; i++)
        o |= v[i]->f24;
    return o;
}

uchar weather_2B0548(TrVert **in, long n, TrVert **out, long *pn)
{
    static const uchar bits[5] = { 0x80, 1, 8, 2, 4 };
    TrVert *buf[2][10];
    TrVert **src = in, **dst = buf[0];
    uchar o = 0, a = 0x8F;
    long i, j, k, toggle = 0;

    for (i = 0; i < n; i++) {
        o |= in[i]->f24;
        a &= in[i]->f24;
    }
    if (!o) {
        if (out != in)
            memcpy(out, in, n * 4);
        *pn = n;
        return 1;
    }
    if (a) {
        *pn = 0;
        return 0;
    }
    for (k = 0; k < 5; k++) {
        uchar bit = bits[k];
        long m = 0;
        if (!(o & bit))
            continue;
        for (j = 0; j < n; j++) {
            long nx = j + 1 == n ? 0 : j + 1;
            wx_clip_prev = src[j];
            wx_clip_next = src[nx];
            if (!(wx_clip_prev->f24 & bit))
                dst[m++] = src[j];
            if ((wx_clip_prev->f24 ^ wx_clip_next->f24) & bit) {
                switch (bit) {
                case 0x80: back_intersect(); break;
                case 1: weather_2B0DA4(); break;
                case 8: weather_2B0EC8(); break;
                case 2: weather_2B0FF4(); break;
                default: weather_2B1118(); break;
                }
                dst[m++] = wx_clip_new;
            }
        }
        n = m;
        src = dst;
        if (bit == 4)
            break;
        o = codes_or(dst, n);
        toggle = 1 - toggle;
        dst = toggle ? buf[1] : buf[0];
    }
    memcpy(out, src, n * 4);
    for (j = 0; j < n; j++) {
        TrVert *v = out[j];
        uchar c;
        wx_clip_new = v;
        if (!(v->kind & 0x20))
            continue;
        c = v->f24;
        if ((c & 0x10) && !(c & 0x20))
            *(fix *)&v->f00[0] = v->x > 0 ? (long)grd_canvas->bm.w << 16 : 0;
        else
            *(fix *)&v->f00[0] = (fix_div(v->x, v->z) + 0x10000) * ff_cam.half_w;
        c = wx_clip_new->f24;
        if (!(c & 0x20) && (c & 0x10))
            *(fix *)&v->f00[4] = (fix_div(v->y, v->z) + 0x10000) * ff_cam.half_h;
        else
            *(fix *)&v->f00[4] = wx_clip_new->y > 0 ? (long)grd_canvas->bm.h << 16 : 0;
    }
    *pn = n;
    return n > 0;
}

void weather_2B0B18(void)
{
    TrVert *p = wx_clip_prev, *q = wx_clip_next, *v = wx_clip_new;
    uchar fl = p->kind | 0x28;

    v->kind = fl;
    if (fl & 0x10) {
        v->r = p->r + fix_mul(wx_clip_t, q->r - p->r);
        v->g = fix_mul(wx_clip_t, q->g - p->g) + p->g;
        v->b = p->b + fix_mul(wx_clip_t, q->b - p->b);
    } else {
        if (fl & 1)
            v->r = fix_mul(wx_clip_t, q->r - p->r) + p->r;
        if (v->kind & 1)
            v->g = fix_mul(wx_clip_t, q->g - p->g) + p->g;
    }
    if (v->kind & 4)
        v->shade = p->shade + fix_mul(wx_clip_t, q->shade - p->shade);
}

static TrVert *clip_alloc(void)
{
    wx_clip_new = (TrVert *)((uchar *)wx_clip_pool + wx_clip_count * 0x27);
    wx_clip_count++;
#ifdef TN_WATCOM_ABI
    if (wx_clip_count >= 0x320)
#else
    if (wx_clip_count >= 0x320 * PORT_S3D_MUL)     /* the pool is that much bigger */
#endif
        DbgReportError(1, "back_intersect: Too many temporary points!\n");
    return wx_clip_new;
}

void back_intersect(void)
{
    TrVert *v = clip_alloc(), *p = wx_clip_prev, *q = wx_clip_next;
    uchar c;

    wx_clip_d[0] = q->x - p->x;
    wx_clip_d[1] = q->y - p->y;
    wx_clip_d[2] = q->z - p->z;
    wx_clip_t = fix_div(wx_near_z - p->z, wx_clip_d[2]);
    v->x = fix_mul(wx_clip_t, wx_clip_d[0]) + p->x;
    v->y = p->y + fix_mul(wx_clip_t, wx_clip_d[1]);
    v->z = wx_near_z;
    c = v->x >= v->z ? 2 : (-v->z >= v->x);
    c |= v->y >= v->z ? 4 : (-v->z >= v->y ? 8 : 0);
    v->f24 = c;
    weather_2B0B18();
}

void weather_2B0DA4(void)
{
    TrVert *v = clip_alloc(), *p = wx_clip_prev, *q = wx_clip_next;

    wx_clip_d[0] = q->x - p->x;
    wx_clip_d[1] = q->y - p->y;
    wx_clip_d[2] = q->z - p->z;
    wx_clip_t = -fix_div(p->z + p->x, wx_clip_d[0] + wx_clip_d[2]);
    v->y = p->y + fix_mul(wx_clip_t, wx_clip_d[1]);
    v->z = p->z + fix_mul(wx_clip_t, wx_clip_d[2]);
    v->x = -v->z;
    v->f24 = (v->z <= v->y ? 4 : (-v->z >= v->y ? 8 : 0)) | 0x10;
    weather_2B0B18();
}

void weather_2B0EC8(void)
{
    TrVert *v = clip_alloc(), *p = wx_clip_prev, *q = wx_clip_next;

    wx_clip_d[0] = q->x - p->x;
    wx_clip_d[1] = q->y - p->y;
    wx_clip_d[2] = q->z - p->z;
    wx_clip_t = -fix_div(p->z + p->y, wx_clip_d[1] + wx_clip_d[2]);
    v->x = fix_mul(wx_clip_t, wx_clip_d[0]) + p->x;
    v->z = p->z + fix_mul(wx_clip_t, wx_clip_d[2]);
    v->y = -v->z;
    v->f24 = (v->z <= v->x ? 2 : 0) | 0x20 | (wx_clip_prev->f24 & wx_clip_next->f24 & 0x10);
    weather_2B0B18();
}

void weather_2B0FF4(void)
{
    TrVert *v = clip_alloc(), *p = wx_clip_prev, *q = wx_clip_next;

    wx_clip_d[0] = q->x - p->x;
    wx_clip_d[1] = q->y - p->y;
    wx_clip_d[2] = q->z - p->z;
    wx_clip_t = fix_div(p->z - p->x, wx_clip_d[0] - wx_clip_d[2]);
    v->y = fix_mul(wx_clip_t, wx_clip_d[1]) + p->y;
    v->z = p->z + fix_mul(wx_clip_t, wx_clip_d[2]);
    v->x = v->z;
    v->f24 = (v->y >= v->z ? 4 : 0) | 0x10 | (wx_clip_prev->f24 & wx_clip_next->f24 & 0x20);
    weather_2B0B18();
}

void weather_2B1118(void)
{
    TrVert *v = clip_alloc(), *p = wx_clip_prev, *q = wx_clip_next;

    wx_clip_d[0] = q->x - p->x;
    wx_clip_d[1] = q->y - p->y;
    wx_clip_d[2] = q->z - p->z;
    wx_clip_t = fix_div(p->z - p->y, wx_clip_d[1] - wx_clip_d[2]);
    v->x = fix_mul(wx_clip_t, wx_clip_d[0]) + p->x;
    v->z = p->z + fix_mul(wx_clip_t, wx_clip_d[2]);
    v->y = v->z;
    v->f24 = (wx_clip_prev->f24 & wx_clip_next->f24 & 0x10) | 0x20;
    weather_2B0B18();
}

void weather_2B1218(TrVert *v)
{
    uchar c = v->z < 0 ? 0x80 : 0;

    if (v->x > v->z)
        c |= 2;
    else if (-v->z >= v->x)
        c |= 1;
    if (v->y >= v->z)
        c |= 4;
    else if (-v->z >= v->y)
        c |= 8;
    v->f24 = c;
    if (c)
        return;
    *(fix *)&v->f00[0] = ff_cam.half_w * (fix_div(v->x, v->z) + 0x10000);
    *(fix *)&v->f00[4] = ff_cam.half_h * (fix_div(v->y, v->z) + 0x10000);
    v->kind |= 8;
}

/* ------------------------------------------------------------------ terrain bins */

#define BINV(b, k) ((TrVert *)((uchar *)tr_verts + tr_idx[(b)->key[k]] * 0x27))

#ifndef TN_WATCOM_ABI
/* the sharp view with more terrain detail (port/video/sharp3d.c): the smooth ground
 * reaches into the haze, so each square of it is drawn through the light table of its haze level,
 * as the game's columns are (haze_2B4E8C's levels by distance, from the camera's haze value) */
static uchar haze_lv[0x400];
static long haze_b = -1;

static long bin_haze(const TerrBin *b)
{
    long d, bv = *(long *)&ff_cam.g_20[0], i;
    if (!port_s3d_on || !port_s3d_haze_ltab)
        return 0;
    if (bv != haze_b) {
        for (i = 0; i < 0x400; i++) {
            long v = (long)((ulong)hviewdiv_2AB3CC(i, bv) << 4) >> 16;
            haze_lv[i] = (uchar)(v > 15 ? 15 : v);
        }
        haze_b = bv;
    }
    d = fix_fast_pyth_dist((ff_view.x >> 16) - b->x, (ff_view.y >> 16) - b->y);
    return haze_lv[d < 0 ? 0 : d > 0x3FF ? 0x3FF : d];
}
#endif

static void uv(TrVert *v, fix u, fix w)
{
    v->r = u;
    v->g = w;
}

void weather_2B12D0(TerrBin *b)
{
    TrVert *v0 = BINV(b, 0);
    switch (v0->variant) {
    case 0:
        uv(BINV(b, 0), 0x2000, 0x2000);
        uv(BINV(b, 1), 0x3FE000, 0x2000);
        uv(BINV(b, 3), 0x2000, 0x3FE000);
        uv(BINV(b, 2), 0x3FE000, 0x3FE000);
        break;
    case 1:
        uv(BINV(b, 0), 0x2000, 0x3FE000);
        uv(BINV(b, 1), 0x2000, 0x2000);
        uv(BINV(b, 3), 0x3FE000, 0x3FE000);
        uv(BINV(b, 2), 0x3FE000, 0x2000);
        break;
    case 2:
        uv(BINV(b, 0), 0x3FE000, 0x3FE000);
        uv(BINV(b, 1), 0x2000, 0x3FE000);
        uv(BINV(b, 3), 0x3FE000, 0x2000);
        uv(BINV(b, 2), 0x2000, 0x2000);
        break;
    case 3:
        uv(BINV(b, 0), 0x3FE000, 0x2000);
        uv(BINV(b, 1), 0x3FE000, 0x3FE000);
        uv(BINV(b, 3), 0x2000, 0x2000);
        uv(BINV(b, 2), 0x2000, 0x3FE000);
        break;
    }
}

void weather_2B1694(void)
{
    memset(&wx_region, 0, 0x25);   /* wx_region and wx_bins[9] */
    weather_2B16AC();
}

void weather_2B16AC(void)
{
}

void weather_2B16B0(long mesh, fix x, fix y)
{
    short *r = (short *)((uchar *)GMAP->f1056 + mesh * 8);   /* x0, y0, x1, y1 */
    TerrBin **tails[9];
    long X = x >> 16, Y = y >> 16, i;
    TerrBin *b;

    for (i = 0; i < 9; i++) {
        tails[i] = &wx_bins[i];
        wx_bins[i] = 0;
    }
    if (X >= r[2])
        wx_region = Y >= r[3] ? 6 : Y < r[1] ? 4 : 2;
    else if (X < r[0])
        wx_region = Y >= r[3] ? 7 : Y < r[1] ? 5 : 3;
    else
        wx_region = Y >= r[3] ? 1 : Y < r[1] ? 0 : 8;
    for (i = strip_count - 1, b = G6B_BINS + strip_count - 1; i >= 0; i--, b--) {
        long col = b->x >= r[2] ? 2 : b->x >= r[0];
        long row = b->y >= r[3] ? 6 : b->y < r[1] ? 0 : 3;
        *tails[col + row] = b;
        tails[col + row] = &G6B_BIN_NEXT(b);
    }
    mesh_groups = (MeshGroup *)wx_bins[4];
}

void weather_2B1814(void)
{
    WxTri *A = (WxTri *)tr_buf30, *B = A + 1;
    uchar textured = ff_cam.g_5c == 1 || ff_cam.g_5c == 2;
    TerrBin *b = G6B_BINS;
    long i;

    for (i = 0; strip_count > i; i++, b++, A += 2, B += 2) {
        long x, y;
        uchar facing;

        G6B_BIN_NEXT(b) = 0;
#ifndef TN_WATCOM_ABI
        if (port_s3d_on && port_s3d_haze_ltab)
            hz_level = bin_haze(b);     /* its objects hazed as the ground under them */
#endif
        viewdist_2A13B8(b, 0);
        if (textured)
            weather_2B12D0(b);
        x = b->x;
        y = b->y;
        if (((x ^ y) & 1) == 1) {
            fix d = ((x + y) << 16) - ff_view.x - ff_view.y;
            fix s = ff_view.cos_h + ff_view.sin_h;
            if (d >= (fix)0xFFFF0000) {
                A->v[0] = BINV(b, 0);
                B->v[1] = BINV(b, 2);
                B->v[0] = A->v[1] = BINV(b, 1);
                B->v[2] = A->v[2] = BINV(b, 3);
                G6B_BIN_FL(b) &= ~4;
                facing = s >= 0;
            } else {
                A->v[1] = BINV(b, 2);
                B->v[0] = BINV(b, 0);
                B->v[1] = A->v[0] = BINV(b, 1);
                B->v[2] = A->v[2] = BINV(b, 3);
                G6B_BIN_FL(b) |= 4;
                facing = s <= 0;
            }
        } else {
            fix d = ((x - y) << 16) - ff_view.x + ff_view.y;
            if (d >= 0) {
                A->v[2] = BINV(b, 3);
                B->v[0] = BINV(b, 1);
                B->v[2] = A->v[0] = BINV(b, 0);
                B->v[1] = A->v[1] = BINV(b, 2);
                G6B_BIN_FL(b) &= ~4;
                facing = ff_view.cos_h >= ff_view.sin_h;
            } else {
                A->v[0] = BINV(b, 1);
                B->v[2] = BINV(b, 3);
                B->v[0] = A->v[2] = BINV(b, 0);
                B->v[1] = A->v[1] = BINV(b, 2);
                G6B_BIN_FL(b) |= 4;
                facing = ff_view.cos_h <= ff_view.sin_h;
            }
        }
        B->n = 3;
        A->n = B->n;
        G6B_BIN_A(b) = A;
        G6B_BIN_B(b) = B;
        if (weather_2B0548(A->v, 3, A->v, &A->n))
            G6B_BIN_FL(b) |= 1;
        else
            G6B_BIN_FL(b) &= ~1;
        if (weather_2B0548(B->v, 3, B->v, &B->n))
            G6B_BIN_FL(b) |= 2;
        else
            G6B_BIN_FL(b) &= ~2;
        if (facing) {
            if ((G6B_BIN_FL(b) & 1) && !water_scan_edges(A->n, (fix **)A->v, hview_cols))
                G6B_BIN_FL(b) &= ~1;
            if ((G6B_BIN_FL(b) & 2) && !water_scan_edges(B->n, (fix **)B->v, hview_cols))
                G6B_BIN_FL(b) &= ~2;
        } else {
            if ((G6B_BIN_FL(b) & 2) && !water_scan_edges(B->n, (fix **)B->v, hview_cols))
                G6B_BIN_FL(b) &= ~2;
            if ((G6B_BIN_FL(b) & 1) && !water_scan_edges(A->n, (fix **)A->v, hview_cols))
                G6B_BIN_FL(b) &= ~1;
        }
    }
}

/* the probe pixel: take it back over after a polygon was drawn on it */
static void probe_check(void)
{
    if (ff_cam.probe && *ff_cam.probe_ptr != 0xFF) {
        ff_cam.probe_hit = 0;
        ff_cam.probe_save = *ff_cam.probe_ptr;
        *ff_cam.probe_ptr = 0xFF;
    }
}

#ifndef TN_WATCOM_ABI
void port_log(const char *fmt, ...);
#endif

#ifndef TN_WATCOM_ABI
extern int port_lit_dither;             /* src/2d/perloop.c */
static void draw_tri_lit(void *fn, WxTri *t, TerrBin *b);

/* "Terrain smoothing" (port/video/screen.c): in the sharp view the ground's light runs smooth between
 * its points (dithered between levels) instead of in bands of whole levels */
static void draw_tri(void *fn, WxTri *t, TerrBin *b)
{
    port_lit_dither = port_s3d_on && port_far_smooth;
    draw_tri_lit(fn, t, b);
    port_lit_dither = 0;
}

static void draw_tri_lit(void *fn, WxTri *t, TerrBin *b)
#else
static void draw_tri(void *fn, WxTri *t, TerrBin *b)
#endif
{
#ifndef TN_WATCOM_ABI
    long lv;
    if (fn == NULL) {
        /* a square of a band whose polygon routine is none (mode 0): the original would call
         * address 0.  Seen once switching from the goggles to the cockpit (P23); skip it */
        static int told;
        if (!told++)
            port_log("terrain: square %ld,%ld of band %d has no polygon routine (bands %d..%d, "
                     "sharp view %d)\n", b->x, b->y, b->band, ff_cam.band0, ff_cam.band_end,
                     port_s3d_on);
        return;
    }
    lv = bin_haze(b);
    if (port_s3d_on && grd_canvas->gc.fill_type == 1 && !lv && !port_fog_on) {
        long clut = grd_canvas->gc.fill_parm;     /* Infrared: Sci-fi, the ground never hot */
        grd_canvas->gc.fill_parm = (long)port_ir_land_clut((const uchar *)clut);
        WATCOM_CALL(fn, (long)t->v, t->n, b->x, b->y);
        grd_canvas->gc.fill_parm = clut;
        return;
    }
    if (lv || port_fog_on) {
        uchar *g = g3d_light_tab, *s = grd_screen->ltab, *lt = port_s3d_haze_ltab + lv * 0x1800;
        long clut = grd_canvas->gc.fill_parm;
        if (port_s3d_on && grd_canvas->gc.fill_type == 1)   /* ... (infrared.c) */
            grd_canvas->gc.fill_parm = (long)port_ir_land_clut((const uchar *)clut);
        if (port_fog_on) {              /* "Fog" (port/video/fog.c): the square's, by its middle */
            long f;
            port_fog_view(&ff_cam);
            f = (port_fog_at((b->x << 16) + 0x8000 - ff_view.x, (b->y << 16) + 0x8000 - ff_view.y) + 0x80) >> 8;
            if (port_fog_ir && grd_canvas->gc.fill_type == 1)   /* the infrared: its colour table */
                grd_canvas->gc.fill_parm = (long)port_fog_clut((const uchar *)grd_canvas->gc.fill_parm, f);
            else {
                gl_haze_lt = lt;        /* (the GPU ground fogs each pixel itself) */
                lt = (uchar *)port_fog_ltab(lv, lt, f);
            }
        }
        g3d_light_tab = grd_screen->ltab = lt;
        WATCOM_CALL(fn, (long)t->v, t->n, b->x, b->y);
        gl_haze_lt = NULL;
        g3d_light_tab = g;
        grd_screen->ltab = s;
        grd_canvas->gc.fill_parm = clut;
        return;
    }
#endif
    WATCOM_CALL(fn, (long)t->v, t->n, b->x, b->y);
}

void weather_2B1BC0(TerrBin *b, void *fn)
{
    uchar fl = G6B_BIN_FL(b);

    if (fl & 2) {
        if (G6B_BIN_FL(b) & 1) {
            draw_tri(fn, G6B_BIN_B(b), b);
            draw_tri(fn, G6B_BIN_A(b), b);
        } else
            draw_tri(fn, G6B_BIN_B(b), b);
        probe_check();
        if (b->list)
            viewdist_2A250C(b, 3);
        return;
    }
    if (!(fl & 1)) {
        if (b->list)
            viewdist_2A250C(b, 3);
        return;
    }
    if (!b->list) {
        draw_tri(fn, G6B_BIN_A(b), b);
        probe_check();
        return;
    }
    if (fl & 4) {
        viewdist_2A250C(b, 1);
        draw_tri(fn, G6B_BIN_A(b), b);
        probe_check();
        viewdist_2A250C(b, 2);
    } else {
        viewdist_2A250C(b, 2);
        draw_tri(fn, G6B_BIN_A(b), b);
        probe_check();
        viewdist_2A250C(b, 1);
    }
}

void weather_2B1DCC(void)
{
    long vx = ff_view.x >> 16, vy = ff_view.y >> 16;
    TerrBin *b = G6B_BINS + strip_count - 1;
    long band = 2, limit = wx_band_dist[2], cur = -1, i;
    uchar textured = ff_cam.g_5c == 1 || ff_cam.g_5c == 2;
    void *fn = 0;

    for (i = strip_count - 1; i >= 0; i--, b--) {
        if (game_35CADA != 0xFF) {
            while (fix_fast_pyth_dist(vx - b->x, vy - b->y) < limit) {
                weather_2AFA5C(ffpipe_41EB60, band);
                band--;
                limit = band >= 0 ? wx_band_dist[band] : -1;
            }
        }
        if (b->band != cur) {
            cur = b->band;
            fn = terr_poly_fns[G6B_BAND_MODE(cur)];
        }
        if (textured)
            weather_2B12D0(b);
        weather_2B1BC0(b, fn);
    }
}

void weather_2B1F24(void)
{
    static const uchar rows[9] = { 1, 6, 4, 3, 2, 0, 7, 5, 8 };
    uchar textured = ff_cam.g_5c == 1 || ff_cam.g_5c == 2;
    long *ord, k;
    void *fn = 0;

    if (wx_region > 8)
        return;
    ord = wx_draw_order[rows[wx_region]];
    for (k = 0; k < 9; k++) {
        TerrBin *b = wx_bins[ord[k]];
        long cur = -1;
        if (!b)
            continue;
        if (ord[k] != 4) {
            for (; b; b = G6B_BIN_NEXT(b)) {
                if (b->band != cur) {
                    cur = b->band;
                    fn = terr_poly_fns[G6B_BAND_MODE(cur)];
                }
                if (textured)
                    weather_2B12D0(b);
                weather_2B1BC0(b, fn);
            }
            continue;
        }
        wx_in_center = 1;
        for (; b; b = G6B_BIN_NEXT(b)) {
            uchar fl;
            if (b->band != cur) {
                cur = b->band;
                fn = terr_poly_fns[G6B_BAND_MODE(cur)];
                if (wx_grid_mode)
                    fn = WATCOM_FN(weather_2B2690);
            }
            if (textured)
                weather_2B12D0(b);
            fl = G6B_BIN_FL(b);
            if (fl & 2) {
                if (fl & 1) {
                    draw_tri(fn, G6B_BIN_B(b), b);
                    draw_tri(fn, G6B_BIN_A(b), b);
                } else
                    draw_tri(fn, G6B_BIN_B(b), b);
                probe_check();
            } else if (fl & 1) {
                draw_tri(fn, G6B_BIN_A(b), b);
                probe_check();
            }
        }
        if (*(long *)(strip_427DE0 - 0x40))
            viewdist_2A250C(strip_427DE0 - 0x40, 3);
        wx_in_center = 0;
    }
}

/* ------------------------------------------------------------------ polygon routines */

#ifndef TN_WATCOM_ABI
#include "mapedge.h"
#define CELL(x, y) PF_CELL(x, y)        /* the map and the land around it (port/video/mapedge.c, G35) */
#else
#define CELL(x, y) ((uchar *)TN_LOWMEM(GMAP->rows2[y]) + (x) * 3)
#endif

uchar weather_2B219C(TrVert **pts, long n, long x, long y)
{
    long t = CELL(x, y)[0] & 0x3F, i;
    uchar probe;
    struct { long top; uchar **rows; long *offs; } refl;

    if (game_41DA28[t >> 3] & (1 << (t & 7))) {
        if (!((1UL << (game_41DD58[t] & 31)) & terrain_bitmask))
            return 0;
    } else {
        uchar k = terrain_kindtab[t][0];
        if (!((1UL << (k & 0xF)) & terrain_bitmask) && !((1UL << (k >> 4)) & terrain_bitmask))
            return 0;
    }
    probe = ff_cam.probe && *ff_cam.probe_ptr == 0xFF;
    if (probe)
        *ff_cam.probe_ptr = ff_cam.probe_save;
    terr_tex_bm.bits = planet_tex[CELL(x, y)[0] & 0x3F];
    for (i = 0; i < n; i++)
        pts[i]->b = fix_div(0x10000, pts[i]->z);
    refl.top = sky_refl_top;
#ifndef TN_WATCOM_ABI
    refl.rows = port_sky_refl_row;
#else
    refl.rows = sky_refl_row;
#endif
    refl.offs = sky_refl_off;
    crt_2FB3A0(&terr_tex_bm, n, pts, &refl);
    if (probe) {
        ff_cam.probe_save = *ff_cam.probe_ptr;
        *ff_cam.probe_ptr = 0xFF;
    }
    return 1;
}

static uchar *cell_tex(long x, long y)
{
    long t = CELL(x, y)[0] & 0x7F;
    return t & 0x40 ? BlastFindTmap(t & 0x3F) : planet_tex[t];
}

#ifndef TN_WATCOM_ABI
/* the effects pack's BlastMarks (port/mods/marks.c) on a scorched square in the sharp view, after
 * cell_tex (the game's copy is made as always): its 64 x 64 texture into terr_tex_bm (0), or on
 * a polygon more than 96 of the screen's pixels across its 256 x 256 one, drawn here by the
 * canvas's mapper k with u, v times 4 (1) */
static uchar pack_mark(TrVert **pts, long n, long x, long y, long k)
{
    long t = CELL(x, y)[0] & 0x7F, i;
    fix x0, x1, y0, y1;
    grs_bitmap bm;
    uchar *p;
    int big;
    if (!(t & 0x40) || !port_s3d_on || n < 3)
        return 0;
    x0 = x1 = ((fix *)pts[0])[0];
    y0 = y1 = ((fix *)pts[0])[1];
    for (i = 1; i < n; i++) {
        fix *v = (fix *)pts[i];
        x0 = v[0] < x0 ? v[0] : x0;
        x1 = v[0] > x1 ? v[0] : x1;
        y0 = v[1] < y0 ? v[1] : y0;
        y1 = v[1] > y1 ? v[1] : y1;
    }
    big = x1 - x0 > 96 << 16 || y1 - y0 > 96 << 16;
    p = port_marks_tex(t & 0x3F, big);
    if (!p && big)
        p = port_marks_tex(t & 0x3F, big = 0);
    if (!p)
        return 0;
    if (!big) {
        terr_tex_bm.bits = p;
        return 0;
    }
    bm = terr_tex_bm;
    bm.bits = p;
    bm.w = bm.h = 256;
    bm.row = 256;
    bm.wlog = bm.hlog = 8;
    for (i = 0; i < n; i++) {
        pts[i]->r <<= 2;
        pts[i]->g <<= 2;
    }
    WATCOM_CALL(grd_canvas_table[k + bm.type * 2], (long)&bm, n, (long)pts);
    for (i = 0; i < n; i++) {
        pts[i]->r >>= 2;
        pts[i]->g >>= 2;
    }
    return 1;
}
#endif

#ifndef TN_WATCOM_ABI
/* the GPU ground (port/video/gpuland.c): the polygon as the routine draws it, caught in the main
 * view's sharp drawing; tex its 64 x 64 texture (NULL: flat in colour), lt its light table */
static void gl_catch(TrVert **pts, long n, const uchar *tex, int colour, const uchar *lt)
{
    PortGlVert v[16];
    long i;
    if (!port_gl_capturing || !port_s3d_on || n < 3 || n > 16 || grd_canvas->gc.fill_type)
        return;                         /* (the infrared: the game's own) */
    if (gl_haze_lt && lt)               /* the card fogs every pixel itself */
        lt = gl_haze_lt;
    for (i = 0; i < n; i++) {
        v[i].x = pts[i]->x / 65536.0f;
        v[i].y = pts[i]->y / 65536.0f;
        v[i].z = pts[i]->z / 65536.0f;
        v[i].u = pts[i]->r / 65536.0f;
        v[i].v = pts[i]->g / 65536.0f;
        v[i].shade = pts[i]->shade / 65536.0f;
    }
    port_gl_poly(v, (int)n, tex, colour, lt);
}
#define GL_CATCH(pts, n, tex, colour, lt) gl_catch(pts, n, tex, colour, lt)
#define GL_SHADOW(on) (port_gl_capturing ? port_gl_shadow(on) : (void)0)
#else
#define GL_CATCH(pts, n, tex, colour, lt) ((void)0)
#define GL_SHADOW(on) ((void)0)
#endif

static uchar sky_first(TrVert **pts, long n, long x, long y)
{
    return !game_41DE34 && ffpipe_35C100 && !grd_canvas->gc.fill_type
        && weather_2B219C(pts, n, x, y);
}

uchar weather_2B2340(TrVert **pts, long n, long x, long y)
{
    long i;
    if (sky_first(pts, n, x, y))
        return 1;
    terr_tex_bm.bits = cell_tex(x, y);
    for (i = 0; i < n; i++)
        pts[i]->b = fix_div(0x10000, pts[i]->z);
#ifndef TN_WATCOM_ABI
    if (pack_mark(pts, n, x, y, 0x68)) {
        GL_CATCH(pts, n, terr_tex_bm.bits, 0, grd_screen->ltab);
        return 1;
    }
#endif
    GL_CATCH(pts, n, terr_tex_bm.bits, 0, grd_screen->ltab);
    GL_SHADOW(1);
    WATCOM_CALL(grd_canvas_table[0x68 + terr_tex_bm.type * 2], (long)&terr_tex_bm, n, (long)pts);
    GL_SHADOW(0);
    return 1;
}

uchar weather_2B2418(TrVert **pts, long n, long x, long y)
{
    if (sky_first(pts, n, x, y))
        return 1;
    terr_tex_bm.bits = cell_tex(x, y);
#ifndef TN_WATCOM_ABI
    if (pack_mark(pts, n, x, y, 0x42)) {
        GL_CATCH(pts, n, terr_tex_bm.bits, 0, grd_screen->ltab);
        return 1;
    }
#endif
    GL_CATCH(pts, n, terr_tex_bm.bits, 0, grd_screen->ltab);
    GL_SHADOW(1);
    WATCOM_CALL(grd_canvas_table[0x42 + terr_tex_bm.type * 2], (long)&terr_tex_bm, n, (long)pts);
    GL_SHADOW(0);
    return 1;
}

uchar weather_2B24C0(TrVert **pts, long n, long x, long y)
{
    uchar c = planet_res2c[*(ushort *)CELL(x, y) & 0x3FF];
    GL_CATCH(pts, n, NULL, c, NULL);
    WATCOM_CALL(grd_canvas_table[25], c, n, (long)pts);
    return 1;
}

uchar weather_2B250C(TrVert **pts, long n)
{
    long i;
    WATCOM_CALL(grd_canvas_table[25], 0, n, (long)pts);
    for (i = 0; i < n; i++) {
        fix *v = (fix *)pts[i];
        if (((long)grd_canvas->bm.w << 16) == v[0])
            v[0] -= 0x10000;
        if (((long)grd_canvas->bm.h << 16) == v[1])
            v[1] -= 0x10000;
    }
    gr_wire_upoly(0x1D, n, (grs_vertex **)pts);
    return 1;
}

uchar weather_2B2584(TrVert **pts, long n)
{
    /* the original passes its caller's edx (n) on unchanged */
    WATCOM_CALL(grd_canvas_table[25], 0, n, (long)pts);
    return 1;
}

uchar weather_2B2598(TrVert **pts, long n, long x, long y)
{
    long k = ((x ^ y) >> 1) & 0xF, i;
    long a = wx_rows[k] << 16;
    long d = (wx_rows[k + 1] - wx_rows[k] - 1) << 16;

    pts[0]->shade = a;
    pts[1]->shade = d / 2 + a;
    pts[2]->shade = d + a;
    for (i = 3; i < n; i++)
        pts[i]->shade = a;
    WATCOM_CALL(grd_canvas_table[28], 0, n, (long)pts);
    return 1;
}

long weather_2B2610(long x, long y)
{
    short first = (short)game_entities[targ_current].f05, i = first;

    if (!first)
        return 0;
    do {
        GridSquare *g = &grid_squares[i];
        if (g->x == x && g->y == y)
            return g->e0 ? 0x1D : 0x44;
        i = g->ent_next;
    } while (i != first);
    return 0;
}

uchar weather_2B2690(TrVert **pts, long n, long x, long y)
{
    long c = (x ^ y) & 0xFF;
    if (game_39A404 && targ_current) {
        long r = weather_2B2610(x, y);
        if (r)
            c = r;
    }
    WATCOM_CALL(grd_canvas_table[25], c, n, (long)pts);
    return 1;
}
