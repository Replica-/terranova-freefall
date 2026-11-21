/* colras.c -- Terra Nova module M094, first part (0x2B2930-0x2B2BDF): hand-written asm helpers
 * of the terrain renderer (source file name unknown; the map files them with thunder.c, which
 * follows).  Decompiled from __FF.EXE v1.09.
 *
 *   0x2B2930 thunder_2B2930 project a haze point     0x2B29B0 thunder_2B29B0 draw column runs
 *   0x2B2AD8 thunder_2B2AD8 set the row table / row length of the column loops
 *
 * Register use (asm, called through the g5 prototypes whose first arguments are don't-cares):
 *   thunder_2B2930: ebx = depth (view z, >= 0x10000 at the only caller), ecx = height; reads
 *     hz_mat[1] / hz_mat[4], hz_vx / hz_vy, the camera tables hz_log2 / hz_divx / hz_divy and
 *     writes the screen point hz_sx / hz_sy.  Clobbers eax, ebx, ecx, edx, esi.
 *   thunder_2B29B0: ebx = StripRas, ecx = number of columns.  Keeps only ebp.
 *   thunder_2B2AD8: ebx = StripRas (rows, row).  Clobbers eax, edx.
 *
 * thunder_2B29B0 is self-modifying: it patches the StripRas values into the immediates of one
 * of two copies of its column loop (colras_fade_loop when fade_step != 0, colras_flat_loop
 * otherwise); thunder_2B2AD8 patches the row table and the row length into both copies, and the
 * fading copy patches its light row (light + (fade & 0xFF00)) once per column.  Our C writes
 * the same immediates into the same code bytes and reads the row table / row length back from
 * them, so the C and the original halves of the pair can be mixed (and a call before any
 * thunder_2B2AD8 uses the same stale values as the original would).
 *
 * The loop, per column i (0..n-1): y = v >> 16; if y < ybuf[i], the pixels from row y down to
 * ybuf[i] - 1 of column dst + i get shade[light_row[tex[u]]] with u advancing by one per pixel
 * (only its low byte wraps: the texture row is 256 bytes), and ybuf[i] = y.  Then v += dv (and
 * fade += fade_step).  u is not reset between columns.
 *
 * Quirks kept:
 *  - n = 0 runs the loop 2^32 times (the counter is tested only after the first column).
 *  - The shift counts of thunder_2B2930 wrap as the asm's byte arithmetic and cl & 31 do.
 */
#include "g6_a.h"

#define PATCH(lbl, off) (*(ulong *)((lbl) + (off)))

void thunder_2B2930(long a, long b, fix depth, long h)
{
    long x = hz_mat[1] * h + hz_vx;       /* imul: low 32 bits */
    long y = hz_mat[4] * h + hz_vy;
    ulong d = (ulong)depth;
    uchar sh;
    ulong idx;

    if (depth > 0xFFFFFF)
        sh = (uchar)(hz_log2[d >> 24] + 0x10);
    else
        sh = (uchar)(hz_log2[d >> 16] + 8);
    idx = (d >> (sh & 31)) & 0xFF;
    sh = (uchar)(sh + 8);
    hz_sx = (fix)(((long long)hz_divx[idx] * x) >> (sh & 31)) + hz_cx;
    hz_sy = (fix)(((long long)hz_divy[idx] * y) >> (sh & 31)) + hz_cy;
}

void thunder_2B2AD8(long a, long b, StripRas *r)
{
    PATCH(colras_fade_loop, 0x4B) = (ulong)r->rows;
    PATCH(colras_flat_loop, 0x3B) = (ulong)r->rows;
    PATCH(colras_fade_loop, 0x81) = (ulong)r->row;
    PATCH(colras_flat_loop, 0x61) = (ulong)r->row;
}

#ifndef TN_WATCOM_ABI
/* the GPU ground (port/video/gpuland.c): the far land's pixels noted while it is drawn */
extern unsigned short *port_gl_shc;
extern const unsigned char *port_gl_sh_base;
#define GL_NOTE_FAR(q) do { if (port_gl_shc) port_gl_shc[(q) - port_gl_sh_base] = (unsigned short)(0x100 | *(q)); } while (0)
#else
#define GL_NOTE_FAR(q) ((void)0)
#endif

/* one column: rows y .. top - 1 of the column at column pointer p (one row above row y) */
static void colras_column(uchar *p, long cnt, ulong *u, uchar *tex, uchar *light, uchar *shade, long row)
{
    ulong uu = *u;
    do {
        uchar c = tex[uu];
        uu = (uu & ~0xFFul) | ((uu + 1) & 0xFF);
        c = light[c];
        p += row;
        *p = shade[c];
        GL_NOTE_FAR(p);
    } while (--cnt);
    *u = uu;
}

#ifndef TN_WATCOM_ABI
/* "Fog" (port/video/fog.c): the column x of the caster's ring through the ring's haze row of its fog
 * level, dithered with the next level per pixel (the column's fog port_fog_f[x]) */
static void colras_fog_column(uchar *p, long cnt, ulong *u, uchar *tex, uchar *light, long row, long x, long y)
{
    long f = x >= 0 && x < 4096 ? port_fog_f[x] : 0x1000, fi = f >> 8, fr = f & 0xFF;
    const uchar *a = port_fog_rows[fi], *b = port_fog_rows[fi < 16 ? fi + 1 : 16];
    const uchar *dd = port_fog_dd[x & 7];
    ulong uu = *u;
    if (port_tc_on) {                   /* "True colour" (port/video/truecol.c): noted, its haze row's
                                         * colour and the fog */
        const uchar *z = port_fog_row0;
        ulong am = (ulong)PORT_TC_FOG << 24 | (ulong)(f >> 4 > 255 ? 255 : f >> 4) << 16;
        ulong k0 = (ulong)(p + row - port_tc_base), k1 = (ulong)(p + row * cnt - port_tc_base);
        ulong *inf = port_tc_info + k0;
        if (k0 < port_tc_n && k1 < port_tc_n && (ulong)y < 2048 && (ulong)(y + cnt) <= 2048) {
            uchar *tr = port_tc_row + y;
            long i;
            for (i = 0; i < cnt; i++)   /* (written once: haze.c's drawing threads read them) */
                if (!tr[i])
                    tr[i] = 1;
            do {
                uchar c = tex[uu], o;
                uu = (uu & ~0xFFul) | ((uu + 1) & 0xFF);
                c = light[c];
                p += row;
                *p = o = (dd[y++ & 7] < fr ? b : a)[c];
                GL_NOTE_FAR(p);
                *inf = am | (ulong)z[c] << 8 | o;
                inf += row;
            } while (--cnt);
            *u = uu;
            return;
        }
    }
    if (fr == 0) {                      /* one fog level for the whole column: no dither */
        do {
            uchar c = tex[uu];
            uu = (uu & ~0xFFul) | ((uu + 1) & 0xFF);
            p += row;
            *p = a[light[c]];
            GL_NOTE_FAR(p);
        } while (--cnt);
        *u = uu;
        return;
    }
    do {
        uchar c = tex[uu];
        uu = (uu & ~0xFFul) | ((uu + 1) & 0xFF);
        c = light[c];
        p += row;
        *p = (dd[y++ & 7] < fr ? b : a)[c];
        GL_NOTE_FAR(p);
    } while (--cnt);
    *u = uu;
}
/* p is rows[y] + the column's address - row (rows[]: offsets of the rows) */
#define COLUMN(p, cnt, u, tex, light, shade, row) \
    (port_fog_cols ? colras_fog_column(p, cnt, u, tex, light, row, (long)((p) - rows[y] + (row) - strip_bits), y) \
                   : colras_column(p, cnt, u, tex, light, shade, row))
#else
#define COLUMN colras_column
#endif

void thunder_2B29B0(long a, long b, StripRas *r, long n)
{
    uchar *L;
    long *yb = r->ybuf + n;
    long *rows, row, i, y, top;
    ulong u = (ulong)r->u;
    ulong k;
    fix v = r->v, dv = r->dv;

    if (r->fade_step != 0) {
        long fade, step = r->fade_step;
        L = colras_fade_loop;
        PATCH(L, 0x0D) = (ulong)yb;
        PATCH(L, 0xA7) = (ulong)yb;
        PATCH(L, 0x44) = (ulong)yb;
        PATCH(L, 0x17) = (ulong)dv;
        PATCH(L, 0x92) = (ulong)dv;
        PATCH(L, 0x73) = (ulong)r->tex;
        PATCH(L, 0x62) = (ulong)r->light;
        PATCH(L, 0x87) = (ulong)r->shade;
        k = (ulong)r->dst + n - r->row;
        PATCH(L, 0x53) = k;
        PATCH(L, 0x02) = (ulong)step;
        PATCH(L, 0x9B) = (ulong)step;
        rows = (long *)PATCH(L, 0x4B);
        row = (long)PATCH(L, 0x81);
        fade = r->fade - step;
        i = -n;
        do {
            fade += step;
            y = v >> 16;
            top = yb[i];
            if (y < top) {
                uchar *lrow = r->light + (fade & 0xFF00);
                yb[i] = y;
                PATCH(L, 0x7B) = (ulong)lrow;
                COLUMN((uchar *)(rows[y] + k + i), top - y, &u, r->tex, lrow, r->shade, row);
            }
            v += dv;
        } while (++i != 0);
        return;
    }
    L = colras_flat_loop;
    PATCH(L, 0x07) = (ulong)yb;
    PATCH(L, 0x81) = (ulong)yb;
    PATCH(L, 0x34) = (ulong)yb;
    PATCH(L, 0x11) = (ulong)dv;
    PATCH(L, 0x72) = (ulong)dv;
    PATCH(L, 0x53) = (ulong)r->tex;
    PATCH(L, 0x5B) = (ulong)r->light;
    PATCH(L, 0x67) = (ulong)r->shade;
    k = (ulong)r->dst + n - r->row;
    PATCH(L, 0x43) = k;
    rows = (long *)PATCH(L, 0x3B);
    row = (long)PATCH(L, 0x61);
    i = -n;
    do {
        y = v >> 16;
        top = yb[i];
        if (y < top) {
            yb[i] = y;
            COLUMN((uchar *)(rows[y] + k + i), top - y, &u, r->tex, r->light, r->shade, row);
        }
        v += dv;
    } while (++i != 0);
}
