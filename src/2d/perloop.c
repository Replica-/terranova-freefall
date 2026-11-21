/* perloop.c -- Terra Nova v1.09 flat8 perspective inner loops (asm with self-modifying code
 * in the original; names from 2d-tmap).
 *
 *   0x34C690 gri_per_setup              0x34C725 gri_per_loop
 *   0x34D010 gri_solid_per_setup        0x34D031 gri_trans_per_setup   0x34D0E2 gri_trans_per_loop
 *   0x34D2B0 gri_lit_per_vsetup         0x34D2E6 gri_lit_per_hsetup    0x34D390 gri_lit_per_loop
 *   0x34DF00 gri_trans_lit_per_vsetup   0x34DF36 gri_trans_lit_per_hsetup
 *   0x34DFE0 gri_trans_lit_per_loop
 *   0x34E160 gri_clut_per_setup         0x34E223 gri_clut_per_loop
 *   0x34E410 gri_trans_clut_per_setup   0x34E4CA gri_trans_clut_per_loop
 *
 * A setup(bm, ps) patches the constants of its loop into the code (texture bits and masks,
 * destination steps, the slope fraction; the clut / light table / solid colour); here they go
 * into a static block per loop.  A loop(ps) draws one run: the destination is ps->p + k for
 * k = ps->dp stepping by dp_lo / dp_hi (slope fraction carry) until k reaches 0; the texel
 * offset is (u >> 16) + ((v >> (16 - wlog)) & ((h - 1) << wlog)), masked by (h << wlog) - 1,
 * stepping with the fractions of u and v (a v carry adds a row of w texels); u, v (and i)
 * are stored back.  The setups return the bitmap (pushad/popad).
 * Lit loops: pixel = ltab[(i_int << 8) | texel] (the high half of the index is the sign of
 * i >> 16), only the "lo" u/v steps, destination steps from the canvas row computed by the
 * lit setups (which also store one of them in ps+0x10: row+1, 1-row or row-1).
 * The solid setup makes the transparent loop write the fill colour (ps+8) instead of the
 * texel; the transparent setup switches that off again.
 * Quirk kept: as in the original the loop of a setup only works after that setup ran.
 */
#include "2d.h"

#ifndef TN_WATCOM_ABI
/* the GPU ground (port/video/gpuland.c): the terrain's pixels noted while it is drawn */
extern unsigned short *port_gl_sh;
extern const unsigned char *port_gl_sh_base;
extern unsigned short port_gl_sh_val;   /* 0x100 the ground, 0x200 an object */
#define GL_NOTE(q) do { if (port_gl_sh) port_gl_sh[(q) - port_gl_sh_base] = (unsigned short)(port_gl_sh_val | *(q)); } while (0)
#else
#define GL_NOTE(q) ((void)0)
#endif

typedef struct {
    void *scanline_func0;          /* +00 */
    void *shell_func;              /* +04 */
    long fill;                     /* +08 clut / fill colour */
    fix scan_slope;                /* +0C */
    long dp_lo, dp_hi;             /* +10 +14 */
    fix u, v;                      /* +18 +1C */
    fix du_lo, dv_lo, du_hi, dv_hi;    /* +20 +24 +28 +2C */
    uchar *p;                      /* +30 */
    long dp;                       /* +34 */
    fix y;                         /* +38 */
    long _3c[6];                   /* +3C .. +50 */
    fix i;                         /* +54 light */
    long _58[2];                   /* +58 +5C */
    fix di;                        /* +60 */
} per_ps;

typedef struct {
    long dp_lo, dp_hi;             /* destination steps */
    ulong yfs;                     /* slope fraction << 16 */
    uchar *bits;
    ulong w;                       /* (ushort)bm->w */
    uchar wlog, sh;                /* wlog, 16 - wlog (as patched, a byte) */
    ulong umask, hm1, vmask, texmask;
    uchar solid, color;            /* transparent loop: write the fill colour */
    uchar *clut;
    uchar *ltab;
} per_lp;

/* the patched constants (code bytes in the original, so no original data address) */
static per_lp lp_plain, lp_trans, lp_lit, lp_tlit, lp_clut, lp_tclut;

static void tex_setup(per_lp *P, grs_bitmap *bm, per_ps *ps)
{
    ulong h = (ushort)bm->h;
    int s;

    P->yfs = (ulong)ps->scan_slope << 16;
    P->bits = bm->bits;
    P->w = (ushort)bm->w;
    P->wlog = bm->wlog;
    s = P->wlog & 31;
    P->umask = (1u << s) - 1;
    P->hm1 = h - 1;
    P->vmask = (h - 1) << s;
    P->sh = (uchar)(16 - P->wlog);
    P->texmask = (h << s) - 1;
}

static void dp_setup(per_lp *P, per_ps *ps)
{
    P->dp_lo = ps->dp_lo;
    P->dp_hi = ps->dp_hi;
}

static long vstep(const per_lp *P, fix dv) { return (dv >> (P->sh & 31)) & P->vmask; }

enum { K_PLAIN, K_TRANS, K_CLUT, K_TCLUT };

static void loop(per_ps *ps, const per_lp *P, int kind)
{
    ulong dvlo = (ulong)ps->dv_lo << 16, dvhi = (ulong)ps->dv_hi << 16;
    ulong dulo = (ulong)ps->du_lo << 16, duhi = (ulong)ps->du_hi << 16;
    long slo = vstep(P, ps->dv_lo) + (ps->du_lo >> 16);
    long shi = vstep(P, ps->dv_hi) + (ps->du_hi >> 16);
    ulong uf = (ulong)ps->u << 16, vf = (ulong)ps->v << 16, yf, t;
    long off = (ps->u >> 16) + vstep(P, ps->v);
    long k = ps->dp;
    uchar *p = ps->p, pix;
    int hi, vc, uc;

    yf = (ulong)ps->y << 16;
    t = yf + P->yfs;
    hi = t < yf;
    yf = t;
    for (;;) {
        off &= P->texmask;
        t = vf + (hi ? dvhi : dvlo);
        vc = t < vf;
        vf = t;
        pix = P->bits[off];
        t = uf + (hi ? duhi : dulo);
        uc = t < uf;
        uf = t;
        off += (hi ? shi : slo) + (vc ? (long)P->w : 0) + uc;
        switch (kind) {
        case K_PLAIN:
            p[k] = pix;
            GL_NOTE(p + k);
            break;
        case K_TRANS:
            if (pix) {
                p[k] = P->solid ? P->color : pix;
                GL_NOTE(p + k);
            }
            break;
        case K_CLUT:
            p[k] = P->clut[pix];
            GL_NOTE(p + k);
            break;
        default:
            if (pix) {
                p[k] = P->clut[pix];
                GL_NOTE(p + k);
            }
            break;
        }
        k += hi ? P->dp_hi : P->dp_lo;
        if (k == 0)
            break;
        t = yf + P->yfs;
        hi = t < yf;
        yf = t;
    }
    ps->u = ((off & P->umask) << 16) | (uf >> 16);
    ps->v = (((off >> (P->wlog & 31)) & P->hm1) << 16) | (vf >> 16);
}

/* ---------------------------------------------------------------- plain */
grs_bitmap *gri_per_setup(grs_bitmap *bm, per_ps *ps)
{
    dp_setup(&lp_plain, ps);
    tex_setup(&lp_plain, bm, ps);
    return bm;
}

void gri_per_loop(per_ps *ps) { loop(ps, &lp_plain, K_PLAIN); }

/* ---------------------------------------------------------------- transparent / solid */
grs_bitmap *gri_solid_per_setup(grs_bitmap *bm, per_ps *ps)
{
    lp_trans.solid = 1;
    lp_trans.color = (uchar)ps->fill;
    dp_setup(&lp_trans, ps);
    tex_setup(&lp_trans, bm, ps);
    return bm;
}

grs_bitmap *gri_trans_per_setup(grs_bitmap *bm, per_ps *ps)
{
    lp_trans.solid = 0;
    dp_setup(&lp_trans, ps);
    tex_setup(&lp_trans, bm, ps);
    return bm;
}

void gri_trans_per_loop(per_ps *ps) { loop(ps, &lp_trans, K_TRANS); }

/* ---------------------------------------------------------------- clut */
grs_bitmap *gri_clut_per_setup(grs_bitmap *bm, per_ps *ps)
{
    dp_setup(&lp_clut, ps);
    lp_clut.clut = (uchar *)ps->fill;
    tex_setup(&lp_clut, bm, ps);
    return bm;
}

void gri_clut_per_loop(per_ps *ps) { loop(ps, &lp_clut, K_CLUT); }

grs_bitmap *gri_trans_clut_per_setup(grs_bitmap *bm, per_ps *ps)
{
    dp_setup(&lp_tclut, ps);
    lp_tclut.clut = (uchar *)ps->fill;
    tex_setup(&lp_tclut, bm, ps);
    return bm;
}

void gri_trans_clut_per_loop(per_ps *ps) { loop(ps, &lp_tclut, K_TCLUT); }

/* ---------------------------------------------------------------- lit */
static grs_bitmap *lit_setup(per_lp *P, grs_bitmap *bm, per_ps *ps, int vscan)
{
    ulong row = (ushort)grd_canvas->bm.row;
    long b;

    if (vscan) {
        if (ps->scan_slope >= 0) {
            b = row + 1;
            P->dp_lo = row;
            P->dp_hi = b;
        } else {
            b = row - 1;
            P->dp_lo = b;
            P->dp_hi = row;
        }
    } else {
        if (ps->scan_slope >= 0) {
            b = row + 1;
            if (ps->scan_slope == 0x10000) {
                P->dp_lo = b;
                P->dp_hi = 1;
            } else {
                P->dp_lo = 1;
                P->dp_hi = b;
            }
        } else {
            b = 1 - row;
            P->dp_lo = b;
            P->dp_hi = 1;
        }
    }
    ps->dp_lo = b;
    tex_setup(P, bm, ps);
    P->ltab = grd_screen->ltab;
    return bm;
}

static void lit_loop(per_ps *ps, const per_lp *P, int trans)
{
    ulong duf = (ulong)ps->du_lo << 16, dvf = (ulong)ps->dv_lo << 16, dif = (ulong)ps->di << 16;
    long step = vstep(P, ps->dv_lo) + (ps->du_lo >> 16);
    uchar di_int = (uchar)(ps->di >> 16), ch = (uchar)(ps->i >> 16), pix;
    ulong base = (ulong)(ps->i >> 16) & 0xFFFF0000u;
    ulong ifr = (ulong)ps->i << 16, uf = (ulong)ps->u << 16, vf = (ulong)ps->v << 16, yf, t;
    long off = (ps->u >> 16) + vstep(P, ps->v);
    long k = ps->dp;
    uchar *p = ps->p;
    int vc, uc, ic, hi;

    yf = (ulong)ps->y << 16;
    t = vf + dvf;
    vc = t < vf;
    vf = t;
    for (;;) {
        off &= P->texmask;
        t = uf + duf;
        uc = t < uf;
        uf = t;
        pix = P->bits[off];
        off += step + (vc ? (long)P->w : 0) + uc;
        if (!trans || pix) {
            p[k] = P->ltab[base | ((ulong)ch << 8) | pix];
            GL_NOTE(p + k);
        }
        t = ifr + dif;
        ic = t < ifr;
        ifr = t;
        ch = (uchar)(ch + di_int + ic);
        t = yf + P->yfs;
        hi = t < yf;
        yf = t;
        k += hi ? P->dp_hi : P->dp_lo;
        if (k == 0)
            break;
        t = vf + dvf;
        vc = t < vf;
        vf = t;
    }
    ps->u = ((off & P->umask) << 16) | (uf >> 16);
    ps->v = (((off >> (P->wlog & 31)) & P->hm1) << 16) | (vf >> 16);
    ps->i = ((ulong)ch << 16) | (ifr >> 16);
}

#ifndef TN_WATCOM_ABI
/* "Terrain smoothing" (weather.c, draw_tri): the ground's light between whole levels by an 8 x 8
 * ordered dither of its fraction, instead of cut to the level below (bands of light).
 * port_dith[y << 4 | x] = the 8 x 8 Bayer threshold << 10, less half a level, plus half a step:
 * level = (i + port_dith[...]) >> 16.  The gap bits 3 and 7 let x and y wrap apart (c + d) & 0x77 */
int port_lit_dither;
#define B8(a, b, c, d, e, f, g, h) \
    D(a), D(b), D(c), D(d), D(e), D(f), D(g), D(h), 0, 0, 0, 0, 0, 0, 0, 0
#define D(b) ((b) * 0x400 + 0x200 - 0x8000)
const long port_dith[128] = {
    B8(0, 32, 8, 40, 2, 34, 10, 42), B8(48, 16, 56, 24, 50, 18, 58, 26),
    B8(12, 44, 4, 36, 14, 46, 6, 38), B8(60, 28, 52, 20, 62, 30, 54, 22),
    B8(3, 35, 11, 43, 1, 33, 9, 41), B8(51, 19, 59, 27, 49, 17, 57, 25),
    B8(15, 47, 7, 39, 13, 45, 5, 37), B8(63, 31, 55, 23, 61, 29, 53, 21) };
#undef D
#undef B8

/* a level, (i + port_dith[...]) >> 16 taken & 63, as its light table row (clamped to 0..15) */
#define R4(a) (a) << 8, (a) << 8, (a) << 8, (a) << 8
const long port_dith_row[64] = {
    0, 1 << 8, 2 << 8, 3 << 8, 4 << 8, 5 << 8, 6 << 8, 7 << 8, 8 << 8, 9 << 8, 10 << 8, 11 << 8,
    12 << 8, 13 << 8, 14 << 8, 15 << 8, R4(15), R4(15), R4(15), R4(15), R4(0), R4(0), R4(0), R4(0),
    R4(0), R4(0), R4(0), R4(0) };
#undef R4

/* a destination step as a port_dith index step (x in bits 0..2, y in bits 4..6) */
static long dp_dith(long d, long row)
{
    long q = d >= 0 ? (d + row / 2) / row : -((-d + row / 2) / row);
    return ((d - q * row) & 7) | ((q & 7) << 4);
}

/* lit_loop with the light dithered (the texels and the steps the same) */
static void lit_loop_dither(per_ps *ps, const per_lp *P)
{
    ulong duf = (ulong)ps->du_lo << 16, dvf = (ulong)ps->dv_lo << 16;
    long step = vstep(P, ps->dv_lo) + (ps->du_lo >> 16);
    ulong uf = (ulong)ps->u << 16, vf = (ulong)ps->v << 16, yf, t, yfs = P->yfs;
    long off = (ps->u >> 16) + vstep(P, ps->v), texmask = P->texmask, w = P->w;
    long k = ps->dp, row = (ushort)grd_canvas->bm.row, at, y, c, dlo, dhi;
    long dp_lo = P->dp_lo, dp_hi = P->dp_hi;
    fix I = ps->i, dI = ps->di;
    const uchar *bits = P->bits, *ltab = P->ltab;
    uchar *p = ps->p;
    int vc, uc, hi;

    if (row <= 0)
        row = 1;
    at = (long)(p + k - grd_canvas->bm.bits);
    y = at >= 0 ? at / row : 0;
    c = ((at - y * row) & 7) | ((y & 7) << 4);
    dlo = dp_dith(dp_lo, row);
    dhi = dp_dith(dp_hi, row);
    yf = (ulong)ps->y << 16;
    t = vf + dvf;
    vc = t < vf;
    vf = t;
    for (;;) {
        off &= texmask;
        t = uf + duf;
        uc = t < uf;
        uf = t;
        p[k] = ltab[port_dith_row[((I + port_dith[c]) >> 16) & 63] | bits[off]];
        GL_NOTE(p + k);
        off += step + (vc ? w : 0) + uc;
        I += dI;
        t = yf + yfs;
        hi = t < yf;
        yf = t;
        k += hi ? dp_hi : dp_lo;
        c = (c + (hi ? dhi : dlo)) & 0x77;
        if (k == 0)
            break;
        t = vf + dvf;
        vc = t < vf;
        vf = t;
    }
    ps->u = ((off & P->umask) << 16) | (uf >> 16);
    ps->v = (((off >> (P->wlog & 31)) & P->hm1) << 16) | (vf >> 16);
    ps->i = I;
}
#endif

grs_bitmap *gri_lit_per_vsetup(grs_bitmap *bm, per_ps *ps) { return lit_setup(&lp_lit, bm, ps, 1); }
grs_bitmap *gri_lit_per_hsetup(grs_bitmap *bm, per_ps *ps) { return lit_setup(&lp_lit, bm, ps, 0); }
void gri_lit_per_loop(per_ps *ps)
{
#ifndef TN_WATCOM_ABI
    if (port_lit_dither) {
        lit_loop_dither(ps, &lp_lit);
        return;
    }
#endif
    lit_loop(ps, &lp_lit, 0);
}

grs_bitmap *gri_trans_lit_per_vsetup(grs_bitmap *bm, per_ps *ps) { return lit_setup(&lp_tlit, bm, ps, 1); }
grs_bitmap *gri_trans_lit_per_hsetup(grs_bitmap *bm, per_ps *ps) { return lit_setup(&lp_tlit, bm, ps, 0); }
void gri_trans_lit_per_loop(per_ps *ps) { lit_loop(ps, &lp_tlit, 1); }
