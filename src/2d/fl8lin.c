/* fl8lin.c -- Terra Nova v1.09 flat8 linear texture mappers (function table GRC_LIN,
 * GRC_LIT_LIN, GRC_CLUT_LIN for BMT_FLAT8).  Decompiled from __FF.EXE; the original is
 * self-modifying asm (the System Shock fl8lin.asm family).
 *
 *   0x32F060 gri_lin_right_edge       0x32F100 gri_lin_left_edge
 *   0x32F200 gri_lin_umap             0x32F2D0 gri_lin_loop
 *   0x32F410 gri_lit_lin_right_edge   0x32F4E0 gri_lit_lin_left_edge
 *   0x32F610 gri_lit_lin_umap         0x32F700 gri_lit_lin_loop
 *   0x32F880 gri_clut_lin_right_edge  0x32F920 gri_clut_lin_left_edge
 *   0x32FA20 gri_clut_lin_umap        0x32FAF0 gri_clut_lin_loop
 *
 * gri_xxx_lin_umap(bm, n, vpl, parm) are function table entries called by temp_lin_umap & co.
 * for every triangle of a fan.  They build a tmap info on the stack, let gri_lin_gradients
 * (0x34C270) compute du/dx, dv/dx (+0x34, +0x38) and du/dy, dv/dy (+0x58, +0x5C) of the
 * triangle and run the generic raster driver gri_lin_raster (0x34C490), which calls the edge
 * functions and the scanline loop below through the tmap info.
 *
 * Every immediate the asm patches into its own code is a member of a per-mapper static
 * state.  The tmap info fields are used in the asm's own way:
 *   n (+00)        line count; the edge functions first store the start vertex pointer there
 *   +08 dword      low word: left x fraction, high word: right x fraction (over bm.bits)
 *   left.x (+1C)   destination pointer of the left end of the current scanline
 *   left.y/right.y end scanline of the edge (fix_cint(y))
 *   left.u/v       texture coordinates at the left end (lit: left.i, right.i = intensities)
 *   left.di (+3C)  integer x step of the left edge
 * The right end pointer of a scanline only lives in the patched code (state rp).
 * Carry chains of the asm (adc on 16.16 fractions) are reproduced with 32-bit fractions.
 */
#include "2d_tmap.h"

#ifndef TN_WATCOM_ABI
/* the GPU ground (port/video/gpuland.c): the terrain's pixels noted while it is drawn */
extern unsigned short *port_gl_sh;
extern const unsigned char *port_gl_sh_base;
#define GL_NOTE(q) do { if (port_gl_sh) port_gl_sh[(q) - port_gl_sh_base] = (unsigned short)(0x100 | *(q)); } while (0)
#else
#define GL_NOTE(q) ((void)0)
#endif

extern long grd_recip_tab[];       /* 0x45F71C fix 1/n */

/* original 2d-core routines */
void gri_lin_gradients(grs_tmap_loop_info *ti, grs_vertex **vpl);           /* 0x34C270 */
int gri_lin_raster(grs_tmap_loop_info *ti, int n, grs_vertex **vpl);        /* 0x34C490 */

typedef struct {
    uchar *dbits;                  /* canvas bits/row at init (edge functions) */
    ulong drow;
    uchar *bits;                   /* texture */
    ulong mask;
    int wlog;
    ulong du16, dv16;              /* du/dx, dv/dx fractions << 16 */
    ulong step, step2;             /* index step without/with v carry */
    uchar *rp;                     /* right end of the scanline */
    ulong rfrac16, rint;           /* right edge: fraction step << 16, int step + row */
    ushort lfrac;                  /* left edge: fraction step, int step + row */
    ulong lint;
    ulong ustep, vstep;            /* left end u/v step per line */
    fix di_l, di_r;                /* lit: intensity step per line */
    uchar *clut, *ltab;
} lin_state;

static lin_state lin_s, lit_lin_s, clut_lin_s;

#define fix_cint32(a) ((fix)((a) + 0xffff) >> 16)
#define TI_FRAC(ti) (*(ulong *)&(ti)->bm.bits)
#define TI_FRAC_L(ti) (((ushort *)&(ti)->bm.bits)[0])
#define TI_FRAC_R(ti) (((ushort *)&(ti)->bm.bits)[1])

/* x slope of edge v0->v1 as the asm computes it (idiv, then -1 for leftward edges) */
static fix edge_slope(grs_vertex *v0, grs_vertex *v1)
{
    fix dx = v1->x - v0->x;
    fix q = (fix)((long long)dx * 65536 / (v1->y - v0->y));
    if (q != 0x7fffffff && q != (fix)0x80000000)
        q += dx < 0 ? -1 : 0;
    return q;
}

/* common part of every edge: start pointer of the first scanline; returns the fix x of
 * the first scanline + 0xffff (fraction stored by the caller) */
static fix edge_start(lin_state *s, grs_vertex *v0, fix q, uchar **p)
{
    fix ypre = (-v0->y) & 0xffff;
    fix x = v0->x + tm_fix_mul(q, ypre) + 0xffff;
    uchar *base = s->dbits + (long)((ulong)fix_cint32(v0->y) * s->drow);
    *p = base + (x >> 16);
    return x;
}

/* ---------------- edges ---------------- */
static void right_edge(lin_state *s, grs_tmap_loop_info *ti, grs_vertex *v0, grs_vertex *v1, int dec)
{
    fix q, x;
    uchar *p;
    ti->right.y = fix_cint32(v1->y);
    TI_FRAC_R(ti) = (ushort)v0->x;
    ti->n = (int)v0;
    q = edge_slope(v0, v1);
    s->rfrac16 = (ulong)q << 16;
    s->rint = (q >> 16) + (ushort)s->drow;
    x = edge_start(s, v0, q, &p);
    TI_FRAC_R(ti) = (ushort)x;
    s->rp = p - dec;
}

static fix left_edge(lin_state *s, grs_tmap_loop_info *ti, grs_vertex *v0, grs_vertex *v1, int dec)
{
    fix q, x;
    uchar *p;
    ti->left.y = fix_cint32(v1->y);
    TI_FRAC_L(ti) = (ushort)v0->x;
    ti->n = (int)v0;
    q = edge_slope(v0, v1);
    s->lfrac = (ushort)q;
    ti->left.di = q >> 16;
    s->lint = (q >> 16) + (ushort)s->drow;
    x = edge_start(s, v0, q, &p);
    TI_FRAC_L(ti) = (ushort)x;
    ti->left.x = (fix)(p - dec);
    return x;
}

/* u, v at the left end of the first scanline and their steps per line */
static void left_uv(lin_state *s, grs_tmap_loop_info *ti, grs_vertex *v0, fix x)
{
    fix xpre = (fix)(x & 0xffff0000) - v0->x;
    fix ypre = (-v0->y) & 0xffff;
    ti->left.u = tm_fix_mul(ti->left.du, xpre) + tm_fix_mul(ti->right.du, ypre) + v0->u;
    ti->left.v = tm_fix_mul(ti->left.dv, xpre) + tm_fix_mul(ti->right.dv, ypre) + v0->v;
    s->ustep = (ulong)ti->left.du * (ulong)ti->left.di + (ulong)ti->right.du;
    s->vstep = (ulong)ti->left.dv * (ulong)ti->left.di + (ulong)ti->right.dv;
}

/* intensity slope along an edge and value at the first scanline */
static fix edge_i(grs_vertex *v0, grs_vertex *v1, fix *di)
{
    fix ypre = (-v0->y) & 0xffff;
    *di = tm_fix_div(v1->i - v0->i, v1->y - v0->y);
    return tm_fix_mul(*di, ypre) + v0->i;
}

grs_tmap_loop_info *gri_lin_right_edge(grs_tmap_loop_info *ti, grs_vertex **pv1, grs_vertex **pv0)
{
    right_edge(&lin_s, ti, *pv0, *pv1, 0);
    return ti;
}

grs_tmap_loop_info *gri_lin_left_edge(grs_tmap_loop_info *ti, grs_vertex **pv1, grs_vertex **pv0)
{
    grs_vertex *v0 = *pv0;
    fix x = left_edge(&lin_s, ti, v0, *pv1, 0);
    left_uv(&lin_s, ti, v0, x);
    return ti;
}

grs_tmap_loop_info *gri_clut_lin_right_edge(grs_tmap_loop_info *ti, grs_vertex **pv1, grs_vertex **pv0)
{
    right_edge(&clut_lin_s, ti, *pv0, *pv1, 0);
    return ti;
}

grs_tmap_loop_info *gri_clut_lin_left_edge(grs_tmap_loop_info *ti, grs_vertex **pv1, grs_vertex **pv0)
{
    grs_vertex *v0 = *pv0;
    fix x = left_edge(&clut_lin_s, ti, v0, *pv1, 0);
    left_uv(&clut_lin_s, ti, v0, x);
    return ti;
}

grs_tmap_loop_info *gri_lit_lin_right_edge(grs_tmap_loop_info *ti, grs_vertex **pv1, grs_vertex **pv0)
{
    grs_vertex *v0 = *pv0, *v1 = *pv1;
    right_edge(&lit_lin_s, ti, v0, v1, 1);
    ti->right.i = edge_i(v0, v1, &lit_lin_s.di_r);
    return ti;
}

grs_tmap_loop_info *gri_lit_lin_left_edge(grs_tmap_loop_info *ti, grs_vertex **pv1, grs_vertex **pv0)
{
    grs_vertex *v0 = *pv0, *v1 = *pv1;
    fix x = left_edge(&lit_lin_s, ti, v0, v1, 1);
    ti->left.i = edge_i(v0, v1, &lit_lin_s.di_l);
    left_uv(&lit_lin_s, ti, v0, x);
    return ti;
}

/* ---------------- scanline loops ---------------- */

/* next scanline: x fractions and both end pointers; returns the left end, the right end is
 * s->rp.  *lc gets the left edge's extra carry as a mask (0 or -1). */
static uchar *advance(lin_state *s, grs_tmap_loop_info *ti, ulong *lc)
{
    ulong f = TI_FRAC(ti), g, lo;
    uchar *l;
    g = f + s->rfrac16;
    s->rp += s->rint + (g < f);
    lo = (g & 0xffff) + s->lfrac;
    g = (g & 0xffff0000) | (lo & 0xffff);
    *lc = (lo >> 16) ? 0xffffffff : 0;
    l = (uchar *)ti->left.x + s->lint + (lo >> 16);
    TI_FRAC(ti) = g;
    ti->left.x = (fix)l;
    return l;
}

static int lin_loop(lin_state *s, grs_tmap_loop_info *ti)
{
    int cnt = ti->left.x - (fix)s->rp;
    fix u, v;
    ulong lc;

    if (cnt > 0)
        return TRUE;
    if (cnt == 0)
        goto next;
    u = ti->left.u, v = ti->left.v;
    for (;;) {
        /* draw: cnt < 0 pixels ending at rp */
        {
            ulong idx = (ulong)(u >> 16) + ((ulong)(v >> 16) << (s->wlog & 31));
            ulong uf = (ulong)u << 16, vf = (ulong)v << 16, t;
            int c;
            t = vf + s->dv16; c = t < vf; vf = t;
            do {
                int cu;
                uchar px;
                idx &= s->mask;
                t = uf + s->du16; cu = t < uf; uf = t;
                px = s->bits[idx];
                s->rp[cnt] = s->clut ? s->clut[px] : px;
                idx += (c ? s->step2 : s->step) + cu;
                t = vf + s->dv16; c = t < vf; vf = t;
            } while (++cnt != 0);
        }
    next:
        for (;;) {
            uchar *l = advance(s, ti, &lc);
            cnt = (fix)l - (fix)s->rp;
            if (cnt > 0)
                return TRUE;
            u = ti->left.u + (ti->left.du & lc) + s->ustep;
            v = ti->left.v + (ti->left.dv & lc) + s->vstep;
            ti->left.u = u;
            ti->left.v = v;
            if (--ti->n == 0)
                return FALSE;
            if (cnt != 0)
                break;
        }
    }
}

int gri_lin_loop(grs_tmap_loop_info *ti)
{
    return lin_loop(&lin_s, ti);
}

int gri_clut_lin_loop(grs_tmap_loop_info *ti)
{
    return lin_loop(&clut_lin_s, ti);
}

#ifndef TN_WATCOM_ABI
extern int port_lit_dither;             /* "Terrain smoothing": the light dithered (perloop.c) */
extern const long port_dith[128], port_dith_row[64];
#endif

int gri_lit_lin_loop(grs_tmap_loop_info *ti)
{
    lin_state *s = &lit_lin_s;
    ulong lc;

    for (;;) {
        int cnt = (fix)s->rp - ti->left.x;
        if (cnt != 0) {
            ulong r, dlo, hi, ecx, eax, edx, esi, edi, t;
            long long p;
            int c;
            if ((ulong)cnt >= 0x400)
                return TRUE;
            r = grd_recip_tab[cnt];
            p = (long long)(ti->right.i - ti->left.i) * (long)r;
            dlo = (ulong)p;
            hi = (ulong)(p >> 32);
            ecx = ((ulong)ti->left.i >> 16) | ((ulong)ti->left.i << 16);
            ecx = (ecx & 0xffff00ff) | ((hi & 0xff) << 8);
            eax = ecx;
            edx = ecx & 0xffff;
            ecx = ((ulong)ti->left.v << 16) | (edx >> 8);
            esi = ((ulong)(ti->left.v >> 16) << (s->wlog & 31)) + (ulong)(ti->left.u >> 16);
            edx = (edx & 0xff) * 0x101;
            edi = (ulong)ti->left.u << 16;
            t = ecx + s->dv16; c = t < ecx; ecx = t;
#ifndef TN_WATCOM_ABI
            int dz = port_lit_dither;
            fix I = ti->left.i, dI = (fix)(p >> 16);
            long at = (long)(s->rp - cnt + 1 - s->dbits), row = s->drow ? (long)s->drow : 1;
            long yy = at >= 0 ? at / row : 0, xx = at - yy * row;
            const long *dd = port_dith + ((yy & 7) << 4);
#endif
            cnt = -cnt;
            do {
                int cu, ci;
                uchar px;
                esi &= s->mask;
                t = edi + s->du16; cu = t < edi; edi = t;
                edx = (edx & ~0xffu) | s->bits[esi];
                esi += (c ? s->step2 : s->step) + cu;
                t = eax + dlo; ci = t < eax; eax = t;
                edx = (edx & 0xff) | ((((edx >> 8) + (ecx & 0xff) + ci) & 0xff) << 8);
                t = ecx + s->dv16; c = t < ecx; ecx = t;
                cnt++;
#ifndef TN_WATCOM_ABI
                if (dz) {
                    px = s->ltab[port_dith_row[((I + dd[xx++ & 7]) >> 16) & 63] | (edx & 0xff)];
                    I += dI;
                } else
#endif
                px = s->ltab[edx];
                ecx = (ecx & 0xffff00ff) | ((ulong)px << 8);
                s->rp[cnt] = px;
                GL_NOTE(s->rp + cnt);
            } while (cnt != 0);
        }
        advance(s, ti, &lc);
        ti->left.i += s->di_l;
        ti->right.i += s->di_r;
        ti->left.u += (ti->left.du & lc) + s->ustep;
        ti->left.v += (ti->left.dv & lc) + s->vstep;
        if (--ti->n == 0)
            return FALSE;
    }
}

/* ---------------- function table entries ---------------- */
static void lin_setup(lin_state *s, grs_bitmap *bm, grs_tmap_loop_info *ti)
{
    s->wlog = bm->wlog;
    ti->wlog = bm->wlog;
    s->mask = ((1u << (bm->hlog & 31)) << (bm->wlog & 31)) - 1;
    s->bits = bm->bits;
    s->drow = grd_bm.row;
    s->dbits = grd_bm.bits;
}

static void lin_steps(lin_state *s, grs_tmap_loop_info *ti)
{
    fix du = ti->left.du, dv = ti->left.dv;
    s->du16 = (ulong)du << 16;
    s->dv16 = (ulong)dv << 16;
    s->step = (ulong)(du >> 16) + ((ulong)(dv >> 16) << (s->wlog & 31));
    s->step2 = s->step + (1u << (s->wlog & 31));
}

int gri_lin_umap(grs_bitmap *bm, int n, grs_vertex **vpl)
{
    grs_tmap_loop_info ti;
    ti.left_edge_func = WATCOM_FN(gri_lin_left_edge);
    ti.right_edge_func = WATCOM_FN(gri_lin_right_edge);
    ti.loop_func = WATCOM_FN(gri_lin_loop);
    lin_setup(&lin_s, bm, &ti);
    lin_s.clut = NULL;
    gri_lin_gradients(&ti, vpl);
    lin_steps(&lin_s, &ti);
    return gri_lin_raster(&ti, n, vpl);
}

int gri_clut_lin_umap(grs_bitmap *bm, int n, grs_vertex **vpl, uchar *cl)
{
    grs_tmap_loop_info ti;
    ti.left_edge_func = WATCOM_FN(gri_clut_lin_left_edge);
    ti.right_edge_func = WATCOM_FN(gri_clut_lin_right_edge);
    ti.loop_func = WATCOM_FN(gri_clut_lin_loop);
    clut_lin_s.clut = cl;
    lin_setup(&clut_lin_s, bm, &ti);
    gri_lin_gradients(&ti, vpl);
    lin_steps(&clut_lin_s, &ti);
    return gri_lin_raster(&ti, n, vpl);
}

/* always draws a triangle (n = 3) */
int gri_lit_lin_umap(grs_bitmap *bm, int n, grs_vertex **vpl)
{
    grs_tmap_loop_info ti;
    ti.left_edge_func = WATCOM_FN(gri_lit_lin_left_edge);
    ti.right_edge_func = WATCOM_FN(gri_lit_lin_right_edge);
    ti.loop_func = WATCOM_FN(gri_lit_lin_loop);
    lin_setup(&lit_lin_s, bm, &ti);
    gri_lin_gradients(&ti, vpl);
    lin_steps(&lit_lin_s, &ti);
    lit_lin_s.ltab = grd_screen->ltab;
    return gri_lin_raster(&ti, 3, vpl);
}
