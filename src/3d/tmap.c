/* tmap.c -- x:\prj\tech\libsrc\3d\tmap.asm (Terra Nova v1.09; RCS string at 0x362526):
 * texture-mapped polygons.  Decompiled from __FF.EXE (hand-written asm, self-modifying).
 *
 *   0x2F8C4C g3_draw_tmap_tile           0x2F8C69 g3_light_tmap_tile
 *   0x2F8C86 g3_tmap_tile_t3 (ours)      0x2F8CA3 g3_tmap_tile_t2 (ours)
 *   0x2F8CBE do_tmap_tile                0x2F8E46 g3_check_and_quad_tile_0 (ours)
 *   0x2F8E5D g3_check_and_quad_tile_1    0x2F8E74 g3_check_and_quad_tile_2
 *   0x2F8E8B g3_check_and_quad_tile_3    0x2F8ECD g3_draw_floor_map
 *   0x2F8EE7 g3_light_floor_map          0x2F8F01 g3_draw_wall_map
 *   0x2F8F18 g3_light_wall_map           0x2F8F2F g3_draw_lmap
 *   0x2F8F46 g3_light_lmap               0x2F8F5D g3_light_tmap
 *   0x2F8F74 g3_draw_tmap                0x2F8F89 do_tmap
 *   0x2F8FFB g3_quad_tile_0 (ours)       0x2F9012 g3_quad_tile_1 (ours)
 *   0x2F9029 g3_quad_tile_2 (ours)       0x2F9040 g3_quad_tile_3 (ours)
 *   0x2F9060 quad_tile (ours)            0x2F9313 g3_compute_warp (ours)
 *   0x2F93B7 calc_warp_matrix
 *
 * Every public entry saves all registers (pushad), stores the 2D canvas function index of its
 * mapper (g3d_tmap_index) and a type (g3d_tmap_type: 0 linear, 1 lit linear, 2 perspective,
 * 3 lit perspective) and joins one of three bodies, which all end in the same drawing code;
 * the result is eax = 0 (drawn) or 0x10 (rejected).  Registers:
 *   tile entries / do_tmap_tile: eax = upper-left point, ebx = u step vector, ecx = v step
 *     vector, edx = n, esi = point list, edi = bitmap (Shock's g3_draw_tmap_tile pragma);
 *   quad tiles: eax = tiles across, ebx = tiles down, esi = 4 points, edi = bitmap;
 *   floor/wall/lmap/tmap / do_tmap: eax = n, edx = point list, ebx = bitmap;
 *   g3_compute_warp: ebx = origin, ecx = point along u, edx = point along v (no pushad; it
 *     falls into calc_warp_matrix, esi = origin).
 * (index, type) per entry: tile 3A/0 46/1 6C/3 60/2, quad tile and check_and_quad tile
 * 3A/0 46/1 6C/3 60/2, floor 30/2 32/3, wall 2A/2 2C/3, lmap 3A/0 46/1, tmap 6C/3 60/2.
 *
 * The drawing code copies the list, makes every point carry a pointer to its saved uv
 * (clip_save_uv, PF_UVSAVED), clips (g3_clip_polygon), and for perspective types switches to
 * the linear mapper (index 3A, or 46 for the lit type) when the depth range of the clipped
 * polygon is less than or equal to (min z >> g3d_detail_a).  The bitmap must be w = 1 << wlog,
 * h = 1 << hlog.  Vertex u, v = ((saved uv >> 8) moved one step toward 0) << wlog / hlog;
 * i = point i << 8 (sign extended); w = g3d_tmap_scale / z for perspective only.
 * Self-modifying code in the original, plain variables here: the shift counts (0x2F9289,
 * 0x2F929D) are wlog and hlog, and the instruction at 0x2F92AB is either the w computation
 * or a jump over it.  Vertices of the linear case keep the w left in the buffer.
 * Quirks kept: tile u, v are bits 8..23 of the quotient (logical shift); the quad tiles set
 * PF_U only; do_tmap tests bitmap type and align as one word (== 4: BMT_RSD8, align 0) and
 * unpacks RSD8 bitmaps into g3d_tmap_rsd_bm with gr_rsd8_convert before rejecting off-screen
 * polygons.  A clipped polygon of 1 point makes the perspective test run away (not reproduced:
 * the original crashes).
 */
#include "3d_b.h"

#ifndef TN_WATCOM_ABI
#include "gpuland.h"                    /* port/video/gpuland.c: GPU rendering's objects */
extern int port_s3d_on;                 /* port/video/sharp3d.c: drawing the sharp view */
static grs_bitmap *gl_rsd_key;          /* the RSD8 bitmap g3d_tmap_rsd_bm was unpacked from */
#endif

#ifndef TN_LINK_ORIGINAL_DATA
long g3d_tmap_index;
g3s_phandle *g3d_tmap_plist;
grs_bitmap *g3d_tmap_bm;
grs_bitmap g3d_tmap_rsd_bm;
long g3d_tile_w, g3d_tile_h;
fix g3d_warp_matrix[9];
g3s_phandle g3d_tmap_points[100];
long g3d_tmap_uvs[100][2];
grs_vertex g3d_tmap_vbuf[100];
grs_vertex *g3d_tmap_vlist[100];
long g3d_tmap_n;
g3s_vector g3d_warp_pos, g3d_warp_u, g3d_warp_v;
fix g3d_tile_uvw[3];
g3s_point *g3d_tile_origin;
g3s_vector *g3d_tile_uvec, *g3d_tile_vvec;
long g3d_tmap_type;
ushort g3d_tmap_codes;
uchar g3d_tmap_clip_flags[4] = { 1, 5, 1, 5 };
#endif

/* --- common drawing code (0x2F9165) --- */
static int tmap_draw(g3s_phandle *pl)
{
    int i, m, persp;
    uint wlog, hlog;
    grs_bitmap *bm;

    g3d_clip_flags = g3d_tmap_clip_flags[g3d_tmap_type];
    for (i = 0; i < g3d_poly_n; i++)
        g3d_tmap_points[i] = pl[i];
    g3d_tmap_n = g3d_poly_n;
    clip_save_uv(g3d_poly_n, g3d_tmap_points, g3d_tmap_uvs);
    m = g3_clip_polygon(g3d_poly_n, g3d_tmap_points, g3d_poly_clipped);
    if (m == 0)
        goto reject;
    g3d_poly_n = m;

    persp = 0;
    if (g3d_tmap_type & 2) {
        /* depth range of the clipped polygon (unsigned min / max) */
        ulong zmin = g3d_poly_clipped[m - 1]->z, zmax = zmin;
        for (i = m - 2; i >= 0; i--) {
            ulong z = g3d_poly_clipped[i]->z;
            if (z < zmin)
                zmin = z;
            if (z >= zmax)
                zmax = z;
        }
        if (((long)zmin >> (g3d_detail_a & 31)) < (long)(zmax - zmin))
            persp = 1;
        else
            g3d_tmap_index = (g3d_tmap_type & 1) ? 0x46 : 0x3A;
    }

    bm = g3d_tmap_bm;
    wlog = bm->wlog;
    hlog = bm->hlog;
    if ((ushort)(1u << (wlog & 31)) != (ushort)bm->w || (ushort)(1u << (hlog & 31)) != (ushort)bm->h)
        goto reject;

    for (i = m - 1; i >= 0; i--) {
        g3s_point *p = g3d_poly_clipped[i];
        grs_vertex *v = &g3d_tmap_vbuf[i];
        long *uv, u;

        g3d_tmap_vlist[i] = v;
        if (!(p->p3_flags & PF_PROJECTED))
            g3b_project_point(p);
        v->x = p->sx;
        v->y = p->sy;
        uv = (long *)p->uvp;
        u = uv[0] >> 8;
        u = (u > 0) ? u - 1 : u + 1;
        v->u = u << (wlog & 31);
        u = uv[1] >> 8;
        u = (u > 0) ? u - 1 : u + 1;
        v->v = u << (hlog & 31);
        v->i = (long)p->i << 8;
        if (persp)
            v->w = fix_div(g3d_tmap_scale, p->z);
    }
    clip_restore_uv(g3d_tmap_n, g3d_tmap_points);
#ifndef TN_WATCOM_ABI
    /* GPU rendering (port/video/gpuland.c): an object's polygon into the sharp canvas, caught as
     * drawn, and its pixels noted as the objects' */
    if (port_gl_capturing && port_s3d_on && bm->type == BMT_FLAT8 && !grd_canvas->gc.fill_type &&
        port_gl_obj_canvas(grd_canvas->bm.bits)) {
        PortGlObjVert gv[24];
        int n = m < 24 ? m : 24, lit = g3d_tmap_type & 1;
        for (i = 0; i < n; i++) {
            grs_vertex *v = g3d_tmap_vlist[i];
            gv[i].sx = v->x / 65536.0f;
            gv[i].sy = v->y / 65536.0f;
            gv[i].z = g3d_poly_clipped[i]->z / 65536.0f;
            gv[i].u = v->u / 65536.0f;
            gv[i].v = v->v / 65536.0f;
            gv[i].shade = lit ? v->i / 65536.0f : 0;
        }
        port_gl_obj(gv, n, bm->bits, bm->w, bm->h, bm->flags & BMF_TRANS, lit ? grd_screen->ltab : NULL,
                    bm == &g3d_tmap_rsd_bm ? (const void *)gl_rsd_key : (const void *)bm);
        port_gl_sh_val = 0x200;
        port_gl_shadow(1);
        WATCOM_CALL(grd_canvas_table[g3d_tmap_index], (long)g3d_tmap_bm, g3d_poly_n, (long)g3d_tmap_vlist);
        port_gl_shadow(0);
        port_gl_sh_val = 0x100;
        return 0;
    }
#endif
    WATCOM_CALL(grd_canvas_table[g3d_tmap_index], (long)g3d_tmap_bm, g3d_poly_n, (long)g3d_tmap_vlist);
    return 0;

reject:
    clip_restore_uv(g3d_tmap_n, g3d_tmap_points);
    return CLIP_ALL;
}

/* or/and of the codes of the g3d_poly_n points; FALSE (reject) when all are off one side */
static int tmap_codes(g3s_phandle *pl)
{
    ushort c = g3_check_codes(g3d_poly_n, pl);

    if (c >> 8)
        return 0;
    g3d_tmap_codes = c;
    return 1;
}

/* --- warp matrix (texture axes from a point and two step vectors) --- */
void calc_warp_matrix(g3s_vector *org)
{
    g3s_vector *p = &g3d_warp_pos, *u = &g3d_warp_u, *v = &g3d_warp_v;
    fix *w = g3d_warp_matrix;

    *p = *org;
    w[6] = fix_mul(u->y, v->z) - fix_mul(v->y, u->z);
    w[7] = fix_mul(v->x, u->z) - fix_mul(u->x, v->z);
    w[8] = fix_mul(u->x, v->y) - fix_mul(v->x, u->y);
    w[0] = fix_mul(v->y, p->z) - fix_mul(p->y, v->z);
    w[1] = fix_mul(p->x, v->z) - fix_mul(v->x, p->z);
    w[2] = fix_mul(v->x, p->y) - fix_mul(p->x, v->y);
    w[3] = fix_mul(p->y, u->z) - fix_mul(u->y, p->z);
    w[4] = fix_mul(u->x, p->z) - fix_mul(p->x, u->z);
    w[5] = fix_mul(p->x, u->y) - fix_mul(u->x, p->y);
}

/* Step vectors from org to pu / pv divided by the tile counts, then the warp matrix. */
void g3_compute_warp(g3s_vector *org, g3s_vector *pu, g3s_vector *pv)
{
    g3d_warp_u.x = pu->x - org->x;
    g3d_warp_u.y = pu->y - org->y;
    g3d_warp_u.z = pu->z - org->z;
    g3d_warp_v.x = pv->x - org->x;
    g3d_warp_v.y = pv->y - org->y;
    g3d_warp_v.z = pv->z - org->z;
    if (g3d_tile_w != 1) {
        g3d_warp_u.x /= g3d_tile_w;
        g3d_warp_u.y /= g3d_tile_w;
        g3d_warp_u.z /= g3d_tile_w;
    }
    if (g3d_tile_h != 1) {
        g3d_warp_v.x /= g3d_tile_h;
        g3d_warp_v.y /= g3d_tile_h;
        g3d_warp_v.z /= g3d_tile_h;
    }
    calc_warp_matrix(org);
}

/* --- tiles: uv of each point from the warp matrix --- */
int do_tmap_tile(g3s_point *ul, g3s_vector *du, g3s_vector *dv, int n, g3s_phandle *pl, grs_bitmap *bm)
{
    int k;
    g3s_phandle *l;

    g3d_tmap_plist = pl;
    g3d_tmap_bm = bm;
    g3d_tile_origin = ul;
    g3d_tile_uvec = du;
    g3d_tile_vvec = dv;
    if (!tmap_codes(pl))
        return CLIP_ALL;
    g3d_warp_u = *g3d_tile_uvec;
    g3d_warp_v = *g3d_tile_vvec;
    calc_warp_matrix((g3s_vector *)g3d_tile_origin);
    l = g3d_tmap_plist;
    k = g3d_poly_n;
    do {
        g3s_point *p = *l++;
        fix sx = fix_div(p->x, p->z), sy = fix_div(p->y, p->z);
        fix *w = g3d_warp_matrix;

        g3d_tile_uvw[0] = fix_mul(w[0], sx) + fix_mul(w[1], sy) + w[2];
        g3d_tile_uvw[1] = fix_mul(w[3], sx) + fix_mul(w[4], sy) + w[5];
        g3d_tile_uvw[2] = fix_mul(w[6], sx) + fix_mul(w[7], sy) + w[8];
        p->u = (ushort)((ulong)fix_div(g3d_tile_uvw[0], g3d_tile_uvw[2]) >> 8);
        p->v = (ushort)((ulong)fix_div(g3d_tile_uvw[1], g3d_tile_uvw[2]) >> 8);
        p->p3_flags |= PF_U;
    } while (--k);
    return tmap_draw(g3d_tmap_plist);
}

static int tile_entry(long index, long type, g3s_point *ul, g3s_vector *du, g3s_vector *dv, int n,
                      g3s_phandle *pl, grs_bitmap *bm)
{
    g3d_tmap_index = index;
    g3d_poly_n = n;
    g3d_tmap_type = type;
    return do_tmap_tile(ul, du, dv, n, pl, bm);
}

int g3_draw_tmap_tile(g3s_point *ul, g3s_vector *du, g3s_vector *dv, int n, g3s_phandle *pl, grs_bitmap *bm)
{
    return tile_entry(0x3A, 0, ul, du, dv, n, pl, bm);
}
int g3_light_tmap_tile(g3s_point *ul, g3s_vector *du, g3s_vector *dv, int n, g3s_phandle *pl, grs_bitmap *bm)
{
    return tile_entry(0x46, 1, ul, du, dv, n, pl, bm);
}
int g3_tmap_tile_t3(g3s_point *ul, g3s_vector *du, g3s_vector *dv, int n, g3s_phandle *pl, grs_bitmap *bm)
{
    return tile_entry(0x6C, 3, ul, du, dv, n, pl, bm);
}
int g3_tmap_tile_t2(g3s_point *ul, g3s_vector *du, g3s_vector *dv, int n, g3s_phandle *pl, grs_bitmap *bm)
{
    return tile_entry(0x60, 2, ul, du, dv, n, pl, bm);
}

/* --- quad tiles: 4 points get the uv corners (0,0) (w,0) (w,h) (0,h) --- */
int quad_tile(int w, int h, g3s_phandle *pl, grs_bitmap *bm)
{
    g3d_poly_n = 4;
    g3d_tmap_plist = pl;
    g3d_tmap_bm = bm;
    w <<= 8;
    h <<= 8;
    pl[0]->u = 0; pl[0]->v = 0; pl[0]->p3_flags |= PF_U;
    pl[1]->u = w; pl[1]->v = 0; pl[1]->p3_flags |= PF_U;
    pl[2]->u = w; pl[2]->v = h; pl[2]->p3_flags |= PF_U;
    pl[3]->u = 0; pl[3]->v = h; pl[3]->p3_flags |= PF_U;
    if (!tmap_codes(pl))
        return CLIP_ALL;
    return tmap_draw(g3d_tmap_plist);
}

static int quad_entry(long index, long type, int w, int h, g3s_phandle *pl, grs_bitmap *bm)
{
    g3d_tmap_index = index;
    g3d_tmap_type = type;
    g3d_tile_w = w;
    g3d_tile_h = h;
    return quad_tile(w, h, pl, bm);
}

int g3_quad_tile_0(int w, int h, g3s_phandle *pl, grs_bitmap *bm) { return quad_entry(0x3A, 0, w, h, pl, bm); }
int g3_quad_tile_1(int w, int h, g3s_phandle *pl, grs_bitmap *bm) { return quad_entry(0x46, 1, w, h, pl, bm); }
int g3_quad_tile_2(int w, int h, g3s_phandle *pl, grs_bitmap *bm) { return quad_entry(0x6C, 3, w, h, pl, bm); }
int g3_quad_tile_3(int w, int h, g3s_phandle *pl, grs_bitmap *bm) { return quad_entry(0x60, 2, w, h, pl, bm); }

static int check_quad_entry(long index, long type, int w, int h, g3s_phandle *pl, grs_bitmap *bm)
{
    g3d_tmap_index = index;
    g3d_tmap_type = type;
    g3d_tile_w = w;
    g3d_tile_h = h;
    if (!(g3_check_poly_facing(pl[0], pl[1], pl[2]) & 1))
        return CLIP_ALL;
    return quad_tile(w, h, pl, bm);
}

int g3_check_and_quad_tile_0(int w, int h, g3s_phandle *pl, grs_bitmap *bm) { return check_quad_entry(0x3A, 0, w, h, pl, bm); }
int g3_check_and_quad_tile_1(int w, int h, g3s_phandle *pl, grs_bitmap *bm) { return check_quad_entry(0x46, 1, w, h, pl, bm); }
int g3_check_and_quad_tile_2(int w, int h, g3s_phandle *pl, grs_bitmap *bm) { return check_quad_entry(0x6C, 3, w, h, pl, bm); }
int g3_check_and_quad_tile_3(int w, int h, g3s_phandle *pl, grs_bitmap *bm) { return check_quad_entry(0x60, 2, w, h, pl, bm); }

/* --- polygons whose points carry their own uv --- */
int do_tmap(int n, g3s_phandle *pl, grs_bitmap *bm)
{
    g3d_poly_n = n;
    g3d_tmap_plist = pl;
    if (*(ushort *)&bm->type == BMT_RSD8) {     /* type and align as one word */
        if (gr_rsd8_convert(bm, &g3d_tmap_rsd_bm) != 0)
            return CLIP_ALL;
#ifndef TN_WATCOM_ABI
        gl_rsd_key = bm;                /* (the unpacked copy's buffer is shared: the key is its source) */
#endif
        bm = &g3d_tmap_rsd_bm;
    }
    g3d_tmap_bm = bm;
    if (!tmap_codes(g3d_tmap_plist))
        return CLIP_ALL;
    return tmap_draw(g3d_tmap_plist);
}

static int map_entry(long index, long type, int n, g3s_phandle *pl, grs_bitmap *bm)
{
    g3d_tmap_index = index;
    g3d_tmap_type = type;
    return do_tmap(n, pl, bm);
}

int g3_draw_floor_map(int n, g3s_phandle *pl, grs_bitmap *bm) { return map_entry(0x30, 2, n, pl, bm); }
int g3_light_floor_map(int n, g3s_phandle *pl, grs_bitmap *bm) { return map_entry(0x32, 3, n, pl, bm); }
int g3_draw_wall_map(int n, g3s_phandle *pl, grs_bitmap *bm) { return map_entry(0x2A, 2, n, pl, bm); }
int g3_light_wall_map(int n, g3s_phandle *pl, grs_bitmap *bm) { return map_entry(0x2C, 3, n, pl, bm); }
int g3_draw_lmap(int n, g3s_phandle *pl, grs_bitmap *bm) { return map_entry(0x3A, 0, n, pl, bm); }
int g3_light_lmap(int n, g3s_phandle *pl, grs_bitmap *bm) { return map_entry(0x46, 1, n, pl, bm); }
int g3_light_tmap(int n, g3s_phandle *pl, grs_bitmap *bm) { return map_entry(0x6C, 3, n, pl, bm); }
int g3_draw_tmap(int n, g3s_phandle *pl, grs_bitmap *bm) { return map_entry(0x60, 2, n, pl, bm); }
