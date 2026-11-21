/* screen.c -- the options screen's picture settings (the Display, Distance, Detail, Cockpit & maps,
 * Sound & movies, Mods and Filters pages' S_ lines, port/ui/options.c; Freefall; all off by default,
 * port_screen_preset sets them for a preset).
 * They change only how the finished game picture is shown in the window, never the game:
 *   - scale: Fit (the default and the original: the biggest 4:3 picture, every pixel scaled
 *     alike) or Sharp (the same size, with soft edges only where a game pixel falls between two
 *     screen pixels); Pixel perfect (whole screen pixels per game line) was taken out (owner),
 *     its screen_scale=integer reads as Sharp;
 *   - brightness, gamma, contrast and colour, worked into the palette the window shows (the
 *     game's own palette and its screenshots stay as they are);
 *   - scanlines: dark lines between the game's lines, drawn over the picture;
 *   - effects that need shaders: Smooth edges (xBR) and CRT (crt-lottes), on SDL's "gpu"
 *     renderer (Direct3D 12 / Vulkan).  The renderer changes only while one of them is on;
 *     where it can't start, the effect shows "not available".
 *   - 3D resolution: the main 3D view drawn again at 2x, 4x or 6x the pixels (sharp3d.c);
 *   - widescreen: the 3D view drawn as wide as the window (sharp3d.c): the mission's whole
 *     screen stretched evenly, the 3D behind its window at the right proportions; the main
 *     menu at 16:9, with the owner's sides beside the game's picture (menuwide.c);
 *   - terrain detail: the smooth (polygon) ground farther out and the far columns denser, in
 *     the picture drawn for the display (sharp3d.c);
 *   - terrain smoothing: the far land's colours blended smoothly (haze.c, sharp view);
 *   - object distance: units, vehicles and buildings drawn twice (Far) or 4 times (Farthest)
 *     as far as the game draws them, trees out to 48 or 96 squares (viewdist.c, haze.c, sharp
 *     view); the enemy's shooters only by "enemy distance": twice as far (Far), its gun turrets
 *     always at the game's distance;
 *   - fog (on / off): the far land fades into the sky and is not drawn past the fog's end,
 *     nearer in rain and snow (fog.c, sharp view);
 *   - true colour: the sky, the clouds and the fog worked out in full colour after the main
 *     view is drawn, soft and without steps or dots; On + sun glow, a glow around the sun and
 *     with the fog the fog lighter toward it (truecol.c, sharp view);
 *   - water reflection: Medium, the sky in all the water at any angle; High, that and land,
 *     buildings and units mirrored where they stand (rndsky.c, sharp view);
 *   - water ripples: moving waves and rings around soldiers in the Medium or High reflection
 *     (rndsky.c);
 *   - water effects: splashes where shots and bodies hit the water, the wakes of moving
 *     vehicles, lighter blue shallows near the shore (rndsky.c);
 *   - effects pack: the effect animations drawn with a pack's pictures (a mod's Effects folder)
 *     (effects.c, sharp view);
 *   - objects pack: the trees, bushes and rocks drawn with a pack's pictures (a mod's Objects)
 *     (effects.c, sharp view);
 *   - cockpit pack: with Widescreen, the cockpit's and the goggles' art from a pack's pictures (a
 *     mod's Cockpits), painted for 16:9, in place of the evenly stretched screen (cockpitpack.c,
 *     platform.c);
 *   - infrared: Sci-fi, the game's infrared view as a thermal camera's: its colours, hot
 *     outlines and a sensor look (infrared.c);
 *   - anti-aliasing: FXAA on the sharp 3D views' pixels only (port_screen_fxaa, on the "gpu"
 *     renderer like the effects; platform.c marks which pixels are the 3D's).
 * With every filter off the window is drawn exactly as before, on the system's default renderer. */
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>
#include "port.h"
#include "screen.h"
#include "platform.h"
#include "sharp3d.h"
#include "cockpitpack.h"
#include "arms.h"
#include "helmetfx.h"
#include "gpuland.h"
#include "videosmooth.h"

#include "shaders/crt.spv.h"
#include "shaders/crt.dxil.h"
#include "shaders/xbr.spv.h"
#include "shaders/xbr.dxil.h"
#include "shaders/fxaa.spv.h"
#include "shaders/fxaa.dxil.h"

enum { SC_FIT, SC_SHARP };
enum { FX_OFF, FX_SMOOTH, FX_CRT, FX_CRT_CURVED, FX_COUNT };

static int scale_mode;                  /* SC_ */
static int brightness;                  /* -50..50 (%) */
static int gamma_pc = 100;              /* 50..200: 100 = as the game */
static int contrast;                    /* -50..50 (%) */
static int saturation = 100;            /* 0..200 (%) */
static int scanlines;                   /* 0 (off)..100 (%) */
static int effect;                      /* FX_ */
static int fxaa;                        /* "Anti-aliasing": 0 Off, 1 FXAA */
static int movie_fill;                  /* "Movie size": 0 Original, 1 Fill screen */
static int gpu_unavailable;             /* the "gpu" renderer or its shaders could not start */
static unsigned serial = 1;

static SDL_Renderer *st_renderer;       /* the renderer the shaders and the overlay were made on */
static SDL_GPURenderState *states[FX_COUNT];
static SDL_GPUShader *shaders[FX_COUNT];  /* the render states use them; freed after them */
static SDL_GPUDevice *st_device;
static int states_tried;
static SDL_GPURenderState *fxaa_state;
static SDL_GPUShader *fxaa_shader;
static SDL_Texture *fxaa_tex;           /* the pass's picture */
static int fxaa_w, fxaa_h;
static SDL_Texture *overlay;            /* scanlines: 1 x 2 lines per game line */
static int overlay_lines;

static const char *scale_names[2] = { "fit", "sharp" };
static const char *effect_names[FX_COUNT] = { "off", "smooth", "crt", "crt_curved" };
static const char *terrain_names[3] = { "original", "medium", "high" };     /* (and the earlier "low") */
static const char *objects_names[3] = { "original", "far", "farthest" };

/* ------------------------------------------------------------------ settings */
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int name_index(const char *v, const char *const *names, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (!SDL_strcasecmp(v, names[i]))
            return i;
    return 0;
}

void port_screen_load(void (*get)(const char *key, char *out, int size))
{
    char v[32];
#define GETI(key, var, lo, hi) do { v[0] = 0; get(key, v, sizeof v); if (v[0]) var = clampi(atoi(v), lo, hi); } while (0)
    v[0] = 0;
    get("screen_scale", v, sizeof v);
    scale_mode = !SDL_strcasecmp(v, "integer") ? SC_SHARP : v[0] ? name_index(v, scale_names, 2) : SC_FIT;
    GETI("screen_brightness", brightness, -50, 50);
    GETI("screen_gamma", gamma_pc, 50, 200);
    GETI("screen_contrast", contrast, -50, 50);
    GETI("screen_colour", saturation, 0, 200);
    GETI("screen_scanlines", scanlines, 0, 100);
#undef GETI
    v[0] = 0;
    get("screen_effect", v, sizeof v);
    effect = name_index(v, effect_names, FX_COUNT);
    v[0] = 0;
    get("screen_sharp3d", v, sizeof v);
    port_sharp3d = clampi(atoi(v), 0, 6) & ~1;     /* Original, 2x, 4x or 6x */
    v[0] = 0;
    get("screen_wide", v, sizeof v);
    port_widescreen = !SDL_strcasecmp(v, "fit") || !SDL_strcasecmp(v, "on");   /* written "fit" (the value shows On) */
    v[0] = 0;
    get("screen_water", v, sizeof v);
    port_water_full = !SDL_strcasecmp(v, "high") || !SDL_strcasecmp(v, "full") ? 2 :
                      !SDL_strcasecmp(v, "medium") || !SDL_strcasecmp(v, "advanced") ? 1 : 0;
    v[0] = 0;
    get("screen_ripples", v, sizeof v);      /* (an earlier test build's "calm" or "lively": on) */
    port_water_ripples = !SDL_strcasecmp(v, "on") || !SDL_strcasecmp(v, "calm") || !SDL_strcasecmp(v, "lively");
    v[0] = 0;
    get("screen_waterfx", v, sizeof v);
    port_water_fx = !SDL_strcasecmp(v, "on");
    v[0] = 0;
    get("screen_terrain", v, sizeof v);
    port_terrain = v[0] ? name_index(v, terrain_names, 3) : 0;
    v[0] = 0;
    get("screen_smooth", v, sizeof v);
    port_far_smooth = !SDL_strcasecmp(v, "on");
    v[0] = 0;
    get("screen_fog", v, sizeof v);
    port_fog = !SDL_strcasecmp(v, "on") || !SDL_strcasecmp(v, "light") ||  /* earlier test builds' */
               !SDL_strcasecmp(v, "strong");
    v[0] = 0;
    get("screen_truecolour", v, sizeof v);
    port_truecol = !SDL_strcasecmp(v, "glow") ? 2 : !SDL_strcasecmp(v, "on");
    v[0] = 0;
    get("screen_renderer", v, sizeof v);       /* "GPU rendering": gpuland.c (FREEFALL_GPU_LAND overrides it) */
    port_gl = !SDL_strcasecmp(v, "gpu");
    v[0] = 0;
    get("screen_gpu_ground", v, sizeof v);     /* (its first name) */
    if (!SDL_strcasecmp(v, "on"))
        port_gl = 1;
    v[0] = 0;
    get("screen_map", v, sizeof v);             /* "smooth" and "soft": earlier test builds' */
    port_map_smooth = !SDL_strcasecmp(v, "on") || !SDL_strcasecmp(v, "smooth") || !SDL_strcasecmp(v, "soft");
    v[0] = 0;
    get("screen_objects", v, sizeof v);
    port_obj_dist = v[0] ? name_index(v, objects_names, 3) : 0;
    v[0] = 0;
    get("screen_enemies", v, sizeof v);
    port_enemy_dist = !SDL_strcasecmp(v, "far");
    port_fx_pack[0] = 0;
    get("screen_effects", port_fx_pack, sizeof port_fx_pack);
    port_obj_pack[0] = 0;
    get("screen_objpack", port_obj_pack, sizeof port_obj_pack);
    port_ck_pack[0] = 0;
    get("screen_cockpit", port_ck_pack, sizeof port_ck_pack);
    port_arms_pack[0] = 0;
    get("screen_arms", port_arms_pack, sizeof port_arms_pack);
    v[0] = 0;
    get("screen_helmet", v, sizeof v);
    port_helmet_motion = !SDL_strcasecmp(v, "on") || !SDL_strcasecmp(v, "light") || !SDL_strcasecmp(v, "full");
    v[0] = 0;
    get("screen_trees", v, sizeof v);
    port_tree_size = !SDL_strcasecmp(v, "realistic");
    v[0] = 0;
    get("screen_burn", v, sizeof v);
    port_burn = !SDL_strcasecmp(v, "realistic") || !SDL_strcasecmp(v, "explosive") ||  /* earlier test builds' */
                !SDL_strcasecmp(v, "flash");
    v[0] = 0;
    get("screen_infrared", v, sizeof v);
    port_ir_style = !SDL_strcasecmp(v, "scifi");
    v[0] = 0;
    get("screen_aa", v, sizeof v);
    fxaa = !SDL_strcasecmp(v, "fxaa");
    v[0] = 0;
    get("screen_movies", v, sizeof v);
    movie_fill = !SDL_strcasecmp(v, "fill");
    serial++;
}

void port_screen_save(FILE *f)
{
    if (scale_mode != SC_FIT)
        fprintf(f, "screen_scale=%s\n", scale_names[scale_mode]);
    if (brightness)
        fprintf(f, "screen_brightness=%d\n", brightness);
    if (gamma_pc != 100)
        fprintf(f, "screen_gamma=%d\n", gamma_pc);
    if (contrast)
        fprintf(f, "screen_contrast=%d\n", contrast);
    if (saturation != 100)
        fprintf(f, "screen_colour=%d\n", saturation);
    if (scanlines)
        fprintf(f, "screen_scanlines=%d\n", scanlines);
    if (effect)
        fprintf(f, "screen_effect=%s\n", effect_names[effect]);
    if (port_sharp3d)
        fprintf(f, "screen_sharp3d=%d\n", port_sharp3d);
    if (port_widescreen)
        fprintf(f, "screen_wide=fit\n");
    if (port_terrain)
        fprintf(f, "screen_terrain=%s\n", terrain_names[port_terrain]);
    if (port_far_smooth)
        fprintf(f, "screen_smooth=on\n");
    if (port_obj_dist)
        fprintf(f, "screen_objects=%s\n", objects_names[port_obj_dist]);
    if (port_enemy_dist)
        fprintf(f, "screen_enemies=far\n");
    if (port_fog)
        fprintf(f, "screen_fog=on\n");
    if (port_truecol)
        fprintf(f, "screen_truecolour=%s\n", port_truecol == 2 ? "glow" : "on");
    if (port_gl)
        fprintf(f, "screen_renderer=gpu\n");
    if (port_water_full)
        fprintf(f, "screen_water=%s\n", port_water_full == 2 ? "high" : "medium");
    if (port_water_ripples)
        fprintf(f, "screen_ripples=on\n");
    if (port_water_fx)
        fprintf(f, "screen_waterfx=on\n");
    if (port_map_smooth)
        fprintf(f, "screen_map=on\n");
    if (port_fx_pack[0])
        fprintf(f, "screen_effects=%s\n", port_fx_pack);
    if (port_obj_pack[0])
        fprintf(f, "screen_objpack=%s\n", port_obj_pack);
    if (port_ck_pack[0])
        fprintf(f, "screen_cockpit=%s\n", port_ck_pack);
    if (port_arms_pack[0])
        fprintf(f, "screen_arms=%s\n", port_arms_pack);
    if (port_helmet_motion)
        fprintf(f, "screen_helmet=on\n");
    if (port_tree_size)
        fprintf(f, "screen_trees=realistic\n");
    if (port_burn)
        fprintf(f, "screen_burn=realistic\n");
    if (port_ir_style)
        fprintf(f, "screen_infrared=scifi\n");
    if (fxaa)
        fprintf(f, "screen_aa=fxaa\n");
    if (movie_fill)
        fprintf(f, "screen_movies=fill\n");
}

void port_screen_filters_off(void)
{
    brightness = contrast = scanlines = 0;
    gamma_pc = saturation = 100;
    effect = FX_OFF;
    serial++;
}

/* ------------------------------------------------------------------ the display */
/* the "gpu" renderer: for an effect (or FREEFALL_RENDERER=gpu, for tests) */
int port_screen_gpu_wanted(void)
{
    const char *s = SDL_getenv("FREEFALL_RENDERER");
    if (s && !SDL_strcmp(s, "gpu"))
        return 1;
    return (effect != FX_OFF || fxaa || port_gl) && !gpu_unavailable;     /* (the GPU ground draws on it) */
}

/* the FXAA pass will run: the display marks the 3D's pixels for it (platform.c) */
int port_screen_fxaa_wanted(void)
{
    return fxaa && !gpu_unavailable;
}

void port_screen_gpu_unavailable(void)
{
    gpu_unavailable = 1;
}

void port_screen_renderer_gone(void)
{
    int i;
    for (i = 0; i < FX_COUNT; i++) {
        if (states[i])
            SDL_DestroyGPURenderState(states[i]);
        if (shaders[i])
            SDL_ReleaseGPUShader(st_device, shaders[i]);
        states[i] = NULL;
        shaders[i] = NULL;
    }
    if (fxaa_state)
        SDL_DestroyGPURenderState(fxaa_state);
    if (fxaa_shader)
        SDL_ReleaseGPUShader(st_device, fxaa_shader);
    fxaa_state = NULL;
    fxaa_shader = NULL;
    if (fxaa_tex)
        SDL_DestroyTexture(fxaa_tex);
    fxaa_tex = NULL;
    st_device = NULL;
    if (overlay)
        SDL_DestroyTexture(overlay);
    overlay = NULL;
    overlay_lines = 0;
    states_tried = 0;
    st_renderer = NULL;
}

unsigned port_screen_serial(void)
{
    return serial;
}

/* the colour settings worked into the palette the window shows */
void port_screen_grade(const uint32_t *pal, uint32_t *out)
{
    float b = brightness / 100.0f, c = 1 + contrast / 100.0f, g = 100.0f / gamma_pc, s = saturation / 100.0f;
    int i, k;

    if (!brightness && !contrast && gamma_pc == 100 && saturation == 100) {
        memcpy(out, pal, 256 * sizeof *out);
        return;
    }
    for (i = 0; i < 256; i++) {
        float v[3], l;
        uint32_t o = 0xFF000000u;
        for (k = 0; k < 3; k++) {
            v[k] = (float)(pal[i] >> (16 - 8 * k) & 0xFF) / 255;
            if (g != 1)
                v[k] = SDL_powf(v[k], g);
            v[k] = (v[k] - 0.5f) * c + 0.5f + b;
        }
        l = 0.299f * v[0] + 0.587f * v[1] + 0.114f * v[2];
        for (k = 0; k < 3; k++) {
            float x = l + (v[k] - l) * s;
            x = x < 0 ? 0 : x > 1 ? 1 : x;
            o |= (uint32_t)(x * 255 + 0.5f) << (16 - 8 * k);
        }
        out[i] = o;
    }
}

/* the same colour settings for RGB pixels (Smooth videos' in-between pictures) */
void port_screen_grade_rgb(const uint8_t *rgb, uint32_t *out, int n)
{
    static float lut[256];
    static uint8_t lut8[256];
    static unsigned made = ~0u;
    float c = 1 + contrast / 100.0f, sat = saturation / 100.0f;
    int i;

    if (made != serial) {
        float b = brightness / 100.0f, g = 100.0f / gamma_pc;
        for (i = 0; i < 256; i++) {
            float v = (float)i / 255;
            if (g != 1)
                v = SDL_powf(v, g);
            lut[i] = (v - 0.5f) * c + 0.5f + b;
            lut8[i] = (uint8_t)((lut[i] < 0 ? 0 : lut[i] > 1 ? 1 : lut[i]) * 255 + 0.5f);
        }
        made = serial;
    }
    if (!brightness && !contrast && gamma_pc == 100 && saturation == 100) {
        for (i = 0; i < n; i++, rgb += 3)
            out[i] = 0xFF000000u | (uint32_t)rgb[0] << 16 | (uint32_t)rgb[1] << 8 | rgb[2];
        return;
    }
    for (i = 0; i < n; i++, rgb += 3) {
        uint32_t o = 0xFF000000u;
        int k;
        if (saturation == 100)
            o |= (uint32_t)lut8[rgb[0]] << 16 | (uint32_t)lut8[rgb[1]] << 8 | lut8[rgb[2]];
        else {
            float v[3] = { lut[rgb[0]], lut[rgb[1]], lut[rgb[2]] };
            float l = 0.299f * v[0] + 0.587f * v[1] + 0.114f * v[2];
            for (k = 0; k < 3; k++) {
                float x = l + (v[k] - l) * sat;
                x = x < 0 ? 0 : x > 1 ? 1 : x;
                o |= (uint32_t)(x * 255 + 0.5f) << (16 - 8 * k);
            }
        }
        out[i] = o;
    }
}

/* the same colour settings for the 3D's full colours (True colour, platform.c): each channel
 * through lut, then the colour (saturation, %) *sat */
void port_screen_grade_lut(uint8_t lut[256], int *sat)
{
    float b = brightness / 100.0f, g = 100.0f / gamma_pc, c = 1 + contrast / 100.0f;
    int i;
    for (i = 0; i < 256; i++) {
        float v = (float)i / 255;
        if (g != 1)
            v = SDL_powf(v, g);
        v = (v - 0.5f) * c + 0.5f + b;
        lut[i] = (uint8_t)((v < 0 ? 0 : v > 1 ? 1 : v) * 255 + 0.5f);
    }
    *sat = saturation;
}

/* where the game's w x h picture goes in a ww x wh window: always 4:3, like on a CRT */

int port_screen_movie_rect(int w, int h, int ww, int wh, int mx, int my, int mw, int mh, SDL_FRect *dst)
{
    SDL_FRect r;
    float kx, ky, s;

    if (!movie_fill || w <= 0 || h <= 0 || mw <= 0 || mh <= 0 || mx < 0 || my < 0 || mx + mw > w || my + mh > h)
        return 0;
    port_screen_rect(w, h, ww, wh, &r);
    kx = r.w / w;                       /* window pixels a game pixel, the screen's shape kept */
    ky = r.h / h;
    s = (float)ww / (mw * kx) < (float)wh / (mh * ky) ? (float)ww / (mw * kx) : (float)wh / (mh * ky);
    if (s < 1.01f)
        return 0;                       /* the movie is as big as it gets already */
    kx *= s;
    ky *= s;
    dst->w = w * kx;
    dst->h = h * ky;
    dst->x = SDL_floorf((ww - mw * kx) / 2 - mx * kx);
    dst->y = SDL_floorf((wh - mh * ky) / 2 - my * ky);
    return 1;
}

void port_screen_rect(int w, int h, int ww, int wh, SDL_FRect *dst)
{
    float sw = (float)ww, sh = (float)wh, scale = sw / 4 < sh / 3 ? sw / 4 : sh / 3;

    (void)w;
    (void)h;
    dst->w = 4 * scale;
    dst->h = 3 * scale;
    dst->x = (sw - dst->w) / 2;
    dst->y = (sh - dst->h) / 2;
}

static SDL_GPURenderState *make_state(SDL_Renderer *r, SDL_GPUDevice *dev, int fx, SDL_GPUShader **shp)
{
    SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(dev);
    SDL_GPUShaderCreateInfo si;
    SDL_GPURenderStateCreateInfo ri;
    SDL_GPUShader *sh;
    SDL_GPURenderState *st;

    SDL_zero(si);
    if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
        si.format = SDL_GPU_SHADERFORMAT_SPIRV;
        si.code = fx == FX_SMOOTH ? xbr_spv : fx == FX_COUNT ? fxaa_spv : crt_spv;
        si.code_size = fx == FX_SMOOTH ? xbr_spv_len : fx == FX_COUNT ? fxaa_spv_len : crt_spv_len;
    } else if (formats & SDL_GPU_SHADERFORMAT_DXIL) {
        si.format = SDL_GPU_SHADERFORMAT_DXIL;
        si.code = fx == FX_SMOOTH ? xbr_dxil : fx == FX_COUNT ? fxaa_dxil : crt_dxil;
        si.code_size = fx == FX_SMOOTH ? xbr_dxil_len : fx == FX_COUNT ? fxaa_dxil_len : crt_dxil_len;
    } else
        return NULL;
    si.entrypoint = "main";
    si.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    si.num_samplers = 1;
    si.num_uniform_buffers = 1;
    sh = SDL_CreateGPUShader(dev, &si);
    if (!sh)
        return NULL;
    SDL_zero(ri);
    ri.fragment_shader = sh;
    st = SDL_CreateGPURenderState(r, &ri);
    if (st)
        *shp = sh;                      /* the render state only points to it */
    else
        SDL_ReleaseGPUShader(dev, sh);
    return st;
}

/* the effects' shaders, made once on each new renderer */
static void make_states(SDL_Renderer *r)
{
    SDL_GPUDevice *dev = SDL_GetGPURendererDevice(r);
    int fx;

    states_tried = 1;
    st_device = dev;
    if (!dev) {
        gpu_unavailable = 1;
        return;
    }
    port_log("Screen filters: graphics card API %s\n", SDL_GetGPUDeviceDriver(dev));
    for (fx = FX_SMOOTH; fx < FX_COUNT; fx++) {
        if (fx == FX_CRT_CURVED) {      /* the same shader, its own uniforms */
            states[fx] = states[FX_CRT] ? make_state(r, dev, FX_CRT, &shaders[fx]) : NULL;
            continue;
        }
        states[fx] = make_state(r, dev, fx, &shaders[fx]);
        if (!states[fx])
            port_log("Screen filter shader %s could not be made: %s\n", effect_names[fx], SDL_GetError());
    }
    fxaa_state = make_state(r, dev, FX_COUNT, &fxaa_shader);    /* (FX_COUNT: FXAA) */
    if (!fxaa_state)
        port_log("Screen filter shader fxaa could not be made: %s\n", SDL_GetError());
    if (!states[FX_SMOOTH] && !states[FX_CRT] && !fxaa_state)
        gpu_unavailable = 1;            /* back to the default renderer (platform.c) */
}

static void draw_scanlines(SDL_Renderer *r, int h, const SDL_FRect *dst)
{
    if (!overlay || overlay_lines != h) {
        uint32_t *px = (uint32_t *)SDL_malloc(2 * h * sizeof *px);
        int y;
        if (overlay)
            SDL_DestroyTexture(overlay);
        overlay = NULL;
        if (!px)
            return;
        for (y = 0; y < 2 * h; y++)
            px[y] = y & 1 ? 0xFF000000u : 0;   /* the lower half of every game line dark */
        overlay = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, 1, 2 * h);
        if (overlay) {
            SDL_UpdateTexture(overlay, NULL, px, sizeof *px);
            SDL_SetTextureBlendMode(overlay, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(overlay, SDL_SCALEMODE_LINEAR);
            overlay_lines = h;
        }
        SDL_free(px);
        if (!overlay)
            return;
    }
    SDL_SetTextureAlphaMod(overlay, (Uint8)(scanlines * 255 / 100));
    SDL_RenderTexture(r, overlay, NULL, dst);
}

/* "Anti-aliasing: FXAA": the w x h picture src (its 3D's pixels with alpha 1, platform.c) through
 * the FXAA shader into a picture of the same size; src itself when the pass can't run */
SDL_Texture *port_screen_fxaa(SDL_Renderer *r, SDL_Texture *src, int w, int h)
{
    struct { float source[4], output[4], params[4]; } u = {
        { (float)w, (float)h, 1.0f / w, 1.0f / h },
        { (float)w, (float)h, 1.0f / w, 1.0f / h },
        { 0.125f, 0.0312f, 0.75f, 0 },  /* edge threshold, its minimum, subpixel amount */
    };
    if (r != st_renderer) {
        port_screen_renderer_gone();
        st_renderer = r;
    }
    if (!states_tried)
        make_states(r);
    if (!fxaa_state || w <= 0 || h <= 0)
        return src;
    if (!fxaa_tex || fxaa_w != w || fxaa_h != h) {
        if (fxaa_tex)
            SDL_DestroyTexture(fxaa_tex);
        fxaa_tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
        fxaa_w = w;
        fxaa_h = h;
        if (!fxaa_tex)
            return src;
    }
    SDL_SetRenderTarget(r, fxaa_tex);
    SDL_SetTextureScaleMode(src, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(src, SDL_BLENDMODE_NONE);
    SDL_SetGPURenderStateFragmentUniforms(fxaa_state, 0, &u, sizeof u);
    SDL_SetGPURenderState(r, fxaa_state);
    SDL_RenderTexture(r, src, NULL, NULL);
    SDL_SetGPURenderState(r, NULL);
    SDL_SetRenderTarget(r, NULL);
    return fxaa_tex;
}

/* the game's picture (texture t, w x h) into dst */
void port_screen_draw(SDL_Renderer *r, SDL_Texture *t, int w, int h, const SDL_FRect *dst)
{
    SDL_GPURenderState *st = NULL;
    int fx = gpu_unavailable ? FX_OFF : effect;

    if (r != st_renderer) {
        port_screen_renderer_gone();
        st_renderer = r;
    }
    if (fx != FX_OFF) {
        if (!states_tried)
            make_states(r);
        st = states[fx];
    }
    SDL_SetTextureScaleMode(t, st || scale_mode == SC_FIT ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_PIXELART);
    if (st) {
        struct { float source[4], output[4], params[4]; } u = {
            { (float)w, (float)h, 1.0f / w, 1.0f / h },
            { dst->w, dst->h, 1.0f / dst->w, 1.0f / dst->h },
            { fx == FX_CRT_CURVED ? 1.0f : 0.0f, 1.0f, 0, 0 },
        };
        SDL_SetGPURenderStateFragmentUniforms(st, 0, &u, sizeof u);
        SDL_SetGPURenderState(r, st);
    }
    SDL_RenderTexture(r, t, NULL, dst);
    if (st)
        SDL_SetGPURenderState(r, NULL);
    if (scanlines && !(st && fx >= FX_CRT))  /* the CRT effect draws its own */
        draw_scanlines(r, h, dst);
}

/* ------------------------------------------------------------------ the settings on the options screen's pages (options.c) */
static const char *labels[S_COUNT] = {
    "3D resolution", "Widescreen", "Anti-aliasing", "Fog", "True colour", "Map smoothing", "Effects mod", "Objects mod",
    "Cockpit mod", "Dynamic helmet", "Arms mod", "Infrared", "Scaling", "Smooth videos", "Movie size",
    "Terrain distance", "Terrain smoothing", "Water reflection", "Water ripples", "Water effects",
    "Object distance", "Enemy distance", "Tree size", "Tree explosion", "GPU rendering",
    "Brightness", "Gamma", "Contrast", "Colour", "Scanlines", "Effect",
    "Switch all filters off",
};

/* 0: the line needs another setting first (the options screen draws it grey) */
int port_screen_usable(int i)
{
    switch (i) {
    case S_AA:
    case S_MAP:
        return port_sharp3d != 0;
    case S_RIPPLES:
    case S_WATERFX:
        return port_water_full != 0;
    case S_CKPACK:
        return port_widescreen != 0;
    case S_HMOTION:
        return port_widescreen != 0 && port_ck_pack[0] != 0;
    }
    return 1;
}

/* the presets' values of these pages (options.c): 0 Classic, 1 Vanilla+, 2 Freefall.  The filters
 * stay as they are; the packs are the options screen's (its Mod set) */
void port_screen_preset(int p)
{
    int v = p == 1, f = p == 2;
    port_sharp3d = f ? 4 : v ? 2 : 0;   /* Vanilla+: 2x, 4:3 (owner) */
    port_widescreen = f;
    scale_mode = f ? SC_SHARP : SC_FIT;
    fxaa = f;
    port_terrain = p > 0 ? 2 : 0;
    port_far_smooth = f;
    port_obj_dist = f ? 2 : v;
    port_enemy_dist = f;
    port_fog = p > 0;
    port_truecol = f;                  /* On; the sun glow stays experimental */
    port_water_full = p > 0;           /* Medium; High stays experimental */
    port_water_ripples = p > 0;
    port_water_fx = p > 0;
    port_map_smooth = 0;                /* Off in every preset (owner) */
    port_tree_size = f;
    port_burn = f;
    port_helmet_motion = f;             /* On (owner) */
    port_ir_style = p > 0;
    if (p > 0 && !port_smooth_videos)
        vsmooth_start(0, -1);           /* loads in the background; the next movie is smoothed */
    port_smooth_videos = p > 0;
    movie_fill = 0;                     /* Classic in every preset (owner; Vanilla+ had
                                           Fill screen before) */
    serial++;
}

/* the settings are preset p's */
int port_screen_is_preset(int p)
{
    int v = p == 1, f = p == 2, on = p > 0;
    return port_sharp3d == (f ? 4 : v ? 2 : 0) && !port_widescreen == !f && scale_mode == (f ? SC_SHARP : SC_FIT) &&
           !fxaa == !f && port_terrain == (on ? 2 : 0) && !port_far_smooth == !f && port_obj_dist == (f ? 2 : v) &&
           !port_enemy_dist == !f && !port_fog == !on && port_truecol == f && port_water_full == on &&
           !port_water_ripples == !on && !port_water_fx == !on && !port_map_smooth && !port_tree_size == !f &&
           !port_burn == !f && port_helmet_motion == f && !port_ir_style == !on && !port_smooth_videos == !on && !movie_fill;
}

int port_screen_is_value(int i)
{
    return i >= 0 && i < S_RESET;
}

const char *port_screen_label(int i)
{
    return i >= 0 && i < S_COUNT ? labels[i] : "";
}

/* help for setting i's value v (port_screen_get's numbering; -1: the value it has), so the help
 * line can say what a button does while the mouse is over it */
const char *port_screen_help_v(int i, int v)
{
    if (v < 0)
        v = port_screen_get(i);
    switch (i) {
    case S_SHARP3D:
        return v ? "The 3D view drawn at a higher resolution" : "Classic: the game's own pixels";
    case S_WIDE:
        return v ? "3D view at 16:9 widescreen" : "Off: 4:3 as the original";   /* (owner) */
    case S_AA:
        if (v && gpu_unavailable)
            return "Needs Direct3D 12 or Vulkan";
        if (!port_sharp3d)
            return "Needs 3D resolution 2x, 4x or 6x";
        return v ? "Smooth edges in the 3D views (uses the GPU)" : "Off: the 3D's edges as drawn";
    case S_TERRAIN:
        return v == 2 ? "Highest drawing distance" :
               v == 1 ? "Improved drawing distance" : "Game's default detail distance";
    case S_SMOOTH:
        return v ? "The far land textured like the near ground" : "Off: the game's far land";
    case S_OBJECTS:
        return v == 2 ? "Your units, buildings, trees 4 times as far" :
               v == 1 ? "Your units, buildings, trees twice as far" : "Game's default drawing distance";
    case S_ENEMIES:
        return v ? "Enemy units and shots twice as far" : "Enemies at the game's distance";
    case S_FOG:
        return v ? "The far land fades into the sky's colour" : "Off: the game's own haze";
    case S_TRUECOL:
        return v == 2 ? "Full colour, sun's glow on sky and fog" :
               v ? "Sky, clouds and fog in full colour, no dots" : "Off: the game's 256 colours";
    case S_WATER:
        return v == 2 ? "Sky, land and units fully reflected" :
               v ? "The sky in the water at any angle" : "Game's default";
    case S_RIPPLES:
        if (!port_water_full)
            return "Needs Water reflection: Medium or High";
        return v ? "Moving waves on the water" : "Off: the water is a flat mirror";
    case S_WATERFX:
        if (!port_water_full)
            return "Needs Water reflection: Medium or High";
        return v ? "Splashes, wakes and shallows" : "Off: no splashes, wakes, shallows";
    case S_MAP:
        if (!port_sharp3d)
            return "Needs 3D resolution 2x, 4x or 6x";
        return v ? "The maps' land with smooth outlines" : "Off: the game's map, a square a colour";
    case S_EFFECTS: {
        char packs[1][64];
        if (port_fx_pack[0])
            return "Explosions and effects from the Mods folder";
        return port_fx_packs(packs, 1) ? "Off: the game's own effects" : "No effects mods in the Mods folder";
    }
    case S_OBJPACK: {
        char packs[1][64];
        if (port_obj_pack[0])
            return "Trees, bushes and rocks from the Mods folder";
        return port_obj_packs(packs, 1) ? "Off: the game's own trees and rocks" : "No objects mods in the Mods folder";
    }
    case S_CKPACK: {
        char packs[1][64];
        if (!port_ck_packs(packs, 1))
            return "No cockpit mods in the Mods folder";
        if (!port_widescreen)
            return "Needs Widescreen";
        return port_ck_pack[0] ? "Cockpit and goggles art made for 16:9" : "Off: the cockpit stretched to 16:9";
    }
    case S_HMOTION:
        if (!port_widescreen || !port_ck_pack[0])
            return "Needs a cockpit mod";
        return v ? "The helmet sways with the motion, best at game's 320x400 resolution" : "Off: the helmet is still";
    case S_ARMS: {
        char packs[1][64];
        if (port_arms_pack[0])
            return "The arm with the weapon, in the goggles";
        return port_arms_packs(packs, 1) ? "Off: no arms in the goggles" : "No arms mods in the Mods folder";
    }
    case S_TREESIZE:
        return v ? "Sizes 0.6x to 2x, the picture only" : "Trees, bushes and rocks the game's size";
    case S_BURN:
        return v ? "Vegetation bursts apart, Freefall Mod is recommended" : "Classic: the game's fireball";
    case S_GPULAND:
        if (v && gpu_unavailable)
            return "Needs Direct3D 12";
        return v ? "The graphics card draws the world, full colour (more to come)" : "Off: the game's own drawing";
    case S_INFRARED:
        return v ? "Real thermal camera vision" : "Classic: the game's infrared colours";
    case S_SCALE:
        return v == SC_FIT ? "Fit: the biggest 4:3 picture (classic)" :
                                      "Sharp: the same size, even pixels";
    case S_VIDEOS:
        if (v && vsmooth_state() < 0)
            return "Needs Vulkan graphics drivers";
        return v ? "Movies at 30 pictures a second (RIFE)" : "Off: movies at 15 pictures a second";
    case S_MOVIES:
        return v ? "Movies as big as the window allows" : "Classic: movies their own size";
    case S_BRIGHT:
        return "Lighter or darker picture";
    case S_GAMMA:
        return "Above 1.00: dark parts lighter";
    case S_CONTRAST:
        return "Difference between light and dark";
    case S_COLOUR:
        return "0% grey, 100% the game's colours";
    case S_SCAN:
        return effect >= FX_CRT && !gpu_unavailable ? "Off while the CRT effect is on" :
               "Dark lines between the game's lines";
    case S_EFFECT:
        if (v && gpu_unavailable)
            return "Needs Direct3D 12 or Vulkan";
        return v == FX_SMOOTH ? "Rounds off the stairs on edges" :
               v >= FX_CRT ? "Looks like a CRT monitor" : "Smooth edges or CRT (uses the GPU)";
    case S_RESET:
        return "Everything on this page as the original";
    }
    return "";
}

const char *port_screen_help(int i)
{
    return port_screen_help_v(i, -1);
}

void port_screen_value(int i, char *buf, size_t n)
{
    static const char *scales[2] = { "Fit", "Sharp" };
    static const char *effects[FX_COUNT] = { "Off", "Smooth edges", "CRT", "CRT curved" };

    buf[0] = 0;
    switch (i) {
    case S_SHARP3D: {                   /* with the size the 3D is drawn at, on the game's screen */
        int sh = port_video.h >= 400 ? 400 : 200, w3, h3;
        port_s3d_size(port_sharp3d ? port_sharp3d : 1, sh, &w3, &h3);
        if (port_sharp3d)
            SDL_snprintf(buf, n, "%dx (%dx%d)", port_sharp3d, w3, h3);
        else
            SDL_snprintf(buf, n, "Classic (%dx%d)", w3, h3);
        break;
    }
    case S_WIDE:
        SDL_snprintf(buf, n, "%s", port_widescreen ? "On" : "Off");
        break;
    case S_AA:
        SDL_snprintf(buf, n, "%s%s", fxaa ? "FXAA" : "Off", fxaa && gpu_unavailable ? " (not available)" : "");
        break;
    case S_TERRAIN: {
        static const char *t[3] = { "Classic", "Medium", "High" };
        SDL_snprintf(buf, n, "%s", t[port_terrain]);
        break;
    }
    case S_SMOOTH:
        SDL_snprintf(buf, n, "%s", port_far_smooth ? "On" : "Off");
        break;
    case S_OBJECTS: {
        static const char *t[3] = { "Classic", "Far", "Farthest" };
        SDL_snprintf(buf, n, "%s", t[port_obj_dist]);
        break;
    }
    case S_ENEMIES:
        SDL_snprintf(buf, n, "%s", port_enemy_dist ? "Far" : "Classic");
        break;
    case S_FOG:
        SDL_snprintf(buf, n, "%s", port_fog ? "On" : "Off");
        break;
    case S_TRUECOL:
        SDL_snprintf(buf, n, "%s", port_truecol == 2 ? "On + sun glow (experimental)" : port_truecol ? "On" : "Off");
        break;
    case S_WATER:
        SDL_snprintf(buf, n, "%s", port_water_full == 2 ? "High (experimental)" : port_water_full ? "Medium" : "Low");
        break;
    case S_RIPPLES:
        SDL_snprintf(buf, n, "%s", port_water_ripples ? "On" : "Off");
        break;
    case S_WATERFX:
        SDL_snprintf(buf, n, "%s", port_water_fx ? "On" : "Off");
        break;
    case S_MAP:
        SDL_snprintf(buf, n, "%s", port_map_smooth ? "On" : "Off");
        break;
    case S_EFFECTS:
        SDL_snprintf(buf, n, "%.30s", port_fx_pack[0] ? port_fx_pack : "Off");
        break;
    case S_OBJPACK:
        SDL_snprintf(buf, n, "%.30s", port_obj_pack[0] ? port_obj_pack : "Off");
        break;
    case S_CKPACK:
        SDL_snprintf(buf, n, "%.30s", port_ck_pack[0] ? port_ck_pack : "Off");
        break;
    case S_ARMS:
        SDL_snprintf(buf, n, "%.30s", port_arms_pack[0] ? port_arms_pack : "Off");
        break;
    case S_HMOTION:
        SDL_snprintf(buf, n, "%s", port_helmet_motion ? "On" : "Off");
        break;
    case S_TREESIZE:
        SDL_snprintf(buf, n, "%s", port_tree_size ? "Realistic" : "Classic");
        break;
    case S_BURN:
        SDL_snprintf(buf, n, "%s", port_burn ? "Realistic" : "Classic");
        break;
    case S_GPULAND:
        SDL_snprintf(buf, n, "%s", port_gl ? "On" : "Off");
        break;
    case S_INFRARED:
        SDL_snprintf(buf, n, "%s", port_ir_style ? "Sci-fi" : "Classic");
        break;
    case S_SCALE:
        SDL_snprintf(buf, n, "%s", scales[scale_mode]);
        break;
    case S_VIDEOS:
        SDL_snprintf(buf, n, "%s%s", port_smooth_videos ? "30 fps" : "Off",
                     !port_smooth_videos ? "" : vsmooth_state() < 0 ? " (not available)" :
                     vsmooth_state() == 1 ? " (starting)" : "");
        break;
    case S_MOVIES:
        SDL_snprintf(buf, n, "%s", movie_fill ? "Fill screen" : "Classic");
        break;
    case S_BRIGHT:
        SDL_snprintf(buf, n, brightness ? "%+d%%" : "0", brightness);
        break;
    case S_GAMMA:
        SDL_snprintf(buf, n, "%d.%02d", gamma_pc / 100, gamma_pc % 100);
        break;
    case S_CONTRAST:
        SDL_snprintf(buf, n, contrast ? "%+d%%" : "0", contrast);
        break;
    case S_COLOUR:
        SDL_snprintf(buf, n, "%d%%", saturation);
        break;
    case S_SCAN:
        SDL_snprintf(buf, n, scanlines ? "%d%%" : "Off", scanlines);
        break;
    case S_EFFECT:
        SDL_snprintf(buf, n, "%s%s", effects[effect], effect && gpu_unavailable ? " (not available)" : "");
        break;
    }
}

void port_screen_change(int i, int dir)
{
    int d = dir < 0 ? -1 : 1;

    switch (i) {
    case S_SHARP3D: {
        static const int steps[4] = { 0, 2, 4, 6 };
        int k;
        for (k = 0; k < 4 && steps[k] != port_sharp3d; k++)
            ;
        port_sharp3d = steps[(k % 4 + 4 + d) % 4];
        break;
    }
    case S_WIDE:
        port_widescreen = !port_widescreen;
        break;
    case S_AA:
        fxaa = !fxaa;
        break;
    case S_TERRAIN:
        port_terrain = (port_terrain + 3 + d) % 3;
        break;
    case S_SMOOTH:
        port_far_smooth = !port_far_smooth;
        break;
    case S_OBJECTS:
        port_obj_dist = (port_obj_dist + 3 + d) % 3;
        break;
    case S_ENEMIES:
        port_enemy_dist = !port_enemy_dist;
        break;
    case S_FOG:
        port_fog = !port_fog;
        break;
    case S_TRUECOL:
        port_truecol = (port_truecol + 3 + d) % 3;
        break;
    case S_WATER:
        port_water_full = (port_water_full + 3 + d) % 3;
        break;
    case S_RIPPLES:
        port_water_ripples = !port_water_ripples;
        break;
    case S_WATERFX:
        port_water_fx = !port_water_fx;
        break;
    case S_MAP:
        port_map_smooth = !port_map_smooth;
        break;
    case S_EFFECTS:                     /* Off, then each mod with an Effects folder (Objects, ...) */
    case S_OBJPACK:
    case S_CKPACK:
    case S_ARMS: {
        static char packs[32][64];
        char *pack = i == S_OBJPACK ? port_obj_pack : i == S_CKPACK ? port_ck_pack : i == S_ARMS ? port_arms_pack :
                     port_fx_pack;
        int np = i == S_OBJPACK ? port_obj_packs(packs, 32) : i == S_CKPACK ? port_ck_packs(packs, 32) :
                 i == S_ARMS ? port_arms_packs(packs, 32) : port_fx_packs(packs, 32), k, cur = -1;
        for (k = 0; k < np; k++)
            if (!SDL_strcasecmp(packs[k], pack))
                cur = k;
        cur += d;
        if (cur < -1)
            cur = np - 1;
        if (cur >= np)
            cur = -1;
        SDL_snprintf(pack, sizeof port_fx_pack, "%s", cur < 0 ? "" : packs[cur]);
        break;
    }
    case S_HMOTION:
        port_helmet_motion = !port_helmet_motion;
        break;
    case S_TREESIZE:
        port_tree_size = !port_tree_size;
        break;
    case S_BURN:
        port_burn = !port_burn;
        break;
    case S_GPULAND:
        port_gl = !port_gl;
        break;
    case S_INFRARED:
        port_ir_style = !port_ir_style;
        break;
    case S_SCALE:
        scale_mode = !scale_mode;       /* Fit, Sharp */
        break;
    case S_VIDEOS:
        port_smooth_videos = !port_smooth_videos;
        if (port_smooth_videos)
            vsmooth_start(0, -1);       /* loads in the background; the next movie is smoothed */
        break;
    case S_MOVIES:
        movie_fill = !movie_fill;
        break;
    case S_BRIGHT:
        brightness = clampi(brightness + 5 * d, -50, 50);
        break;
    case S_GAMMA:
        gamma_pc = clampi(gamma_pc + 5 * d, 50, 200);
        break;
    case S_CONTRAST:
        contrast = clampi(contrast + 5 * d, -50, 50);
        break;
    case S_COLOUR:
        saturation = clampi(saturation + 10 * d, 0, 200);
        break;
    case S_SCAN:
        scanlines = clampi(scanlines + 10 * d, 0, 100);
        break;
    case S_EFFECT:
        effect = (effect + FX_COUNT + d) % FX_COUNT;
        break;
    case S_RESET:
        port_screen_filters_off();
        break;
    }
    serial++;
}

/* the options screen's buttons and sliders (options.c): a choice setting's value as the index of
 * its button (3D resolution: Classic, 2x, 4x, 6x), a filter's as its number */
int port_screen_get(int i)
{
    switch (i) {
    case S_SHARP3D: return port_sharp3d / 2;
    case S_WIDE: return port_widescreen != 0;
    case S_AA: return fxaa;
    case S_TERRAIN: return port_terrain;
    case S_SMOOTH: return port_far_smooth != 0;
    case S_OBJECTS: return port_obj_dist;
    case S_ENEMIES: return port_enemy_dist != 0;
    case S_FOG: return port_fog != 0;
    case S_TRUECOL: return port_truecol;
    case S_WATER: return port_water_full;
    case S_RIPPLES: return port_water_ripples != 0;
    case S_WATERFX: return port_water_fx != 0;
    case S_MAP: return port_map_smooth != 0;
    case S_HMOTION: return port_helmet_motion != 0;
    case S_TREESIZE: return port_tree_size != 0;
    case S_BURN: return port_burn != 0;
    case S_GPULAND: return port_gl != 0;
    case S_INFRARED: return port_ir_style != 0;
    case S_SCALE: return scale_mode;
    case S_VIDEOS: return port_smooth_videos != 0;
    case S_MOVIES: return movie_fill;
    case S_EFFECT: return effect;
    case S_BRIGHT: return brightness;
    case S_GAMMA: return gamma_pc;
    case S_CONTRAST: return contrast;
    case S_COLOUR: return saturation;
    case S_SCAN: return scanlines;
    }
    return 0;
}

/* a filter's range and step (sliders); 0 for a setting that is no filter */
int port_screen_range(int i, int *lo, int *hi, int *step)
{
    switch (i) {
    case S_BRIGHT: case S_CONTRAST: *lo = -50; *hi = 50; *step = 5; return 1;
    case S_GAMMA: *lo = 50; *hi = 200; *step = 5; return 1;
    case S_COLOUR: *lo = 0; *hi = 200; *step = 10; return 1;
    case S_SCAN: *lo = 0; *hi = 100; *step = 10; return 1;
    }
    return 0;
}

/* sets what port_screen_get gives: a filter's number (kept to its range and steps), a choice
 * setting's button, changed as Left/Right change it (so Smooth videos starts its model) */
void port_screen_set(int i, int v)
{
    int lo, hi, step, k;
    if (port_screen_range(i, &lo, &hi, &step)) {
        v = clampi(lo + (v - lo + step / 2) / step * step, lo, hi);
        switch (i) {
        case S_BRIGHT: brightness = v; break;
        case S_GAMMA: gamma_pc = v; break;
        case S_CONTRAST: contrast = v; break;
        case S_COLOUR: saturation = v; break;
        case S_SCAN: scanlines = v; break;
        }
        serial++;
        return;
    }
    for (k = 0; k < 8 && port_screen_get(i) != v; k++)
        port_screen_change(i, 1);
}
