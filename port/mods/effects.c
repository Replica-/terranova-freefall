/* effects.c -- Freefall: "Effects pack" on the Mods page: the game's effect animations
 * (explosions, smoke, impacts) drawn with a mod pack's pictures, in the sharp 3D view
 * (fxpack.c finds and loads the pack); and "Objects pack": the trees', bushes' and rocks' pictures
 * (port_obj_frame).
 *
 *  - Colours: each pixel takes the nearest colour of the palette in use (0 and the see-through
 *    and cycling colours 0xF8..0xFF left out), made again for a picture when the palette has
 *    changed since; then through the sprite's light table (or the infrared's colour table) as
 *    the game's own frames.
 *  - See-through: by the picture's alpha: under 16 not drawn, 240 and over solid, in between
 *    mixed with what is behind it that much (port_fx_blit, the nearest palette colour of the mix,
 *    in the infrared a step along its ramp), so soft smoke is soft without dots.
 *  - Timing: the game runs its animation as always (it ends the effect; nothing in the game
 *    depends on the pictures).  The pack's pictures are spread over the frames resource by the
 *    frame shown and the time into it at the animations' last run (ActAnim last_time,
 *    next_time, frame_time), so 30 pictures for the
 *    game's 15 show one between each two of the game's, and any other count is spread over the
 *    same time.
 *  - Size and place: the game sizes a sprite by its width in the world and keeps the bitmap's
 *    proportions (drawobj_29BE78), so a picture 2 or 3 times the original's size takes the same
 *    room; the anchor is the original frame's (+0x10), scaled to the picture.
 *  - Only the sharp view draws them (port_s3d_wanted turns it on for a pack, at 3D resolution
 *    Original too, at the game's size), so the game's own drawing, and whatever the game takes
 *    from it, stays the original's.
 *  - What died: the game's fireball of every death (1171) shows the pack's pictures of another
 *    animation, when the pack has it, by what died (port_fx_death, from sfx.c): a tree or bush
 *    1183, a rock 1192, a soldier 1184 (1.25 times as big), a vehicle or a gun turret 1167 (1.5
 *    times).  A tree's or bush's pictures are as tall as the plant was drawn (with "Tree size").
 *    They take the fireball's time and place; the game's effect is the same.
 *  - "Tree explosion" (burn.c) draws a plant's death itself, on the plant's own picture: its
 *    fireball is not drawn then (port_burn_hides), and the pack's pictures it uses come from
 *    here (port_fx_pic, port_fx_draw, port_fx_near, port_fx_mix).
 */
#include <math.h>
#include <string.h>
#include "2d.h"
#include "res.h"
#include "support.h"
#include "t2_b.h"
#include "port.h"
#include "fxpack.h"
#include "gpuland.h"

extern int port_s3d_on;                 /* sharp3d.c: drawing the sharp view */

static FxPic *fx_cur;                   /* the picture port_fx_frame gave last */

/* what died, by effect object (game_cls6 slot, PORT_CLS6_N in game.h): its fireball's animation,
 * the kind (DK_), and a plant's height in the world and place (its "Tree size") */
enum { DK_NONE, DK_PLANT, DK_ROCK, DK_SOLDIER, DK_VEHICLE, DK_N };
static const ushort dead_res[DK_N] = { 0, 1183, 1192, 1184, 1167 };
static struct { void *anim; int kind; fix h, x, y; } dead[0x100];

/* the frames of 1160..1163: width and height (pixels) and which are rocks */
static const uchar plant_wh[4][14][2] = {
    { { 141, 160 }, { 98, 98 }, { 98, 84 }, { 50, 45 }, { 50, 50 }, { 72, 70 }, { 86, 66 }, { 83, 91 },
      { 45, 39 }, { 44, 34 }, { 36, 26 }, { 68, 36 }, { 97, 88 }, { 97, 31 } },
    { { 94, 92 }, { 93, 92 }, { 96, 81 }, { 65, 64 }, { 93, 53 }, { 198, 58 }, { 207, 69 }, { 179, 65 },
      { 94, 50 }, { 93, 36 }, { 76, 47 }, { 83, 51 }, { 72, 32 } },
    { { 87, 93 }, { 125, 124 }, { 97, 52 }, { 103, 55 } },
    { { 77, 54 }, { 101, 82 }, { 87, 71 }, { 69, 53 } },
};
static const ushort rock_frames[4] = { 0x5E0, 0xE0, 0xF, 0xF };     /* rocks, stones, grassy mounds */
fix port_tree_scale(long ref, fix x, fix y, fix scale);    /* "Tree size" (sharp3d.c) */
void port_burn_start(long slot, void *anim, long type, long ref, fix w, fix x, fix y, fix z, long ent,
                     int broken);       /* burn.c */
int port_burn_hides(long slot, void *anim);

/* sfx.c: the effect object slot was made (anim: its animation, NULL none) for the death of an
 * object of class cls (or -1: not a death), the entity ent; for class 0 ref is its model and w
 * its width in the world, type its type and (x, y, z) its place, broken 1 a plant that broke (the
 * game keeps it, as its broken picture) or 2 whose broken picture went (ref: that picture, a
 * plant's death whatever it looks like); armed an installation with a gun.  Read in game time;
 * nothing the game keeps changes. */
void port_fx_death(long slot, void *anim, long cls, long type, long ref, fix w, fix x, fix y, fix z, int armed,
                   long ent, int broken)
{
    ulong id = (ulong)ref >> 16, k = REFINDEX((ulong)ref);
    int kind = DK_NONE;

    if (slot < 0 || slot >= 0x100)
        return;
    if (cls == 0 && id >= 1160 && id <= 1163 && k < 14 && plant_wh[id - 1160][k][0] && w > 0) {
        kind = !broken && rock_frames[id - 1160] >> k & 1 ? DK_ROCK : DK_PLANT;
        dead[slot].h = (fix)((long long)w * plant_wh[id - 1160][k][1] / plant_wh[id - 1160][k][0]);
        dead[slot].x = x;
        dead[slot].y = y;
    } else if (cls == 1)
        kind = DK_SOLDIER;
    else if (cls == 2 || (cls == 4 && armed))
        kind = DK_VEHICLE;
    dead[slot].anim = anim;
    dead[slot].kind = kind;
    if (kind == DK_PLANT)               /* "Tree explosion" (burn.c) */
        port_burn_start(slot, anim, type, ref, w, x, y, z, ent, broken);
}

/* the palette's key: its colours but 0 and 0xF8..0xFF (the cycling ones change every few frames) */
static ulong pal_key(void)
{
    ulong k = 2166136261u;
    int c;
    if (!grd_bpal)
        return 0;
    for (c = 1; c < 0xF8; c++)
        k = (k ^ grd_bpal[c]) * 16777619u;
    return k | 1;
}

/* the picture in the palette's colours: the nearest one for each colour, looked up once a colour
 * per palette (most pictures have a few hundred; P108: a pack's picture may have thousands, so
 * the table starts again when it is three quarters full, and the probe always finds a free slot) */
#define CACHE_SIZE 4096
#define CACHE_FULL (CACHE_SIZE * 3 / 4)
static void pic_convert(FxPic *p, ulong key)
{
    static ulong cache_rgb[CACHE_SIZE]; /* rgb | 1 << 24, 0 = free */
    static uchar cache_c[CACHE_SIZE];
    static ulong cache_pal;
    static int cache_n;                 /* slots in use */
    long pr[256], pg[256], pb[256];
    int i, c, n = p->w * p->h;

    if (cache_pal != key) {
        memset(cache_rgb, 0, sizeof cache_rgb);
        cache_n = 0;
        cache_pal = key;
    }
    for (c = 1; c < 0xF8; c++) {
        grs_rgb w = grd_bpal[c];
        pr[c] = (long)(w & 0x3FF) >> 2;
        pg[c] = (long)(w >> 13) & 0xFF;
        pb[c] = (long)(w >> 24) & 0xFF;
    }
    for (i = 0; i < n; i++) {
        const uchar *s = p->rgba + i * 4;
        ulong rgb = s[0] | s[1] << 8 | (ulong)s[2] << 16 | 1ul << 24, h;
        if (s[3] < 16) {
            p->pix[i] = 0;
            continue;
        }
        for (h = ((rgb * 2654435761u) >> 20) & (CACHE_SIZE - 1); cache_rgb[h] && cache_rgb[h] != rgb;
             h = (h + 1) & (CACHE_SIZE - 1))
            ;
        if (!cache_rgb[h]) {
            long best = 0x7FFFFFFF;
            int bc = 1;
            if (cache_n >= CACHE_FULL) {    /* P108: start again (the probe ran forever once full) */
                memset(cache_rgb, 0, sizeof cache_rgb);
                cache_n = 0;
                h = ((rgb * 2654435761u) >> 20) & (CACHE_SIZE - 1);
            }
            for (c = 1; c < 0xF8; c++) {
                long dr = pr[c] - s[0], dg = pg[c] - s[1], db = pb[c] - s[2];
                long d = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
                if (d < best) {
                    best = d;
                    bc = c;
                }
            }
            cache_rgb[h] = rgb;
            cache_c[h] = (uchar)bc;
            cache_n++;
        }
        p->pix[i] = cache_c[h];
    }
    p->pal = key;
}

/* drawobj_29AF98, in the sharp view: the pack's picture for the animation anim (an ActAnim) of
 * the effect object slot showing the frame ref, whose bitmap is orig, as a frame in hdr (a
 * grs_bitmap and the anchor at +0x10, as the game's frames), and its width in the world (scale,
 * the frame's); 0 when the pack has none, -1 when nothing is to be drawn (a burning plant's) */
int port_fx_frame(void *anim, long slot, long ref, uchar *orig, uchar *hdr, fix *scale)
{
    ActAnim *pa = anim;
    grs_bitmap *ob = (grs_bitmap *)orig, *b = (grs_bitmap *)hdr;
    FxAnim *a = NULL;
    FxPic *p;
    long item = REFINDEX(ref), nref, k, f = 0;
    int kind = 0;
    ulong key;

    if (pa && port_burn_hides(slot, anim))
        return -1;                      /* "Tree explosion" draws this death (burn.c) */
    if (!port_fx_loaded() || !pa || ob->w <= 0 || ob->h <= 0)
        return 0;
    if (slot >= 0 && slot < 0x100 && dead[slot].anim == anim && dead[slot].kind && REFID((ulong)ref) == 1171 &&
        (a = port_fx_anim(dead_res[dead[slot].kind])) != NULL)
        kind = dead[slot].kind;         /* what died */
    if (!a)
        a = port_fx_anim((ushort)REFID((ulong)ref));
    if (!a)
        return 0;
    nref = ResNumRefs((Id)REFID((ulong)ref));   /* locked by the caller: read from memory */
    if (nref <= 0)
        return 0;
    if (pa->frame >= 0 && pa->frame_time > 0) {     /* the time into the frame, 0..0xFFFF */
        /* by the animations' last run (last_time): the game's clock has moved on by a frame
         * when it draws, which would put every first picture of a frame half a frame late */
        long into = (long)(pa->last_time - (pa->next_time - (ulong)(long)pa->frame_time));
        if (into > 0)
            f = into >= pa->frame_time ? 0xFFFF : (into << 16) / pa->frame_time;
    }
    k = (long)((((long long)item << 16) + f) * a->n / ((long long)nref << 16));
    if (k < 0)
        k = 0;
    if (k >= a->n)
        k = a->n - 1;
    p = &a->pic[k];
    key = pal_key();
    if (!key)
        return 0;
    if (p->pal != key)
        pic_convert(p, key);
    memset(hdr, 0, 0x1C);
    b->bits = p->pix;
    b->type = ob->flags & BMF_TLUC8 ? BMT_TLUC8 : BMT_FLAT8;
    b->flags = ob->flags | BMF_TRANS;
    b->w = (short)p->w;
    b->h = (short)p->h;
    b->row = (ushort)p->w;
    *(short *)(hdr + 0x10) = (short)(*(short *)(orig + 0x10) * p->w / ob->w);
    *(short *)(hdr + 0x12) = (short)(*(short *)(orig + 0x12) * p->h / ob->h);
    if (kind == DK_PLANT) {             /* as tall as the plant was drawn */
        fix h = port_tree_scale(1160L << 16, dead[slot].x, dead[slot].y, dead[slot].h);
        *scale = (fix)((long long)h * p->w / p->h);
    } else if (kind == DK_VEHICLE)
        *scale += *scale / 2;
    else if (kind == DK_SOLDIER)
        *scale += *scale / 4;
    fx_cur = p;
    return 1;
}

/* "Objects pack" (fxpack.c), drawobj_29AF98 in the sharp view: the pack's picture for the frame
 * ref, whose bitmap is orig, as a frame in hdr (as port_fx_frame); 0 when the pack has none.  Its
 * colour numbers are the palette's, drawn through the light table (or the infrared's colour
 * table) as the game's own frame, see-through where 0; the anchor scaled to the picture */
int port_obj_frame(long ref, uchar *orig, uchar *hdr)
{
    grs_bitmap *ob = (grs_bitmap *)orig, *b = (grs_bitmap *)hdr;
    long k = REFINDEX(ref);
    FxAnim *a;
    FxPic *p;

    if (!port_obj_loaded() || ob->w <= 0 || ob->h <= 0)
        return 0;
    a = port_obj_set((ushort)REFID((ulong)ref));
    if (!a || k < 0 || k >= a->n || !a->pic[k].pix)
        return 0;
    p = &a->pic[k];
    memset(hdr, 0, 0x1C);
    b->bits = p->pix;
    b->type = ob->flags & BMF_TLUC8 ? BMT_TLUC8 : BMT_FLAT8;
    b->flags = ob->flags | BMF_TRANS;
    b->w = (short)p->w;
    b->h = (short)p->h;
    b->row = (ushort)p->w;
    *(short *)(hdr + 0x10) = (short)(*(short *)(orig + 0x10) * p->w / ob->w);
    *(short *)(hdr + 0x12) = (short)(*(short *)(orig + 0x12) * p->h / ob->h);
    return 1;
}

/* the mixes: the palette's colours, and the nearest one of each of 32 x 32 x 32 colours, found
 * when first needed (0 not yet) for the palette of key, nearest as the eye sees it (CIE Lab: a
 * grey smoke over the blue sky stays a blue-grey, not the palette's nearer teal) */
static long mx_r[256], mx_g[256], mx_b[256];
static float mx_lab[256][3];
static uchar mx_cube[32 * 32 * 32];
static ulong mx_key;

static float lin(long v)
{
    float u = (float)v / 255.0f;
    return u > 0.04045f ? powf((u + 0.055f) / 1.055f, 2.4f) : u / 12.92f;
}

static float lab_f(float t)
{
    return t > 0.008856f ? cbrtf(t) : 7.787f * t + 16.0f / 116.0f;
}

static void lab(long r, long g, long b, float *o)
{
    float R = lin(r), G = lin(g), B = lin(b);
    float x = lab_f((0.4124f * R + 0.3576f * G + 0.1805f * B) / 0.95047f), y = lab_f(0.2126f * R + 0.7152f * G + 0.0722f * B),
          z = lab_f((0.0193f * R + 0.1192f * G + 0.9505f * B) / 1.08883f);
    o[0] = 116.0f * y - 16.0f;
    o[1] = 500.0f * (x - y);
    o[2] = 200.0f * (y - z);
}

static uchar mix_colour(long r, long g, long b)
{
    long k = (r >> 3) << 10 | (g >> 3) << 5 | b >> 3, c;
    if (!mx_cube[k]) {
        float o[3], best = 1e30f;
        lab((r & ~7) + 4, (g & ~7) + 4, (b & ~7) + 4, o);
        mx_cube[k] = 1;
        for (c = 1; c < 0xF8; c++) {
            float d0 = mx_lab[c][0] - o[0], d1 = mx_lab[c][1] - o[1], d2 = mx_lab[c][2] - o[2],
                  d = d0 * d0 + d1 * d1 + d2 * d2;
            if (d < best) {
                best = d;
                mx_cube[k] = (uchar)c;
            }
        }
    }
    return mx_cube[k];
}

/* the mixes' tables for the palette in use (in the infrared the ramp's steps need none);
 * 1 in the infrared */
static int mix_ir;

static void mix_ready(void)
{
    ulong key;
    mix_ir = grd_canvas->gc.fill_type == 1;
    key = pal_key();
    if (!mix_ir && key != mx_key) {
        int c;
        for (c = 0; c < 256; c++) {
            grs_rgb v = grd_bpal[c];
            mx_r[c] = (long)(v & 0x3FF) >> 2;
            mx_g[c] = (long)(v >> 13) & 0xFF;
            mx_b[c] = (long)(v >> 24) & 0xFF;
            lab(mx_r[c], mx_g[c], mx_b[c], mx_lab[c]);
        }
        memset(mx_cube, 0, sizeof mx_cube);
        mx_key = key;
    }
}

/* the screen's colour o mixed with c by a (0..255): the nearest palette colour of the mix, in
 * the infrared a step along its ramp (infrared.c) */
static uchar mix(long o, long c, long a)
{
    if (mix_ir)
        return (uchar)(c >= 0xB0 && c <= 0xE6 && o >= 0xB0 && o <= 0xE6 ? o + ((c - o) * a + (c > o ? 127 : -127)) / 255 :
                       a >= 128 ? c : o);
    return mix_colour(mx_r[o] + (mx_r[c] - mx_r[o]) * a / 255, mx_g[o] + (mx_g[c] - mx_g[o]) * a / 255,
                      mx_b[o] + (mx_b[c] - mx_b[o]) * a / 255);
}

/* GPU rendering (port/video/gpuland.c): the picture handed to the card in its own colours and alpha,
 * the light row clut as colour * a + b per channel (least squares over the picture's colours, as
 * many times as it has each), and the canvas's pixels it would cover (xa .. xb, ya .. yb: alpha 16
 * or more) noted for it instead of drawn; 0 when the card does not take it */
static int gl_draw(const FxPic *p, long x0, long y0, long w, long h, const uchar *clut, long op, int mirror,
                   long xa, long xb, long ya, long yb)
{
    static long cnt[256];
    float fit[6];
    grs_canvas *cv = grd_canvas;
    long x, y, row = cv->bm.row, c, k;
    if (clut) {
        memset(cnt, 0, sizeof cnt);
        for (c = 0; c < (long)p->w * p->h; c++)
            if (p->rgba[c * 4 + 3] >= 16)
                cnt[p->pix[c]]++;
        for (k = 0; k < 3; k++) {
            const long *ch = k == 0 ? mx_r : k == 1 ? mx_g : mx_b;
            double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0, a, b;
            for (c = 0; c < 256; c++) {
                double wt = cnt[c] + 0.01, X = ch[c] / 255.0, Y = ch[clut[c]] / 255.0;
                sw += wt;
                sx += wt * X;
                sy += wt * Y;
                sxx += wt * X * X;
                sxy += wt * X * Y;
            }
            a = sxx * sw - sx * sx > 1e-12 ? (sxy * sw - sx * sy) / (sxx * sw - sx * sx) : 0;
            b = (sy - a * sx) / sw;
            fit[k] = (float)a;
            fit[3 + k] = (float)b;
        }
    }
    if (!port_gl_fx(p->rgba, p->w, p->h, p, (float)x0, (float)y0, (float)(x0 + w), (float)(y0 + h), mirror,
                    op >= 256 ? 1.0f : op / 256.0f, clut ? fit : NULL))
        return 0;
    for (y = ya; y < yb; y++) {         /* the pixels it covers, as draw_alpha would draw them */
        long sy = (2 * (y - y0) + 1) * p->h / (2 * h);
        const uchar *A = p->rgba + sy * p->w * 4 + 3;
        uchar *d = cv->bm.bits + y * row;
        for (x = xa; x < xb; x++) {
            long i = (2 * (x - x0) + 1) * p->w / (2 * w);
            if (mirror)
                i = p->w - 1 - i;
            if ((op >= 256 ? A[i * 4] : A[i * 4] * op >> 8) >= 16)
                port_gl_fxsh[(d + x) - port_gl_sh_base] = (uint16_t)(0x100 | d[x]);
        }
    }
    return 1;
}

/* the picture p scaled to w x h at (x0, y0) on the canvas (mirror: turned left for right), each
 * pixel by its alpha times op / 256, through clut (NULL none) */
static void draw_alpha(const FxPic *p, long x0, long y0, long w, long h, const uchar *clut, long op, int mirror)
{
    static long sx[4096];
    grs_canvas *cv = grd_canvas;
    long xa, xb, ya, yb, x, y, row = cv->bm.row;
    xa = x0 > cv->gc.clip.i.left ? x0 : cv->gc.clip.i.left;
    xb = x0 + w < cv->gc.clip.i.right ? x0 + w : cv->gc.clip.i.right;
    ya = y0 > cv->gc.clip.i.top ? y0 : cv->gc.clip.i.top;
    yb = y0 + h < cv->gc.clip.i.bot ? y0 + h : cv->gc.clip.i.bot;
    if (xa >= xb || ya >= yb || xb - xa > 4096 || op <= 0)
        return;
    mix_ready();
    if (port_gl_capturing && port_s3d_on && p->rgba && !cv->gc.fill_type && port_gl_obj_canvas(cv->bm.bits) &&
        gl_draw(p, x0, y0, w, h, clut, op, mirror, xa, xb, ya, yb))
        return;                         /* GPU rendering draws it, in its own colours (gpuland.c) */
    for (x = xa; x < xb; x++) {
        sx[x - xa] = (2 * (x - x0) + 1) * p->w / (2 * w);
        if (mirror)
            sx[x - xa] = p->w - 1 - sx[x - xa];
    }
    for (y = ya; y < yb; y++) {
        long sy = (2 * (y - y0) + 1) * p->h / (2 * h), li = -1, la = 0, lc = 0;
        const uchar *s = p->pix + sy * p->w, *A = p->rgba + sy * p->w * 4 + 3;
        uchar *d = cv->bm.bits + y * row;
        int lo = -1;
        uchar lr = 0;
        for (x = xa; x < xb; x++) {
            long i = sx[x - xa], a, c;
            if (i != li) {              /* a new picture pixel (a scaled-up one covers many) */
                li = i;
                la = op >= 256 ? A[i * 4] : A[i * 4] * op >> 8;
                lc = clut ? clut[s[i]] : s[i];
                lo = -1;
            }
            a = la;
            c = lc;
            if (a < 16)
                continue;
            if (a >= 240)
                d[x] = (uchar)c;
            else if (d[x] == lo)        /* the same screen colour under it: the same mix */
                d[x] = lr;
            else {
                lo = d[x];
                d[x] = lr = mix(lo, c, a);
            }
        }
    }
}

/* drawobj_29BE78, in the sharp view: the picture whose pixels are bits (the one port_fx_frame
 * gave) scaled to w x h at (x0, y0) on the canvas, each pixel by its alpha, through clut (NULL
 * none); 0 when it is not that picture */
int port_fx_blit(const uchar *bits, long x0, long y0, long w, long h, const uchar *clut)
{
    FxPic *p = fx_cur;
    if (!p || p->pix != bits || w <= 0 || h <= 0 || w > 0x7FFF || h > 0x7FFF)
        return 0;
    draw_alpha(p, x0, y0, w, h, clut, 256, 0);
    return 1;
}

/* ------------------------------------------------------------------ for burn.c */

/* the loaded effects pack's picture k (0 .. *n - 1) of res in the palette's colours, or NULL */
const FxPic *port_fx_pic(unsigned short res, long k, int *n)
{
    FxAnim *a = port_fx_loaded() ? port_fx_anim(res) : NULL;
    ulong key = pal_key();
    FxPic *p;
    if (n)
        *n = a ? a->n : 0;
    if (!a || a->n <= 0 || !key)
        return NULL;
    p = &a->pic[k < 0 ? 0 : k >= a->n ? a->n - 1 : k];
    if (p->pal != key)
        pic_convert(p, key);
    return p;
}

/* p (port_fx_pic's) scaled to w x h at (x0, y0), by its alpha times op / 256, through clut
 * (port_fx_draw_m: mirrored if mirror) */
void port_fx_draw_m(const FxPic *p, long x0, long y0, long w, long h, const uchar *clut, long op, int mirror)
{
    if (p && w > 0 && h > 0 && w <= 0x7FFF && h <= 0x7FFF)
        draw_alpha(p, x0, y0, w, h, clut, op, mirror);
}

void port_fx_draw(const FxPic *p, long x0, long y0, long w, long h, const uchar *clut, long op)
{
    port_fx_draw_m(p, x0, y0, w, h, clut, op, 0);
}

/* the game's frame k of a plant's resource (1160..1163, ref): its width and height; 0 if none */
int port_plant_size(long ref, int *w, int *h)
{
    ulong id = (ulong)ref >> 16, k = REFINDEX((ulong)ref);
    if (id < 1160 || id > 1163 || k >= 14 || !plant_wh[id - 1160][k][0])
        return 0;
    *w = plant_wh[id - 1160][k][0];
    *h = plant_wh[id - 1160][k][1];
    return 1;
}

/* the palette's key (0: none yet), the RGB of its colour c, the nearest colour to (r, g, b) as
 * the eye sees it, and the mix of the screen's colour o with c by a (0..255); port_fx_mix_begin
 * first, for the canvas and palette in use */
ulong port_fx_pal(void)
{
    return pal_key();
}

void port_fx_mix_begin(void)
{
    mix_ready();
}

void port_fx_rgb(int c, long *r, long *g, long *b)
{
    grs_rgb v = grd_bpal[c & 0xFF];
    *r = (long)(v & 0x3FF) >> 2;
    *g = (long)(v >> 13) & 0xFF;
    *b = (long)(v >> 24) & 0xFF;
}

uchar port_fx_near(long r, long g, long b)
{
    int ir = mix_ir;
    uchar c;
    mix_ir = 0;
    if (pal_key() != mx_key) {          /* in the infrared too: the colours are the palette's */
        int fill = grd_canvas->gc.fill_type;
        grd_canvas->gc.fill_type = 0;
        mix_ready();
        grd_canvas->gc.fill_type = fill;
    }
    c = mix_colour(r < 0 ? 0 : r > 255 ? 255 : r, g < 0 ? 0 : g > 255 ? 255 : g, b < 0 ? 0 : b > 255 ? 255 : b);
    mix_ir = ir;
    return c;
}

uchar port_fx_mix(uchar o, uchar c, long a)
{
    return a >= 240 ? c : a < 8 ? o : mix(o, c, a);
}
