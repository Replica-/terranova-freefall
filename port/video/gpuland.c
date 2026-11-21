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
#include "gpuland.h"
#include "shaders/gpuland_vert.dxil.h"
#include "shaders/gpuland_frag.dxil.h"

void port_log(const char *fmt, ...);

int port_gl;
int port_gl_capturing;

#define TEX_MAX 256                     /* textures in one picture (the planet's 64, blast marks) */
#define LT_MAX 64                       /* light tables in one picture (haze and fog levels) */
#define LT_SIZE 0x1800                  /* 24 rows of 256 */
#define TEX_SIZE 4096                   /* 64 x 64 */
#define NEAR_Z (1.0f / 64)
#define FAR_Z 8192.0f

/* a point as the card gets it: x, y, z, light | u, v, texture layer, light table */
typedef struct {
    float a[4], b[4];
} GpuVert;

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
} Capture;

static Capture *cur;
static unsigned serial;
static int last_tex = -1, last_lt = -1;

/* ------------------------------------------------------------------ the game's thread */
void port_gl_begin(void **slot, int W, int H, long half_w, long half_h)
{
    Capture *c = *slot;
    if (!port_gl)
        return;
    if (!c) {
        c = calloc(1, sizeof *c);
        if (c) {
            c->tex = malloc((size_t)TEX_MAX * TEX_SIZE);
            c->lt = malloc((size_t)LT_MAX * LT_SIZE);
        }
        if (!c || !c->tex || !c->lt) {
            port_log("GPU ground: no memory for the capture, off\n");
            port_gl = 0;
            return;
        }
        *slot = c;
    }
    c->W = W;
    c->H = H;
    c->hw = (float)half_w;
    c->hh = (float)half_h;
    c->nv = 0;
    c->ntex = c->nlt = 0;
    c->dropped = 0;
    last_tex = last_lt = -1;
    cur = c;
    port_gl_capturing = 1;
}

void port_gl_end(void)
{
    if (!cur)
        return;
    cur->serial = ++serial;
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
static SDL_GPUSampler *point;
static SDL_GPUTexture *texels, *ltabs, *paltex, *color, *depth;
static SDL_GPUTextureFormat depth_fmt;
static SDL_GPUBuffer *vbuf;
static Uint32 vbuf_size;
static SDL_GPUTransferBuffer *tbuf;
static Uint32 tbuf_size;
static SDL_Texture *shown;
static int tw, th;
static unsigned drawn_serial;
static int filter = 1;

static void forget(void)
{
    /* the renderer (and with it its device and textures) is gone or new: start again */
    g_r = NULL;
    dev = NULL;
    pipe = NULL;
    point = NULL;
    texels = ltabs = paltex = color = depth = NULL;
    vbuf = NULL;
    tbuf = NULL;
    vbuf_size = tbuf_size = 0;
    shown = NULL;
    tw = th = 0;
    drawn_serial = 0;
}

void port_gl_shutdown(void)
{
    if (dev) {
        if (shown)
            SDL_DestroyTexture(shown);
        if (pipe)
            SDL_ReleaseGPUGraphicsPipeline(dev, pipe);
        if (point)
            SDL_ReleaseGPUSampler(dev, point);
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
                                int w, int h, int layers)
{
    SDL_GPUTextureCreateInfo ti;
    SDL_zero(ti);
    ti.type = type;
    ti.format = fmt;
    ti.usage = usage;
    ti.width = (Uint32)w;
    ti.height = (Uint32)h;
    ti.layer_count_or_depth = (Uint32)layers;
    ti.num_levels = 1;
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
    si.code = gpuland_frag_dxil;
    si.code_size = gpuland_frag_dxil_len;
    si.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    si.num_samplers = 3;
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
    if (!pipe) {
        port_log("GPU ground: pipeline not made (%s), off\n", SDL_GetError());
        return 0;
    }

    SDL_zero(sci);
    sci.min_filter = sci.mag_filter = SDL_GPU_FILTER_NEAREST;
    sci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sci.address_mode_u = sci.address_mode_v = sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    point = SDL_CreateGPUSampler(dev, &sci);
    texels = make_tex(SDL_GPU_TEXTURETYPE_2D_ARRAY, SDL_GPU_TEXTUREFORMAT_R8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER, 64, 64, TEX_MAX);
    ltabs = make_tex(SDL_GPU_TEXTURETYPE_2D_ARRAY, SDL_GPU_TEXTUREFORMAT_R8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER, 256, 24, LT_MAX);
    paltex = make_tex(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER, 256, 1, 1);
    if (!point || !texels || !ltabs || !paltex) {
        port_log("GPU ground: textures not made (%s), off\n", SDL_GetError());
        return 0;
    }
    port_log("GPU ground: on (%s, depth %s, texture filter %s)\n", SDL_GetGPUDeviceDriver(dev),
             depth_fmt == SDL_GPU_TEXTUREFORMAT_D32_FLOAT ? "32-bit float" : "24-bit", filter ? "on" : "off");
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
    shown = NULL;
    color = make_tex(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                     SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, W, H, 1);
    depth = make_tex(SDL_GPU_TEXTURETYPE_2D, depth_fmt, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, W, H, 1);
    if (!color || !depth)
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

SDL_Texture *port_gl_render(SDL_Renderer *r, void *capture, const uint32_t *pal)
{
    Capture *c = capture;
    Uint32 vbytes, tbytes, off;
    uint8_t *m;
    SDL_GPUCommandBuffer *cmd;
    SDL_GPUCopyPass *cp;
    SDL_GPURenderPass *rp;
    SDL_GPUColorTargetInfo cti;
    SDL_GPUDepthStencilTargetInfo dti;
    SDL_GPUTextureSamplerBinding sb[3];
    SDL_GPUBufferBinding bb;
    float cam[8], look[4];
    int i;

    if (!port_gl || broken || !c || c->W <= 0 || c->H <= 0)
        return NULL;
    if (r != g_r) {                     /* a new renderer: its device's things are gone with the old */
        forget();
        if (!setup(r)) {
            broken = 1;
            return NULL;
        }
    }
    if (!targets(c->W, c->H))
        return NULL;
    if (c->serial == drawn_serial)
        return shown;
    drawn_serial = c->serial;

    vbytes = (Uint32)(c->nv * sizeof(GpuVert));
    tbytes = (Uint32)c->ntex * TEX_SIZE + (Uint32)c->nlt * LT_SIZE + 1024 + vbytes;
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
    SDL_EndGPUCopyPass(cp);

    SDL_zero(cti);
    cti.texture = color;
    cti.clear_color.r = 0.10f;          /* where no ground is drawn (the sky, for now) */
    cti.clear_color.g = 0.12f;
    cti.clear_color.b = 0.16f;
    cti.clear_color.a = 1.0f;
    cti.load_op = SDL_GPU_LOADOP_CLEAR;
    cti.store_op = SDL_GPU_STOREOP_STORE;
    SDL_zero(dti);
    dti.texture = depth;
    dti.clear_depth = 1.0f;
    dti.load_op = SDL_GPU_LOADOP_CLEAR;
    dti.store_op = SDL_GPU_STOREOP_DONT_CARE;
    dti.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    dti.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    rp = SDL_BeginGPURenderPass(cmd, &cti, 1, &dti);
    if (vbytes) {
        /* the game's projection, sx = hw * (x / z + 1) on a canvas W wide (sy alike, down), as
         * clip coordinates over w = z */
        float ax = 2 * c->hw / c->W, ay = 2 * c->hh / c->H;
        cam[0] = ax;
        cam[1] = ax - 1;
        cam[2] = -ay;
        cam[3] = 1 - ay;
        cam[4] = FAR_Z / (FAR_Z - NEAR_Z);
        cam[5] = -NEAR_Z * FAR_Z / (FAR_Z - NEAR_Z);
        cam[6] = cam[7] = 0;
        look[0] = (float)filter;
        look[1] = look[2] = look[3] = 0;
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
        SDL_BindGPUFragmentSamplers(rp, 0, sb, 3);
        SDL_PushGPUVertexUniformData(cmd, 0, cam, sizeof cam);
        SDL_PushGPUFragmentUniformData(cmd, 0, look, sizeof look);
        SDL_DrawGPUPrimitives(rp, (Uint32)c->nv, 1, 0, 0);
    }
    SDL_EndGPURenderPass(rp);
    if (!SDL_SubmitGPUCommandBuffer(cmd))
        return NULL;
    {
        static int told;
        if (!told && c->nv && ++told)
            port_log("GPU ground: first picture %dx%d, %lu triangles, %d textures, %d light tables, %lu dropped\n",
                     c->W, c->H, (unsigned long)(c->nv / 3), c->ntex, c->nlt, c->dropped);
    }
    return shown;
}
