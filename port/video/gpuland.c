/* gpuland.c -- Freefall: the GPU ground (FREEFALL_GPU_LAND=1, a first step of the GPU 3D view).
 *
 * While the game draws the main view's sharp picture (sharp3d.c port_s3d_render), the terrain's
 * polygon routines (weather.c) hand every polygon to port_gl_poly as well: its points in the
 * game's view space with their texels and light, its texture (64 x 64 colour numbers) and the
 * light table it is drawn through.  The game's drawing is not changed.  Each of the view's three
 * pictures (making, done, shown) has its own capture, with copies of the textures and light
 * tables it used, so the display never sees one being made and a picture it skips loses
 * nothing.  The captures come from the system's memory, not the game's heap (frame comparisons,
 * lockstep.c), as truecol.c's planes.
 *
 * The display (platform.c) then draws the picture's capture on the renderer's graphics card
 * (SDL_gpu, the device of SDL's "gpu" renderer): the points projected as the game projects them
 * but with the depth kept (w = z), so the textures and the light are interpolated
 * perspective-correct (no swimming) with a depth buffer instead of the game's far-to-near order,
 * in full colour: the light smooth between the table's rows, the texture filtered
 * (shaders/gpuland.frag.hlsl).  For now it is shown in place of the whole picture, to compare
 * with the game's (the objects, the sky and the water are not drawn yet).
 *
 * DXIL shaders only so far (Direct3D 12); without them the GPU ground stays off. */
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gpuland.h"
#include "shaders/gpuland_vert.dxil.h"
#include "shaders/gpuland.dxil.h"
#include "shaders/gpuland_mix.dxil.h"
#include "shaders/gpuwx_vert.dxil.h"
#include "shaders/gpuwx.dxil.h"
#include "shaders/gpuobj_vert.dxil.h"
#include "shaders/gpuobj.dxil.h"
#include "shaders/gpufx_vert.dxil.h"
#include "shaders/gpufx.dxil.h"

void port_log(const char *fmt, ...);

int port_gl;
int port_gl_capturing;
uint16_t *port_gl_sh;
uint16_t port_gl_sh_val = 0x100;
uint16_t *port_gl_shc;
uint16_t *port_gl_fxsh;
static uint16_t *fxsh_buf;              /* the effects' shadow (port_gl_fx's callers), as sh_buf */
static size_t fxsh_n;
static int fx_used;                     /* (cleared on its first use in a drawing) */
static volatile int fx_ready;           /* the card draws the effects: their callers may leave them out */
const uint8_t *port_gl_sh_base;
static uint16_t *sh_buf;                /* the shadow of the canvas (the game's thread) */
static size_t sh_n;

#define TEX_MAX 256                     /* textures in one picture (the planet's 64, blast marks) */
#define LT_MAX 64                       /* light tables in one picture (haze and fog levels) */
#define LT_SIZE 0x1800                  /* 24 rows of 256 */
#define TEX_SIZE 4096                   /* 64 x 64 */
#define NEAR_Z (1.0f / 64)
#define FAR_Z 8192.0f
#define MIPS 7                          /* 64 .. 1 */
#define OTEX_MAX 192                    /* objects' textures in one picture */
#define OTEX_DIM 256                    /* ... at most this wide and high (a bigger one stays the game's) */
#define OPOOL (4 << 20)                 /* ... their texels a picture, at most */
#define FX_MAX 160                      /* effects pack pictures kept on the card (the last used) */
#define FX_DIM 256                      /* ... at most this wide and high (a bigger one stays the game's) */
#define FXS_MAX 768                     /* effects in one picture */

/* an effects pack picture where the game placed it (port_gl_fx) */
typedef struct {
    const uint8_t *rgba;
    const void *key;
    int w, h, mirror, lit;
    float x0, y0, x1, y1, op, fit[6];
} FxSprite;
#define MIP_BYTES (4 * (64 * 64 + 32 * 32 + 16 * 16 + 8 * 8 + 4 * 4 + 2 * 2 + 1))
#define FIT_BYTES (2 * 24 * 16)

/* a point as the card gets it: x, y, z, light | u, v, texture layer, light table */
typedef struct {
    float a[4], b[4];
} GpuVert;

/* an object's point: as GpuVert, plus its texture's width, height and see-through (gpuobj shaders) */
typedef struct {
    float a[4], b[4], c[4];
} GpuObjVert;

typedef struct {
    int W, H;
    float hw, hh;
    GpuVert *v;
    size_t nv, cap;
    int ntex, nlt;
    const uint8_t *tex_ptr[TEX_MAX];
    const uint8_t *lt_ptr[LT_MAX];
    uint8_t (*tex)[TEX_SIZE];
    uint8_t (*lt)[LT_SIZE];
    unsigned long dropped;
    unsigned serial;
    int wx_kind, wx_colour;             /* the weather (port_gl_weather): -1 none, 0 rain, 1 snow */
    float wx_density, wx_wind, wx_time, cam_x, cam_y;
    int fog, fog_colour;                /* the fog for each pixel (port_gl_view) */
    float fog_start, fog_end, m[9], mz[3];
    FxSprite fx[FXS_MAX];               /* the effects, in the order drawn */
    int nfx;
    GpuObjVert *ov;                     /* the objects' triangles, in the order drawn */
    size_t onv, ocap;
    int notex;
    const void *otex_key[OTEX_MAX];
    int otex_w[OTEX_MAX], otex_h[OTEX_MAX];
    size_t otex_at[OTEX_MAX];
    uint8_t *opool;                     /* their texels (OPOOL) */
    size_t opool_used;
    uint16_t *mask;                     /* W x H: what the 3D shows (1 the ground, 2 an object, 4 an effect
                                         * over it) << 8 | the canvas's colour then (port_gl_mask) */
    size_t mask_n;
    const uint8_t *canvas;
} Capture;

static Capture *cur;
static unsigned serial;
static int last_tex = -1, last_lt = -1;

/* ------------------------------------------------------------------ the game's thread */
void port_gl_begin(void **slot, const uint8_t *canvas, int W, int H, long half_w, long half_h)
{
    Capture *c = *slot;
    size_t n = (size_t)W * H;
    if (!port_gl || W <= 0 || H <= 0)
        return;
    if (n > sh_n) {                     /* the shadow, as big as the canvas */
        free(sh_buf);
        sh_buf = malloc(n * sizeof *sh_buf);
        sh_n = sh_buf ? n : 0;
        if (!sh_buf) {
            port_log("GPU ground: no memory for the canvas's shadow, off\n");
            port_gl = 0;
            return;
        }
    }
    memset(sh_buf, 0, n * sizeof *sh_buf);
    if (!c) {
        c = calloc(1, sizeof *c);
        if (c) {
            c->tex = malloc((size_t)TEX_MAX * TEX_SIZE);
            c->lt = malloc((size_t)LT_MAX * LT_SIZE);
            c->opool = malloc(OPOOL);
        }
        if (!c || !c->tex || !c->lt || !c->opool) {
            port_log("GPU ground: no memory for the capture, off\n");
            port_gl = 0;
            return;
        }
        *slot = c;
    }
    if (n > c->mask_n) {
        free(c->mask);
        c->mask = malloc(n * sizeof *c->mask);
        c->mask_n = c->mask ? n : 0;
    }
    c->canvas = canvas;
    port_gl_sh_base = canvas;
    c->W = W;
    c->H = H;
    c->hw = (float)half_w;
    c->hh = (float)half_h;
    c->nv = 0;
    c->ntex = c->nlt = 0;
    c->onv = 0;
    c->notex = 0;
    c->nfx = 0;
    fx_used = 0;
    if (n > fxsh_n) {                   /* the effects' shadow */
        free(fxsh_buf);
        fxsh_buf = malloc(n * sizeof *fxsh_buf);
        fxsh_n = fxsh_buf ? n : 0;
    }
    c->opool_used = 0;
    c->fog = 0;
    c->wx_kind = -1;
    c->dropped = 0;
    last_tex = last_lt = -1;
    cur = c;
    port_gl_capturing = 1;
    port_gl_shc = sh_buf;
    port_gl_fxsh = NULL;                /* (port_gl_fx gives it, cleared, on the drawing's first effect) */
}

void port_gl_view(const float m[9], const float mz[3], int fog, float start, float end, int colour)
{
    Capture *c = cur;
    if (!c)
        return;
    memcpy(c->m, m, sizeof c->m);
    memcpy(c->mz, mz, sizeof c->mz);
    c->fog = fog && end > start;
    c->fog_start = start;
    c->fog_end = end;
    c->fog_colour = colour;
}

void port_gl_weather(int kind, int colour, float density, float wind, float time, float cam_x, float cam_y)
{
    Capture *c = cur;
    if (!c)
        return;
    c->wx_kind = kind;
    c->wx_colour = colour;
    c->wx_density = density;
    c->wx_wind = wind;
    c->wx_time = time;
    c->cam_x = cam_x;
    c->cam_y = cam_y;
}

int port_gl_fx(const uint8_t *rgba, int w, int h, const void *key, float x0, float y0, float x1, float y1,
               int mirror, float op, const float *fit)
{
    Capture *c = cur;
    FxSprite *f;
    if (!c || !fx_ready || !rgba || !fxsh_buf || w <= 0 || h <= 0 || w > FX_DIM || h > FX_DIM || c->nfx == FXS_MAX)
        return 0;
    if (!fx_used) {
        memset(fxsh_buf, 0, (size_t)c->W * c->H * sizeof *fxsh_buf);
        fx_used = 1;
    }
    port_gl_fxsh = fxsh_buf;
    f = &c->fx[c->nfx++];
    f->rgba = rgba;
    f->key = key;
    f->w = w;
    f->h = h;
    f->mirror = mirror;
    f->x0 = x0;
    f->y0 = y0;
    f->x1 = x1;
    f->y1 = y1;
    f->op = op;
    f->lit = fit != NULL;
    if (fit)
        memcpy(f->fit, fit, sizeof f->fit);
    return 1;
}

int port_gl_obj_canvas(const void *bits)
{
    return cur && bits == cur->canvas;
}

static int lt_layer(Capture *c, const uint8_t *t);

static int otex_slot(Capture *c, const void *key, const uint8_t *bits, int w, int h)
{
    int i;
    for (i = 0; i < c->notex; i++)
        if (c->otex_key[i] == key && c->otex_w[i] == w && c->otex_h[i] == h)
            return i;
    if (c->notex == OTEX_MAX || c->opool_used + (size_t)w * h > OPOOL)
        return -1;
    c->otex_key[i] = key;
    c->otex_w[i] = w;
    c->otex_h[i] = h;
    c->otex_at[i] = c->opool_used;
    memcpy(c->opool + c->opool_used, bits, (size_t)w * h);
    c->opool_used += (size_t)w * h;
    c->notex++;
    return i;
}

void port_gl_obj(const PortGlObjVert *v, int n, const uint8_t *bits, int w, int h, int trans,
                 const uint8_t *ltab, const void *key)
{
    Capture *c = cur;
    int k, j, layer, lt = -1;
    if (!c || n < 3 || w <= 0 || h <= 0 || w > OTEX_DIM || h > OTEX_DIM || c->hw <= 0 || c->hh <= 0)
        return;
    layer = otex_slot(c, key, bits, w, h);
    if (layer < 0) {
        c->dropped++;
        return;
    }
    if (ltab) {
        lt = lt_layer(c, ltab);
        if (lt < 0) {
            c->dropped++;
            return;
        }
    }
    if (c->onv + (size_t)(n - 2) * 3 > c->ocap) {
        size_t cap = c->ocap ? c->ocap * 2 : 16384;
        GpuObjVert *nv;
        while (cap < c->onv + (size_t)(n - 2) * 3)
            cap *= 2;
        nv = realloc(c->ov, cap * sizeof *nv);
        if (!nv) {
            c->dropped++;
            return;
        }
        c->ov = nv;
        c->ocap = cap;
    }
    for (k = 1; k + 1 < n; k++) {       /* a fan from the first point */
        const PortGlObjVert *p[3] = { &v[0], &v[k], &v[k + 1] };
        for (j = 0; j < 3; j++) {
            GpuObjVert *o = &c->ov[c->onv++];
            float z = p[j]->z > 1e-4f ? p[j]->z : 1e-4f;
            /* back from the canvas to the view space the ground's projection takes (w = z: the
             * texture's perspective; its size is the 3D library's, the depth unused) */
            o->a[0] = (p[j]->sx / c->hw - 1) * z;
            o->a[1] = (p[j]->sy / c->hh - 1) * z;
            o->a[2] = z;
            o->a[3] = p[j]->shade;
            o->b[0] = p[j]->u;
            o->b[1] = p[j]->v;
            o->b[2] = (float)layer;
            o->b[3] = (float)lt;
            o->c[0] = (float)w;
            o->c[1] = (float)h;
            o->c[2] = (float)(trans != 0);
            o->c[3] = 0;
        }
    }
}

void port_gl_shadow(int on)
{
    port_gl_sh = on && cur ? sh_buf : NULL;
}

void port_gl_end(void)
{
    Capture *c = cur;
    if (!c)
        return;
    port_gl_sh = NULL;
    port_gl_shc = NULL;
    port_gl_fxsh = NULL;
    if (c->mask) {                      /* the ground: drawn by the terrain, not drawn over since */
        size_t i, n = (size_t)c->W * c->H;
        for (i = 0; i < n; i++)
            c->mask[i] = (uint16_t)(((sh_buf[i] & 0x300) && (uint8_t)sh_buf[i] == c->canvas[i] ? (sh_buf[i] & 0x300) : 0) |
                                    c->canvas[i]);
        if (fx_used)                    /* an effect over it: nothing drawn there since (4) */
            for (i = 0; i < n; i++)
                if ((fxsh_buf[i] & 0x100) && (uint8_t)fxsh_buf[i] == c->canvas[i])
                    c->mask[i] |= 0x400;
    }
    c->serial = ++serial;
    port_gl_capturing = 0;
    cur = NULL;
}

static int tex_layer(Capture *c, const uint8_t *t)
{
    int i;
    if (last_tex >= 0 && c->tex_ptr[last_tex] == t)
        return last_tex;
    for (i = 0; i < c->ntex; i++)
        if (c->tex_ptr[i] == t)
            return last_tex = i;
    if (c->ntex == TEX_MAX)
        return -1;
    c->tex_ptr[i] = t;
    memcpy(c->tex[i], t, TEX_SIZE);
    c->ntex++;
    return last_tex = i;
}

static int lt_layer(Capture *c, const uint8_t *t)
{
    int i;
    if (last_lt >= 0 && c->lt_ptr[last_lt] == t)
        return last_lt;
    for (i = 0; i < c->nlt; i++)
        if (c->lt_ptr[i] == t)
            return last_lt = i;
    if (c->nlt == LT_MAX)
        return -2;
    c->lt_ptr[i] = t;
    memcpy(c->lt[i], t, LT_SIZE);
    c->nlt++;
    return last_lt = i;
}

void port_gl_poly(const PortGlVert *v, int n, const uint8_t *tex, int colour, const uint8_t *ltab)
{
    Capture *c = cur;
    float layer, lt;
    int k, j;
    if (!c || n < 3)
        return;
    if (tex) {
        int l = tex_layer(c, tex);
        if (l < 0) {
            c->dropped++;
            return;
        }
        layer = (float)l;
    } else
        layer = (float)(-1 - (colour & 0xFF));
    if (ltab) {
        int l = lt_layer(c, ltab);
        if (l < -1) {
            c->dropped++;
            return;
        }
        lt = (float)l;
    } else
        lt = -1.0f;
    if (c->nv + (size_t)(n - 2) * 3 > c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 65536;
        GpuVert *nvp;
        while (cap < c->nv + (size_t)(n - 2) * 3)
            cap *= 2;
        nvp = realloc(c->v, cap * sizeof *nvp);
        if (!nvp) {
            c->dropped++;
            return;
        }
        c->v = nvp;
        c->cap = cap;
    }
    for (k = 1; k + 1 < n; k++) {       /* a fan from the first point */
        const PortGlVert *p[3] = { &v[0], &v[k], &v[k + 1] };
        for (j = 0; j < 3; j++) {
            GpuVert *o = &c->v[c->nv++];
            o->a[0] = p[j]->x;
            o->a[1] = p[j]->y;
            o->a[2] = p[j]->z;
            o->a[3] = p[j]->shade;
            o->b[0] = p[j]->u;
            o->b[1] = p[j]->v;
            o->b[2] = layer;
            o->b[3] = lt;
        }
    }
}

/* ------------------------------------------------------------------ the display's thread */
static SDL_Renderer *g_r;
static SDL_GPUDevice *dev;
static int broken;
static SDL_GPUGraphicsPipeline *pipe;
static SDL_GPUGraphicsPipeline *wx_pipe;   /* the weather's particles (gpuwx shaders) */
static SDL_GPUTexture *wx_tex;          /* ... their layer, the picture's size (premultiplied) */
int port_gl_wx_on;                      /* the card draws the weather: the game's drops left out */
static SDL_GPUGraphicsPipeline *obj_pipe;  /* the objects (gpuobj shaders) */
static SDL_GPUTexture *otex, *obj_tex;  /* ... their textures (256 x 256 slots), their layer */
static SDL_GPUBuffer *obuf;
static Uint32 obuf_size;
static SDL_GPUGraphicsPipeline *fx_pipe;   /* the effects pack's pictures (gpufx shaders) */
static SDL_GPUTexture *fx_pics, *fx_tex;   /* ... kept pictures (FX_DIM slots), their layer */
static SDL_GPUSampler *linclamp;
static SDL_GPUBuffer *fxbuf;
static Uint32 fxbuf_size;
static const void *fx_key[FX_MAX];      /* the pictures in the slots, when each was last used */
static unsigned fx_age[FX_MAX], fx_clock;
static SDL_GPUSampler *point;
static SDL_GPUTexture *texels, *ltabs, *paltex, *color, *depth;
static SDL_GPUTexture *colours, *fits;  /* the textures in colour (mipmapped), the rows' fits */
static SDL_GPUSampler *trilin;
static SDL_GPUTextureFormat depth_fmt;
static SDL_GPUBuffer *vbuf;
static Uint32 vbuf_size;
static SDL_GPUTransferBuffer *tbuf;
static Uint32 tbuf_size;
static SDL_Texture *shown;
static int tw, th;
static unsigned drawn_serial;
static int filter = 1;
static SDL_GPURenderState *mix;         /* the ground put into the picture (port_gl_state) */
static SDL_GPUShader *mix_sh;
static SDL_GPUTexture *mix_for;         /* ... the colour target it was made with */

/* GPU rendering cannot go on with this renderer: the game draws everything again (the weather's
 * drops, the effects) */
static void gl_off(void)
{
    broken = 1;
    port_gl_wx_on = 0;
    fx_ready = 0;
}

static void forget(void)
{
    /* the renderer (and with it its device and textures) is gone or new: start again */
    g_r = NULL;
    dev = NULL;
    pipe = wx_pipe = obj_pipe = fx_pipe = NULL;
    wx_tex = otex = obj_tex = fx_pics = fx_tex = NULL;
    linclamp = NULL;
    obuf = fxbuf = NULL;
    obuf_size = fxbuf_size = 0;
    memset(fx_key, 0, sizeof fx_key);
    port_gl_wx_on = 0;
    fx_ready = 0;
    point = trilin = NULL;
    texels = ltabs = paltex = color = depth = colours = fits = NULL;
    vbuf = NULL;
    tbuf = NULL;
    vbuf_size = tbuf_size = 0;
    shown = NULL;
    tw = th = 0;
    drawn_serial = 0;
    mix = NULL;
    mix_sh = NULL;
    mix_for = NULL;
}

void port_gl_shutdown(void)
{
    if (dev) {
        if (mix)
            SDL_DestroyGPURenderState(mix);
        if (mix_sh)
            SDL_ReleaseGPUShader(dev, mix_sh);
        if (shown)
            SDL_DestroyTexture(shown);
        if (pipe)
            SDL_ReleaseGPUGraphicsPipeline(dev, pipe);
        if (wx_pipe)
            SDL_ReleaseGPUGraphicsPipeline(dev, wx_pipe);
        if (wx_tex)
            SDL_ReleaseGPUTexture(dev, wx_tex);
        if (obj_pipe)
            SDL_ReleaseGPUGraphicsPipeline(dev, obj_pipe);
        if (otex)
            SDL_ReleaseGPUTexture(dev, otex);
        if (obj_tex)
            SDL_ReleaseGPUTexture(dev, obj_tex);
        if (obuf)
            SDL_ReleaseGPUBuffer(dev, obuf);
        if (fx_pipe)
            SDL_ReleaseGPUGraphicsPipeline(dev, fx_pipe);
        if (fx_pics)
            SDL_ReleaseGPUTexture(dev, fx_pics);
        if (fx_tex)
            SDL_ReleaseGPUTexture(dev, fx_tex);
        if (linclamp)
            SDL_ReleaseGPUSampler(dev, linclamp);
        if (fxbuf)
            SDL_ReleaseGPUBuffer(dev, fxbuf);
        if (point)
            SDL_ReleaseGPUSampler(dev, point);
        if (trilin)
            SDL_ReleaseGPUSampler(dev, trilin);
        if (colours)
            SDL_ReleaseGPUTexture(dev, colours);
        if (fits)
            SDL_ReleaseGPUTexture(dev, fits);
        if (texels)
            SDL_ReleaseGPUTexture(dev, texels);
        if (ltabs)
            SDL_ReleaseGPUTexture(dev, ltabs);
        if (paltex)
            SDL_ReleaseGPUTexture(dev, paltex);
        if (color)
            SDL_ReleaseGPUTexture(dev, color);
        if (depth)
            SDL_ReleaseGPUTexture(dev, depth);
        if (vbuf)
            SDL_ReleaseGPUBuffer(dev, vbuf);
        if (tbuf)
            SDL_ReleaseGPUTransferBuffer(dev, tbuf);
    }
    forget();
}

static SDL_GPUTexture *make_tex(SDL_GPUTextureType type, SDL_GPUTextureFormat fmt, SDL_GPUTextureUsageFlags usage,
                                int w, int h, int layers, int levels)
{
    SDL_GPUTextureCreateInfo ti;
    SDL_zero(ti);
    ti.type = type;
    ti.format = fmt;
    ti.usage = usage;
    ti.width = (Uint32)w;
    ti.height = (Uint32)h;
    ti.layer_count_or_depth = (Uint32)layers;
    ti.num_levels = (Uint32)levels;
    return SDL_CreateGPUTexture(dev, &ti);
}

static int setup(SDL_Renderer *r)
{
    SDL_GPUShaderCreateInfo si;
    SDL_GPUShader *vs, *fs;
    SDL_GPUGraphicsPipelineCreateInfo pi;
    SDL_GPUVertexBufferDescription vb;
    SDL_GPUVertexAttribute va[2];
    SDL_GPUColorTargetDescription ct;
    SDL_GPUSamplerCreateInfo sci;
    const char *f;

    dev = SDL_GetGPURendererDevice(r);
    if (!dev) {
        port_log("GPU ground: the renderer has no graphics card device (Renderer: %s), off\n", SDL_GetRendererName(r));
        return 0;
    }
    if (!(SDL_GetGPUShaderFormats(dev) & SDL_GPU_SHADERFORMAT_DXIL)) {
        port_log("GPU ground: %s takes no DXIL shaders (only Direct3D 12 for now), off\n", SDL_GetGPUDeviceDriver(dev));
        return 0;
    }
    g_r = r;
    if ((f = SDL_getenv("FREEFALL_GPU_LAND_FILTER")) != NULL)
        filter = *f != '0';

    SDL_zero(si);
    si.format = SDL_GPU_SHADERFORMAT_DXIL;
    si.entrypoint = "main";
    si.code = gpuland_vert_dxil;
    si.code_size = gpuland_vert_dxil_len;
    si.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    si.num_uniform_buffers = 1;
    vs = SDL_CreateGPUShader(dev, &si);
    SDL_zero(si);
    si.format = SDL_GPU_SHADERFORMAT_DXIL;
    si.entrypoint = "main";
    si.code = gpuland_dxil;
    si.code_size = gpuland_dxil_len;
    si.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    si.num_samplers = 5;
    si.num_uniform_buffers = 1;
    fs = SDL_CreateGPUShader(dev, &si);
    if (!vs || !fs) {
        port_log("GPU ground: shaders not made (%s), off\n", SDL_GetError());
        return 0;
    }

    depth_fmt = SDL_GPUTextureSupportsFormat(dev, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTURETYPE_2D,
                                             SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
                ? SDL_GPU_TEXTUREFORMAT_D32_FLOAT : SDL_GPU_TEXTUREFORMAT_D24_UNORM;
    SDL_zero(pi);
    SDL_zero(vb);
    vb.slot = 0;
    vb.pitch = sizeof(GpuVert);
    vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_zero(va);
    va[0].location = 0;
    va[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    va[0].offset = 0;
    va[1].location = 1;
    va[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    va[1].offset = 16;
    SDL_zero(ct);
    ct.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    pi.vertex_shader = vs;
    pi.fragment_shader = fs;
    pi.vertex_input_state.vertex_buffer_descriptions = &vb;
    pi.vertex_input_state.num_vertex_buffers = 1;
    pi.vertex_input_state.vertex_attributes = va;
    pi.vertex_input_state.num_vertex_attributes = 2;
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pi.depth_stencil_state.enable_depth_test = true;
    pi.depth_stencil_state.enable_depth_write = true;
    pi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    pi.target_info.color_target_descriptions = &ct;
    pi.target_info.num_color_targets = 1;
    pi.target_info.depth_stencil_format = depth_fmt;
    pi.target_info.has_depth_stencil_target = true;
    pipe = SDL_CreateGPUGraphicsPipeline(dev, &pi);
    SDL_ReleaseGPUShader(dev, vs);
    SDL_ReleaseGPUShader(dev, fs);
    {   /* the weather: no vertices (made from the instance), blended premultiplied, under the
         * ground's depth (hills hide it), writing none */
        SDL_GPUShader *wvs, *wfs;
        SDL_GPUGraphicsPipelineCreateInfo wi = pi;
        SDL_GPUColorTargetDescription wct = ct;
        SDL_zero(si);
        si.format = SDL_GPU_SHADERFORMAT_DXIL;
        si.entrypoint = "main";
        si.code = gpuwx_vert_dxil;
        si.code_size = gpuwx_vert_dxil_len;
        si.stage = SDL_GPU_SHADERSTAGE_VERTEX;
        si.num_uniform_buffers = 1;
        wvs = SDL_CreateGPUShader(dev, &si);
        si.code = gpuwx_dxil;
        si.code_size = gpuwx_dxil_len;
        si.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        wfs = SDL_CreateGPUShader(dev, &si);
        wct.blend_state.enable_blend = true;
        wct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        wct.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        wct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        wct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        wct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        wct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        wi.vertex_shader = wvs;
        wi.fragment_shader = wfs;
        SDL_zero(wi.vertex_input_state);
        wi.depth_stencil_state.enable_depth_write = false;
        wi.target_info.color_target_descriptions = &wct;
        wx_pipe = wvs && wfs ? SDL_CreateGPUGraphicsPipeline(dev, &wi) : NULL;
        if (wvs)
            SDL_ReleaseGPUShader(dev, wvs);
        if (wfs)
            SDL_ReleaseGPUShader(dev, wfs);
    }
    {   /* the objects: three attributes, drawn in the game's order (no depth), opaque */
        SDL_GPUShader *ovs, *ofs;
        SDL_GPUGraphicsPipelineCreateInfo oi = pi;
        SDL_GPUVertexBufferDescription ovb = vb;
        SDL_GPUVertexAttribute ova[3];
        SDL_zero(si);
        si.format = SDL_GPU_SHADERFORMAT_DXIL;
        si.entrypoint = "main";
        si.code = gpuobj_vert_dxil;
        si.code_size = gpuobj_vert_dxil_len;
        si.stage = SDL_GPU_SHADERSTAGE_VERTEX;
        si.num_uniform_buffers = 1;
        ovs = SDL_CreateGPUShader(dev, &si);
        SDL_zero(si);
        si.format = SDL_GPU_SHADERFORMAT_DXIL;
        si.entrypoint = "main";
        si.code = gpuobj_dxil;
        si.code_size = gpuobj_dxil_len;
        si.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        si.num_samplers = 3;
        ofs = SDL_CreateGPUShader(dev, &si);
        ovb.pitch = sizeof(GpuObjVert);
        ova[0] = va[0];
        ova[1] = va[1];
        ova[2] = va[1];
        ova[2].location = 2;
        ova[2].offset = 32;
        oi.vertex_shader = ovs;
        oi.fragment_shader = ofs;
        oi.vertex_input_state.vertex_buffer_descriptions = &ovb;
        oi.vertex_input_state.vertex_attributes = ova;
        oi.vertex_input_state.num_vertex_attributes = 3;
        oi.depth_stencil_state.enable_depth_test = false;
        oi.depth_stencil_state.enable_depth_write = false;
        oi.target_info.has_depth_stencil_target = false;
        oi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_INVALID;
        obj_pipe = ovs && ofs ? SDL_CreateGPUGraphicsPipeline(dev, &oi) : NULL;
        if (ovs)
            SDL_ReleaseGPUShader(dev, ovs);
        if (ofs)
            SDL_ReleaseGPUShader(dev, ofs);
        otex = make_tex(SDL_GPU_TEXTURETYPE_2D_ARRAY, SDL_GPU_TEXTUREFORMAT_R8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER,
                        OTEX_DIM, OTEX_DIM, OTEX_MAX, 1);
    }
    {   /* the effects: four attributes, premultiplied over each other in the game's order */
        SDL_GPUShader *fvs, *ffs;
        SDL_GPUGraphicsPipelineCreateInfo fi = pi;
        SDL_GPUVertexBufferDescription fvb = vb;
        SDL_GPUVertexAttribute fva[4];
        SDL_GPUColorTargetDescription fct = ct;
        SDL_GPUSamplerCreateInfo lsi;
        int k;
        SDL_zero(si);
        si.format = SDL_GPU_SHADERFORMAT_DXIL;
        si.entrypoint = "main";
        si.code = gpufx_vert_dxil;
        si.code_size = gpufx_vert_dxil_len;
        si.stage = SDL_GPU_SHADERSTAGE_VERTEX;
        si.num_uniform_buffers = 1;
        fvs = SDL_CreateGPUShader(dev, &si);
        SDL_zero(si);
        si.format = SDL_GPU_SHADERFORMAT_DXIL;
        si.entrypoint = "main";
        si.code = gpufx_dxil;
        si.code_size = gpufx_dxil_len;
        si.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        si.num_samplers = 1;
        ffs = SDL_CreateGPUShader(dev, &si);
        fvb.pitch = 64;
        for (k = 0; k < 4; k++) {
            fva[k] = va[0];
            fva[k].location = (Uint32)k;
            fva[k].offset = (Uint32)(16 * k);
        }
        fct.blend_state.enable_blend = true;
        fct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        fct.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        fct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        fct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        fct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        fct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        fi.vertex_shader = fvs;
        fi.fragment_shader = ffs;
        fi.vertex_input_state.vertex_buffer_descriptions = &fvb;
        fi.vertex_input_state.vertex_attributes = fva;
        fi.vertex_input_state.num_vertex_attributes = 4;
        fi.target_info.color_target_descriptions = &fct;
        fi.depth_stencil_state.enable_depth_test = false;
        fi.depth_stencil_state.enable_depth_write = false;
        fi.target_info.has_depth_stencil_target = false;
        fi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_INVALID;
        fx_pipe = fvs && ffs ? SDL_CreateGPUGraphicsPipeline(dev, &fi) : NULL;
        if (fvs)
            SDL_ReleaseGPUShader(dev, fvs);
        if (ffs)
            SDL_ReleaseGPUShader(dev, ffs);
        fx_pics = make_tex(SDL_GPU_TEXTURETYPE_2D_ARRAY, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER,
                           FX_DIM, FX_DIM, FX_MAX, 1);
        SDL_zero(lsi);
        lsi.min_filter = lsi.mag_filter = SDL_GPU_FILTER_LINEAR;
        lsi.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        lsi.address_mode_u = lsi.address_mode_v = lsi.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        linclamp = SDL_CreateGPUSampler(dev, &lsi);
        memset(fx_key, 0, sizeof fx_key);
    }
    if (!pipe) {
        port_log("GPU ground: pipeline not made (%s), off\n", SDL_GetError());
        return 0;
    }

    SDL_zero(sci);
    sci.min_filter = sci.mag_filter = SDL_GPU_FILTER_NEAREST;
    sci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sci.address_mode_u = sci.address_mode_v = sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    point = SDL_CreateGPUSampler(dev, &sci);
    sci.min_filter = sci.mag_filter = SDL_GPU_FILTER_LINEAR;
    sci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sci.address_mode_u = sci.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sci.max_lod = 16;
    sci.enable_anisotropy = true;       /* the ground seen at a low angle: sharp along it, not smeared */
    sci.max_anisotropy = 8;
    trilin = SDL_CreateGPUSampler(dev, &sci);
    colours = make_tex(SDL_GPU_TEXTURETYPE_2D_ARRAY, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER,
                       64, 64, TEX_MAX, MIPS);
    fits = make_tex(SDL_GPU_TEXTURETYPE_2D_ARRAY, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT, SDL_GPU_TEXTUREUSAGE_SAMPLER,
                    2, 24, LT_MAX, 1);
    texels = make_tex(SDL_GPU_TEXTURETYPE_2D_ARRAY, SDL_GPU_TEXTUREFORMAT_R8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER, 64, 64, TEX_MAX, 1);
    ltabs = make_tex(SDL_GPU_TEXTURETYPE_2D_ARRAY, SDL_GPU_TEXTUREFORMAT_R8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER, 256, 24, LT_MAX, 1);
    paltex = make_tex(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER, 256, 1, 1, 1);
    if (!point || !trilin || !texels || !ltabs || !paltex || !colours || !fits) {
        port_log("GPU ground: textures not made (%s), off\n", SDL_GetError());
        return 0;
    }
    port_gl_wx_on = wx_pipe != NULL;
    fx_ready = fx_pipe && fx_pics && linclamp;
    port_log("GPU rendering: on (%s, depth %s, texture filter %s, weather %s)\n", SDL_GetGPUDeviceDriver(dev),
             depth_fmt == SDL_GPU_TEXTUREFORMAT_D32_FLOAT ? "32-bit float" : "24-bit", filter ? "on" : "off",
             wx_pipe ? "on" : "the game's");
    return 1;
}

/* the picture's size: the card's colour and depth targets and the renderer's texture of the
 * colour one */
static int targets(int W, int H)
{
    SDL_PropertiesID p;
    if (shown && tw == W && th == H)
        return 1;
    if (shown)
        SDL_DestroyTexture(shown);
    if (color)
        SDL_ReleaseGPUTexture(dev, color);
    if (depth)
        SDL_ReleaseGPUTexture(dev, depth);
    if (wx_tex)
        SDL_ReleaseGPUTexture(dev, wx_tex);
    if (obj_tex)
        SDL_ReleaseGPUTexture(dev, obj_tex);
    if (fx_tex)
        SDL_ReleaseGPUTexture(dev, fx_tex);
    shown = NULL;
    color = make_tex(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                     SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, W, H, 1, 1);
    depth = make_tex(SDL_GPU_TEXTURETYPE_2D, depth_fmt, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, W, H, 1, 1);
    wx_tex = make_tex(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                      SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, W, H, 1, 1);
    obj_tex = make_tex(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                       SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, W, H, 1, 1);
    fx_tex = make_tex(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                      SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, W, H, 1, 1);
    if (!color || !depth || !wx_tex || !obj_tex || !fx_tex)
        return 0;
    p = SDL_CreateProperties();
    SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_ABGR8888);
    SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STATIC);
    SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, W);
    SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, H);
    SDL_SetPointerProperty(p, SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER, color);
    shown = SDL_CreateTextureWithProperties(g_r, p);
    SDL_DestroyProperties(p);
    if (!shown) {
        port_log("GPU ground: the picture's texture not made (%s)\n", SDL_GetError());
        return 0;
    }
    SDL_SetTextureBlendMode(shown, SDL_BLENDMODE_NONE);
    tw = W;
    th = H;
    return 1;
}

static int buffers(Uint32 vbytes, Uint32 tbytes)
{
    if (vbytes > vbuf_size) {
        SDL_GPUBufferCreateInfo bi;
        if (vbuf)
            SDL_ReleaseGPUBuffer(dev, vbuf);
        SDL_zero(bi);
        bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        bi.size = vbytes + vbytes / 2;
        vbuf = SDL_CreateGPUBuffer(dev, &bi);
        vbuf_size = vbuf ? bi.size : 0;
    }
    if (tbytes > tbuf_size) {
        SDL_GPUTransferBufferCreateInfo ti;
        if (tbuf)
            SDL_ReleaseGPUTransferBuffer(dev, tbuf);
        SDL_zero(ti);
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        ti.size = tbytes + tbytes / 2;
        tbuf = SDL_CreateGPUTransferBuffer(dev, &ti);
        tbuf_size = tbuf ? ti.size : 0;
    }
    return vbuf && tbuf;
}

/* a texture's colours (64 x 64 colour numbers through the palette) and its mipmaps down to 1 x 1,
 * one after the other (MIP_BYTES) */
static void make_mips(uint8_t *d, const uint8_t *idx, const uint32_t *pal)
{
    int i, x, y, n = 64;
    for (i = 0; i < 64 * 64; i++) {
        uint32_t c = pal[idx[i]];
        d[i * 4 + 0] = (uint8_t)(c >> 16);
        d[i * 4 + 1] = (uint8_t)(c >> 8);
        d[i * 4 + 2] = (uint8_t)c;
        d[i * 4 + 3] = 255;
    }
    while (n > 1) {
        uint8_t *s = d, *o = d + (size_t)n * n * 4;
        int h = n / 2, k;
        for (y = 0; y < h; y++)
            for (x = 0; x < h; x++)
                for (k = 0; k < 4; k++) {
                    const uint8_t *a = s + ((size_t)(2 * y) * n + 2 * x) * 4 + k;
                    o[((size_t)y * h + x) * 4 + k] = (uint8_t)((a[0] + a[4] + a[n * 4] + a[n * 4 + 4] + 2) / 4);
                }
        d = o;
        n = h;
    }
}

/* each row of a light table as colour * a + b per channel (least squares over the colours, each
 * weighted by how many texels of this picture's textures have it: the far land's mipmaps mix
 * those, and a fit over the whole palette missed the haze rows' grey); row r's a at texel (0, r),
 * b at (1, r), RGBA floats */
static void make_fit(float *d, const uint8_t *lt, const uint32_t *pal, const double *wt)
{
    int r, c, k;
    for (r = 0; r < 24; r++)
        for (k = 0; k < 3; k++) {
            int sh = 16 - 8 * k;
            double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0, a, b;
            for (c = 0; c < 256; c++) {
                double x = ((pal[c] >> sh) & 255) / 255.0, y = ((pal[lt[r * 256 + c]] >> sh) & 255) / 255.0, w = wt[c];
                sw += w;
                sx += w * x;
                sy += w * y;
                sxx += w * x * x;
                sxy += w * x * y;
            }
            a = sxx * sw - sx * sx > 1e-9 ? (sxy * sw - sx * sy) / (sxx * sw - sx * sx) : 0;
            b = sw > 0 ? (sy - a * sx) / sw : 0;
            d[(r * 2 + 0) * 4 + k] = (float)a;
            d[(r * 2 + 1) * 4 + k] = (float)b;
        }
    for (r = 0; r < 24; r++)
        d[(r * 2 + 0) * 4 + 3] = d[(r * 2 + 1) * 4 + 3] = 0;
}

/* the weather (gpuwx shaders): its layer cleared, and the particles drawn into it under the
 * ground's depth.  A square of the map is about 8 m (a unit 2 m tall stands a quarter of one): the
 * rain falls at about 9 m a second, the snow at about 1 */
static void weather_pass(SDL_GPUCommandBuffer *cmd, const Capture *c, const float *cam, const uint32_t *pal)
{
    SDL_GPUColorTargetInfo wti;
    SDL_GPUDepthStencilTargetInfo wdi;
    SDL_GPURenderPass *wp;
    int draw = wx_pipe && (c->wx_kind == 0 || c->wx_kind == 1);
    SDL_zero(wti);
    wti.texture = wx_tex;
    wti.load_op = SDL_GPU_LOADOP_CLEAR;
    wti.store_op = SDL_GPU_STOREOP_STORE;
    SDL_zero(wdi);
    wdi.texture = depth;
    wdi.load_op = SDL_GPU_LOADOP_LOAD;
    wdi.store_op = SDL_GPU_STOREOP_DONT_CARE;
    wdi.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    wdi.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    wp = SDL_BeginGPURenderPass(cmd, &wti, 1, draw ? &wdi : NULL);
    if (draw) {
        const float *a = c->m;
        float u[52], f[8], c1 = a[1] * a[1] + a[4] * a[4] + a[7] * a[7];
        float det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) +
                    a[2] * (a[3] * a[7] - a[4] * a[6]);
        float rx = 0, ry = 0, rl, fall, snow = (float)c->wx_kind;
        uint32_t col = pal[c->wx_colour & 0xFF];
        Uint32 n;
        int i;
        if (det != 0) {                 /* the view's right on the map (its inverse's first column) */
            ry = (a[4] * a[8] - a[5] * a[7]) / det;     /* ey */
            rx = (a[3] * a[7] - a[4] * a[6]) / det;     /* ex */
        }
        rl = sqrtf(rx * rx + ry * ry);
        if (rl > 0) {
            rx /= rl;
            ry /= rl;
        }
        fall = snow ? 0.13f : 1.6f;
        memset(u, 0, sizeof u);
        for (i = 0; i < 4; i++)
            u[i] = cam[i];
        u[4] = cam[4];
        u[5] = cam[5];
        for (i = 0; i < 3; i++) {
            u[8 + i] = a[i];
            u[12 + i] = a[3 + i];
            u[16 + i] = a[6 + i];
            u[20 + i] = c->mz[i];
        }
        u[24] = c->cam_x;
        u[25] = c->cam_y;
        u[26] = c1 > 0 ? -(c->mz[0] * a[1] + c->mz[1] * a[4] + c->mz[2] * a[7]) / c1 : 0;    /* the viewer's height */
        u[27] = c->wx_time;
        u[28] = snow;
        u[29] = snow ? 10.0f : 14.0f;   /* the box across, its height (squares) */
        u[30] = snow ? 3.0f : 4.0f;
        u[32] = rx * c->wx_wind * fall; /* the game's drift: across the view */
        u[33] = ry * c->wx_wind * fall;
        u[34] = fall;
        u[35] = 0.12f;                  /* the streak: where the drop was 120 ms ago (the eye's blur) */
        u[36] = 0.014f;                 /* a flake (squares) */
        u[37] = 1.3f + c->W / 1200.0f;  /* a streak's width (pixels) */
        u[38] = (float)c->W;
        u[39] = (float)c->H;
        f[0] = (((col >> 16) & 255) / 255.0f) * 0.5f + 0.5f;    /* the game's drop colour, lighter */
        f[1] = (((col >> 8) & 255) / 255.0f) * 0.5f + 0.5f;
        f[2] = ((col & 255) / 255.0f) * 0.5f + 0.5f;
        f[3] = snow ? 1.0f : 0.5f;
        f[4] = snow;
        f[5] = f[6] = f[7] = 0;
        n = (Uint32)((snow ? 14000 : 8000) * c->wx_density);
        SDL_BindGPUGraphicsPipeline(wp, wx_pipe);
        SDL_PushGPUVertexUniformData(cmd, 0, u, sizeof u);
        SDL_PushGPUFragmentUniformData(cmd, 0, f, sizeof f);
        SDL_DrawGPUPrimitives(wp, 6, n, 0, 0);
    }
    SDL_EndGPURenderPass(wp);
}

/* the effects (port_gl_fx): each picture's slot on the card, uploading the ones it lacks (the
 * slot used longest ago makes room; not one this picture uses: then the effect is left out) */
static int fx_slot[FXS_MAX];
static int fx_new[FXS_MAX], fx_nnew;

static Uint32 fx_prepare(const Capture *c)
{
    Uint32 bytes = 0;
    int i, k;
    fx_clock++;
    fx_nnew = 0;
    for (i = 0; i < c->nfx; i++) {
        const FxSprite *f = &c->fx[i];
        int best = -1;
        for (k = 0; k < FX_MAX; k++)
            if (fx_key[k] == f->key)
                break;
        if (k == FX_MAX) {
            for (k = 0; k < FX_MAX; k++)
                if (fx_age[k] != fx_clock && (best < 0 || !fx_key[k] || fx_age[k] < fx_age[best]))
                    best = k;
            k = best;
            if (k >= 0) {
                fx_key[k] = f->key;
                fx_new[fx_nnew++] = i;
                bytes += (Uint32)f->w * f->h * 4;
            }
        }
        fx_slot[i] = k;
        if (k >= 0)
            fx_age[k] = fx_clock;
    }
    return bytes + (Uint32)c->nfx * 6 * 64;
}

/* their new pictures and their rectangles into the transfer buffer at m */
static void fx_fill(const Capture *c, uint8_t *m)
{
    int i, j;
    float *v;
    for (i = 0; i < fx_nnew; i++) {
        const FxSprite *f = &c->fx[fx_new[i]];
        memcpy(m, f->rgba, (size_t)f->w * f->h * 4);
        m += (size_t)f->w * f->h * 4;
    }
    v = (float *)m;
    for (i = 0; i < c->nfx; i++) {
        const FxSprite *f = &c->fx[i];
        static const float q[6][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 0, 1 } };
        float us = (float)f->w / FX_DIM, vs = (float)f->h / FX_DIM;
        for (j = 0; j < 6; j++, v += 16) {
            float qx = q[j][0], qy = q[j][1];
            memset(v, 0, 16 * sizeof *v);
            if (fx_slot[i] < 0)         /* (no slot: left out, off the canvas) */
                continue;
            v[0] = f->x0 + (f->x1 - f->x0) * qx;
            v[1] = f->y0 + (f->y1 - f->y0) * qy;
            v[2] = (float)fx_slot[i];
            v[4] = (f->mirror ? 1 - qx : qx) * us;
            v[5] = qy * vs;
            v[8] = f->fit[0];
            v[9] = f->fit[1];
            v[10] = f->fit[2];
            v[11] = f->op;
            v[12] = f->fit[3];
            v[13] = f->fit[4];
            v[14] = f->fit[5];
            v[15] = (float)f->lit;
        }
    }
}

static void fx_upload(const Capture *c, SDL_GPUCopyPass *cp, Uint32 off)
{
    int i;
    for (i = 0; i < fx_nnew; i++) {
        const FxSprite *f = &c->fx[fx_new[i]];
        SDL_GPUTextureTransferInfo src = { tbuf, off, (Uint32)f->w, (Uint32)f->h };
        SDL_GPUTextureRegion dst;
        SDL_zero(dst);
        dst.texture = fx_pics;
        dst.layer = (Uint32)fx_slot[fx_new[i]];
        dst.w = (Uint32)f->w;
        dst.h = (Uint32)f->h;
        dst.d = 1;
        SDL_UploadToGPUTexture(cp, &src, &dst, false);
        off += (Uint32)f->w * f->h * 4;
    }
    if (c->nfx) {
        SDL_GPUTransferBufferLocation src = { tbuf, off };
        SDL_GPUBufferRegion dst = { fxbuf, 0, (Uint32)c->nfx * 6 * 64 };
        SDL_UploadToGPUBuffer(cp, &src, &dst, true);
    }
}

static void fx_pass(SDL_GPUCommandBuffer *cmd, const Capture *c)
{
    SDL_GPUColorTargetInfo ti;
    SDL_GPURenderPass *rp;
    SDL_zero(ti);
    ti.texture = fx_tex;
    ti.load_op = SDL_GPU_LOADOP_CLEAR;
    ti.store_op = SDL_GPU_STOREOP_STORE;
    rp = SDL_BeginGPURenderPass(cmd, &ti, 1, NULL);
    if (fx_pipe && fx_pics && linclamp && c->nfx) {
        SDL_GPUTextureSamplerBinding sb = { fx_pics, linclamp };
        SDL_GPUBufferBinding bb = { fxbuf, 0 };
        float size[4] = { (float)c->W, (float)c->H, 0, 0 };
        SDL_BindGPUGraphicsPipeline(rp, fx_pipe);
        SDL_BindGPUVertexBuffers(rp, 0, &bb, 1);
        SDL_BindGPUFragmentSamplers(rp, 0, &sb, 1);
        SDL_PushGPUVertexUniformData(cmd, 0, size, sizeof size);
        SDL_DrawGPUPrimitives(rp, (Uint32)c->nfx * 6, 1, 0, 0);
    }
    SDL_EndGPURenderPass(rp);
}

SDL_Texture *port_gl_render(SDL_Renderer *r, void *capture, const uint32_t *pal)
{
    Capture *c = capture;
    Uint32 vbytes, tbytes, off, obytes, ooff, fxbytes;
    uint8_t *m;
    SDL_GPUCommandBuffer *cmd;
    SDL_GPUCopyPass *cp;
    SDL_GPURenderPass *rp;
    SDL_GPUColorTargetInfo cti;
    SDL_GPUDepthStencilTargetInfo dti;
    SDL_GPUTextureSamplerBinding sb[5];
    SDL_GPUBufferBinding bb;
    float cam[8], look[28];
    int i;

    if (!port_gl || broken || !c || c->W <= 0 || c->H <= 0)
        return NULL;
    if (r != g_r) {                     /* a new renderer: its device's things are gone with the old */
        forget();
        if (!setup(r)) {
            gl_off();
            return NULL;
        }
    }
    if (!targets(c->W, c->H))
        return NULL;
    if (c->serial == drawn_serial)
        return shown;
    drawn_serial = c->serial;

    vbytes = (Uint32)(c->nv * sizeof(GpuVert));
    obytes = (Uint32)(c->onv * sizeof(GpuObjVert));
    fxbytes = fx_prepare(c);
    tbytes = (Uint32)c->ntex * (TEX_SIZE + MIP_BYTES) + (Uint32)c->nlt * (LT_SIZE + FIT_BYTES) + 1024 + vbytes +
             (Uint32)c->opool_used + obytes + fxbytes;
    if ((Uint32)c->nfx * 6 * 64 > fxbuf_size) {     /* the effects' rectangles */
        SDL_GPUBufferCreateInfo bi;
        if (fxbuf)
            SDL_ReleaseGPUBuffer(dev, fxbuf);
        SDL_zero(bi);
        bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        bi.size = (Uint32)FXS_MAX * 6 * 64;
        fxbuf = SDL_CreateGPUBuffer(dev, &bi);
        fxbuf_size = fxbuf ? bi.size : 0;
        if (!fxbuf)
            return NULL;
    }
    if (obytes > obuf_size) {           /* the objects' vertices */
        SDL_GPUBufferCreateInfo bi;
        if (obuf)
            SDL_ReleaseGPUBuffer(dev, obuf);
        SDL_zero(bi);
        bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        bi.size = obytes + obytes / 2;
        obuf = SDL_CreateGPUBuffer(dev, &bi);
        obuf_size = obuf ? bi.size : 0;
        if (!obuf)
            return NULL;
    }
    if (!buffers(vbytes ? vbytes : 64, tbytes))
        return NULL;
    m = SDL_MapGPUTransferBuffer(dev, tbuf, true);
    if (!m)
        return NULL;
    off = 0;
    memcpy(m, c->tex, (size_t)c->ntex * TEX_SIZE);
    off += (Uint32)c->ntex * TEX_SIZE;
    memcpy(m + off, c->lt, (size_t)c->nlt * LT_SIZE);
    off += (Uint32)c->nlt * LT_SIZE;
    for (i = 0; i < 256; i++) {         /* 0xAARRGGBB to R, G, B, A */
        m[off + i * 4 + 0] = (uint8_t)(pal[i] >> 16);
        m[off + i * 4 + 1] = (uint8_t)(pal[i] >> 8);
        m[off + i * 4 + 2] = (uint8_t)pal[i];
        m[off + i * 4 + 3] = 255;
    }
    off += 1024;
    memcpy(m + off, c->v, vbytes);
    off += vbytes;
    for (i = 0; i < c->ntex; i++)       /* the colours and their mipmaps, the rows' fits (below) */
        make_mips(m + off + (size_t)i * MIP_BYTES, c->tex[i], pal);
    off += (Uint32)c->ntex * MIP_BYTES;
    {
        double wt[256];
        size_t j;
        for (i = 0; i < 256; i++)
            wt[i] = 0.25;               /* (every colour a little: a fit for colours no texture has) */
        for (i = 0; i < c->ntex; i++)
            for (j = 0; j < TEX_SIZE; j++)
                wt[c->tex[i][j]] += 1;
        for (i = 0; i < c->nlt; i++)
            make_fit((float *)(m + off + (size_t)i * FIT_BYTES), c->lt[i], pal, wt);
    }
    ooff = off + (Uint32)c->nlt * FIT_BYTES;    /* the objects' texels, then their vertices */
    memcpy(m + ooff, c->opool, c->opool_used);
    memcpy(m + ooff + c->opool_used, c->ov, obytes);
    fx_fill(c, m + ooff + c->opool_used + obytes);
    SDL_UnmapGPUTransferBuffer(dev, tbuf);

    cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (!cmd)
        return NULL;
    cp = SDL_BeginGPUCopyPass(cmd);
    off = 0;
    for (i = 0; i < c->ntex; i++, off += TEX_SIZE) {
        SDL_GPUTextureTransferInfo src = { tbuf, off, 64, 64 };
        SDL_GPUTextureRegion dst;
        SDL_zero(dst);
        dst.texture = texels;
        dst.layer = (Uint32)i;
        dst.w = dst.h = 64;
        dst.d = 1;
        SDL_UploadToGPUTexture(cp, &src, &dst, false);
    }
    for (i = 0; i < c->nlt; i++, off += LT_SIZE) {
        SDL_GPUTextureTransferInfo src = { tbuf, off, 256, 24 };
        SDL_GPUTextureRegion dst;
        SDL_zero(dst);
        dst.texture = ltabs;
        dst.layer = (Uint32)i;
        dst.w = 256;
        dst.h = 24;
        dst.d = 1;
        SDL_UploadToGPUTexture(cp, &src, &dst, false);
    }
    {
        SDL_GPUTextureTransferInfo src = { tbuf, off, 256, 1 };
        SDL_GPUTextureRegion dst;
        SDL_zero(dst);
        dst.texture = paltex;
        dst.w = 256;
        dst.h = 1;
        dst.d = 1;
        SDL_UploadToGPUTexture(cp, &src, &dst, false);
        off += 1024;
    }
    if (vbytes) {
        SDL_GPUTransferBufferLocation src = { tbuf, off };
        SDL_GPUBufferRegion dst = { vbuf, 0, vbytes };
        SDL_UploadToGPUBuffer(cp, &src, &dst, true);
    }
    off += vbytes;
    for (i = 0; i < c->ntex; i++) {
        Uint32 o = off + (Uint32)i * MIP_BYTES;
        int lv, n;
        for (lv = 0, n = 64; lv < MIPS; lv++, n /= 2) {
            SDL_GPUTextureTransferInfo src = { tbuf, o, (Uint32)n, (Uint32)n };
            SDL_GPUTextureRegion dst;
            SDL_zero(dst);
            dst.texture = colours;
            dst.mip_level = (Uint32)lv;
            dst.layer = (Uint32)i;
            dst.w = dst.h = (Uint32)n;
            dst.d = 1;
            SDL_UploadToGPUTexture(cp, &src, &dst, false);
            o += (Uint32)n * n * 4;
        }
    }
    off += (Uint32)c->ntex * MIP_BYTES;
    for (i = 0; i < c->notex; i++) {
        SDL_GPUTextureTransferInfo src = { tbuf, ooff + (Uint32)c->otex_at[i], (Uint32)c->otex_w[i], (Uint32)c->otex_h[i] };
        SDL_GPUTextureRegion dst;
        SDL_zero(dst);
        dst.texture = otex;
        dst.layer = (Uint32)i;
        dst.w = (Uint32)c->otex_w[i];
        dst.h = (Uint32)c->otex_h[i];
        dst.d = 1;
        SDL_UploadToGPUTexture(cp, &src, &dst, false);
    }
    if (obytes) {
        SDL_GPUTransferBufferLocation src = { tbuf, ooff + (Uint32)c->opool_used };
        SDL_GPUBufferRegion dst = { obuf, 0, obytes };
        SDL_UploadToGPUBuffer(cp, &src, &dst, true);
    }
    fx_upload(c, cp, ooff + (Uint32)c->opool_used + obytes);
    for (i = 0; i < c->nlt; i++) {
        SDL_GPUTextureTransferInfo src = { tbuf, off + (Uint32)i * FIT_BYTES, 2, 24 };
        SDL_GPUTextureRegion dst;
        SDL_zero(dst);
        dst.texture = fits;
        dst.layer = (Uint32)i;
        dst.w = 2;
        dst.h = 24;
        dst.d = 1;
        SDL_UploadToGPUTexture(cp, &src, &dst, false);
    }
    SDL_EndGPUCopyPass(cp);

    SDL_zero(cti);
    cti.texture = color;
    cti.clear_color.r = 0.10f;          /* where no ground is drawn (alpha 0: the picture keeps its own) */
    cti.clear_color.g = 0.12f;
    cti.clear_color.b = 0.16f;
    cti.clear_color.a = 0.0f;
    cti.load_op = SDL_GPU_LOADOP_CLEAR;
    cti.store_op = SDL_GPU_STOREOP_STORE;
    SDL_zero(dti);
    dti.texture = depth;
    dti.clear_depth = 1.0f;
    dti.load_op = SDL_GPU_LOADOP_CLEAR;
    dti.store_op = SDL_GPU_STOREOP_STORE;   /* (the weather's pass tests against it) */
    dti.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    dti.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    {   /* the game's projection, sx = hw * (x / z + 1) on a canvas W wide (sy alike, down), as
         * clip coordinates over w = z */
        float ax = 2 * c->hw / c->W, ay = 2 * c->hh / c->H;
        cam[0] = ax;
        cam[1] = ax - 1;
        cam[2] = -ay;
        cam[3] = 1 - ay;
        cam[4] = FAR_Z / (FAR_Z - NEAR_Z);
        cam[5] = -NEAR_Z * FAR_Z / (FAR_Z - NEAR_Z);
        cam[6] = cam[7] = 0;
    }
    rp = SDL_BeginGPURenderPass(cmd, &cti, 1, &dti);
    if (vbytes) {
        memset(look, 0, sizeof look);
        look[0] = (float)filter;
        if (c->fog) {                   /* the fog: start, end, its colour; the view's inverse */
            const float *a = c->m;
            float det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) +
                        a[2] * (a[3] * a[7] - a[4] * a[6]);
            uint32_t fc = pal[c->fog_colour & 0xFF];
            if (det != 0) {
                float k = 1 / det;
                look[4] = c->fog_start;
                look[5] = c->fog_end;
                look[6] = 1;
                look[8] = ((fc >> 16) & 255) / 255.0f;
                look[9] = ((fc >> 8) & 255) / 255.0f;
                look[10] = (fc & 255) / 255.0f;
                look[12] = (a[4] * a[8] - a[5] * a[7]) * k;     /* rows of the inverse */
                look[13] = (a[2] * a[7] - a[1] * a[8]) * k;
                look[14] = (a[1] * a[5] - a[2] * a[4]) * k;
                look[16] = (a[5] * a[6] - a[3] * a[8]) * k;
                look[17] = (a[0] * a[8] - a[2] * a[6]) * k;
                look[18] = (a[2] * a[3] - a[0] * a[5]) * k;
                look[20] = (a[3] * a[7] - a[4] * a[6]) * k;
                look[21] = (a[1] * a[6] - a[0] * a[7]) * k;
                look[22] = (a[0] * a[4] - a[1] * a[3]) * k;
                look[24] = c->mz[0];
                look[25] = c->mz[1];
                look[26] = c->mz[2];
            }
        }
        SDL_BindGPUGraphicsPipeline(rp, pipe);
        bb.buffer = vbuf;
        bb.offset = 0;
        SDL_BindGPUVertexBuffers(rp, 0, &bb, 1);
        sb[0].texture = texels;
        sb[0].sampler = point;
        sb[1].texture = ltabs;
        sb[1].sampler = point;
        sb[2].texture = paltex;
        sb[2].sampler = point;
        sb[3].texture = colours;
        sb[3].sampler = trilin;
        sb[4].texture = fits;
        sb[4].sampler = point;
        SDL_BindGPUFragmentSamplers(rp, 0, sb, 5);
        SDL_PushGPUVertexUniformData(cmd, 0, cam, sizeof cam);
        SDL_PushGPUFragmentUniformData(cmd, 0, look, sizeof look);
        SDL_DrawGPUPrimitives(rp, (Uint32)c->nv, 1, 0, 0);
    }
    SDL_EndGPURenderPass(rp);
    {   /* the objects (gpuobj shaders): their layer cleared, their polygons in the game's order */
        SDL_GPUColorTargetInfo oti;
        SDL_GPURenderPass *op;
        SDL_zero(oti);
        oti.texture = obj_tex;
        oti.load_op = SDL_GPU_LOADOP_CLEAR;
        oti.store_op = SDL_GPU_STOREOP_STORE;
        op = SDL_BeginGPURenderPass(cmd, &oti, 1, NULL);
        if (obj_pipe && otex && c->onv) {
            SDL_GPUTextureSamplerBinding os[3];
            SDL_GPUBufferBinding ob;
            os[0].texture = otex;
            os[0].sampler = point;
            os[1].texture = ltabs;
            os[1].sampler = point;
            os[2].texture = paltex;
            os[2].sampler = point;
            ob.buffer = obuf;
            ob.offset = 0;
            SDL_BindGPUGraphicsPipeline(op, obj_pipe);
            SDL_BindGPUVertexBuffers(op, 0, &ob, 1);
            SDL_BindGPUFragmentSamplers(op, 0, os, 3);
            SDL_PushGPUVertexUniformData(cmd, 0, cam, 8 * sizeof(float));
            SDL_DrawGPUPrimitives(op, (Uint32)c->onv, 1, 0, 0);
        }
        SDL_EndGPURenderPass(op);
    }
    fx_pass(cmd, c);
    weather_pass(cmd, c, cam, pal);
    if (!SDL_SubmitGPUCommandBuffer(cmd))
        return NULL;
    {
        static int told;                /* (the first picture, then the first with objects, with effects) */
        int now = (c->nv ? 1 : 0) | (c->onv ? 2 : 0) | (c->nfx ? 4 : 0);
        if ((now & ~told) && c->nv) {
            told |= now;
            port_log("GPU rendering: picture %dx%d, ground %lu triangles, %d textures; objects %lu triangles, "
                     "%d textures; effects %d (%d new); %d light tables, %lu dropped; fog %s (%.0f to %.0f squares, "
                     "colour %d = %06X)\n",
                     c->W, c->H, (unsigned long)(c->nv / 3), c->ntex, (unsigned long)(c->onv / 3), c->notex, c->nfx,
                     fx_nnew, c->nlt, c->dropped, c->fog ? "on" : "off", c->fog_start, c->fog_end, c->fog_colour,
                     (unsigned)(pal[c->fog_colour & 0xFF] & 0xFFFFFF));
        }
    }
    return shown;
}

const uint16_t *port_gl_mask(void *capture, int row)
{
    const Capture *c = capture;
    if (!port_gl || broken || !c || !c->mask || !shown || c->serial != drawn_serial || row < 0 || row >= c->H)
        return NULL;
    return c->mask + (size_t)row * c->W;
}

SDL_GPURenderState *port_gl_state(SDL_Renderer *r, float sx, float sy, float oy, float a)
{
    float map[4];
    if (!port_gl || broken || r != g_r || !color || !wx_tex || !obj_tex || !fx_tex)
        return NULL;
    if (mix && mix_for != color) {      /* a new picture size: made again with the new target */
        SDL_DestroyGPURenderState(mix);
        mix = NULL;
    }
    if (!mix) {
        SDL_GPURenderStateCreateInfo ri;
        SDL_GPUTextureSamplerBinding sb[4];
        if (!mix_sh) {
            SDL_GPUShaderCreateInfo si;
            SDL_zero(si);
            si.format = SDL_GPU_SHADERFORMAT_DXIL;
            si.entrypoint = "main";
            si.code = gpuland_mix_dxil;
            si.code_size = gpuland_mix_dxil_len;
            si.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
            si.num_samplers = 5;
            si.num_uniform_buffers = 1;
            mix_sh = SDL_CreateGPUShader(dev, &si);
            if (!mix_sh) {
                port_log("GPU ground: the mixing shader not made (%s), off\n", SDL_GetError());
                gl_off();
                return NULL;
            }
        }
        sb[0].texture = color;
        sb[0].sampler = point;
        sb[1].texture = wx_tex;
        sb[1].sampler = point;
        sb[2].texture = obj_tex;
        sb[2].sampler = point;
        sb[3].texture = fx_tex;
        sb[3].sampler = point;
        SDL_zero(ri);
        ri.fragment_shader = mix_sh;
        ri.num_sampler_bindings = 4;
        ri.sampler_bindings = sb;
        mix = SDL_CreateGPURenderState(r, &ri);
        if (!mix) {
            port_log("GPU ground: the mixing state not made (%s), off\n", SDL_GetError());
            gl_off();
            return NULL;
        }
        mix_for = color;
    }
    map[0] = sx;
    map[1] = sy;
    map[2] = oy;
    map[3] = a;
    SDL_SetGPURenderStateFragmentUniforms(mix, 0, map, sizeof map);
    return mix;
}
