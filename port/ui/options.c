/* options.c -- the Freefall options screen (Terra Nova: Freefall quality-of-life settings).
 *
 * O opens and closes it, in the game's menus and in missions (owner: the game's own
 * options screen moved to Alt+O); in a mission Esc opens it too
 * (esc_opens: where the game itself has no use for Esc).  It is drawn by the main thread over the
 * game's picture, in the style of the game's own options screen (owner's design): a
 * 640 x 480 picture in that screen's palette with its slider ticks, arrows and pointer and the
 * game's font (RESGAME.RES 0x188, 0x164, read by gameart.c), as big as the game's picture allows
 * at 4:3; the logo and the preview pictures (built in; see pic_get) over it at the
 * window's own pixels.  While it is open the game gets no keys or buttons, and in a mission the
 * game waits at the start of its next frame (port_frame_limit), so the mission stands still.
 * Every change takes effect at once and is saved in tnfree.ini.  With every improvement off the
 * game is the original.
 * Left: Fullscreen, Preset (Freefall, Vanilla+, Classic; Custom lit when the settings are none of
 * them, worked out from the settings themselves), the five settings pages, Mods and Show FPS.  The
 * page's settings fill the middle and right columns (pages[]): buttons (one for each value),
 * sliders (30 ticks, as the game's), lists with arrows, a preview picture.  Gameplay also shows
 * the game's own sound options (volumes, stereo: port_game_option, src/game/shell/options.c).
 * Under them: the help line, KEY BINDINGS, CLOSE, the FREEFALL box (the website) and GAME
 * OPTIONS (the game's own screen, its O key, in a mission).  The mouse only (owner): it
 * clicks a button, drags a slider, the wheel changes the setting under it; Esc closes.  A setting that needs another one first is
 * drawn dim (port_screen_usable, opt_usable).
 * The Keys page lists the controls style's actions with their two keys (keys.c): a click on a
 * key waits for the new key or mouse button (Backspace clears it, Esc cancels), DEFAULT KEYS
 * brings back the style's default keys, Esc goes back.  A key another action had is swapped with
 * it.
 * On the first start (no settings in tnfree.ini yet) a question picks the preset, once the game's
 * menu is up (port_options_welcome, main.c): the three presets side by side with their pictures.
 */
#include <SDL3/SDL.h>
#include <stdio.h>
#include "port.h"
#include "platform.h"
#include "keys.h"
#include "screen.h"
#include "sharp3d.h"
#include "version.h"
#include "cockpitpack.h"
#include "arms.h"
#include "videosmooth.h"
#include "gameart.h"
#include "helmetfx.h"
#include <string.h>

int port_options_open;
int port_smooth_fps;
int port_auto_targcam;                  /* left screen: target camera for a new target (control.c) */
int port_game_options_up;               /* the game's own options screen (O) is up */
static SDL_FRect ffbutton;              /* where the Freefall options button was drawn */
static SDL_FRect ffmenu;                /* the main menu's Freefall Options button (uiwidget.c) */
extern int port_menu_ffbutton;          /* it is up (src/game/shell/uiwidget.c) */
int port_options_welcome;               /* the first start: ask for the preset (main.c) */
int port_game_typing;                   /* a saved game's name is being typed (saveload.c) */
int port_game_dialog_up;                /* the game's yes / no box is up (src/game/main/ffmain.c) */
extern volatile int port_options_held;  /* the mission stands still for this screen (platform.c) */
static int esc_held;                    /* Esc opened the options and is still down */
extern unsigned char hud_38FC80;        /* the squad's orders menu is up (src/game/hud/cmdpanel.c) */
extern int port_missions_open, port_ms_all, port_ms_fading;    /* the Mission Selector (missions.c) */
void port_missions_changed(void);

void port_set_smooth_fps(int on)
{
    port_smooth_fps = on;
    port_frame_us = on ? 1000000 / 60 : 40000;   /* 16667 us: exactly 60 a second */
    port_highfps_fix = on;
}

/* the options: Preset and the settings options.c keeps (screen.c keeps the rest) */
enum { O_PRESET, O_FPS, O_CTRL, O_SENS, O_INV, O_TCAM, O_TLABEL, O_MAPCONE, O_SHOWFPS, O_MUSIC, O_PACK,
       O_MODSET, O_FULLSCREEN, O_SOUNDFONT, O_ENEMIES, O_RADAR, O_AIM, O_AIMSIGN, O_MSEL, O_COUNT };
/* the pages: the five settings pages and Mods, the key bindings, the first start's question */
enum { PG_GAMEPLAY, PG_DISPLAY, PG_DETAIL, PG_COCKPIT, PG_SOUND, PG_MODS, PG_KEYS, PG_WELCOME };
static int page;                        /* the page shown */
/* the Keys page */
static int ksel, kcol, ktop, capturing;
static char kstatus[96];

static const char *style_names[2] = { "Classic", "Advanced (recommended)" };   /* (Vanilla before) */
static const char *preset_names[3] = { "Classic", "Vanilla+", "Freefall (recommended)" };

/* "Auto-aim" (Gameplay page): with a locked target (T, Y, a right click) and the pointer within
 * this many pixels of its box (twice that up and down with the goggles), the shot goes to the
 * target itself: a beam ahead of it by its speed, a grenade or missile lobbed onto it
 * (hudmain.c hud_aim_on_target).  20 is the game's; 0 (Off) never hands the shot the lock, not
 * even a click in a target camera's view, which otherwise uses it wherever it is (hview3d.c).
 * The owner's steps: 20 (classic), 5 and Off; 20 only with the Classic controls style.  With
 * Advanced a 20 kept from Classic shows grey and works as 5 (port_aim_box_now). */
int port_aim_box = AIM_CLASSIC;
int port_aim_box_now(void)
{
    return port_aim_box == AIM_CLASSIC && port_mouselook ? AIM_SMALL : port_aim_box;
}

/* "Auto-aim highlight" (Gameplay page, hudmain.c port_hud_aim_sign): while a shot would go to
 * the locked target, the crosshair's bright green middle turns red.  On in the Freefall preset. */
int port_aim_sign;
#define NMUSIC 5
#define MUSIC_OPL3 2                    /* index of "opl3" (owner's order) */
static const char *music_modes[NMUSIC] = { "gm", "soundfont", "opl3", "opl2", "off" };
static const char *music_names[NMUSIC] = { "General MIDI", "SoundFont (AWE32)", "OPL3 FM", "OPL2 FM", "Off" };

/* ------------------------------------------------------------------ Mod set: the same name for the
 * cockpit, arms, effects and objects packs (a mod's folders: Mods/<name>.zip, fxpack.c) */
static char *kind_pack(int k) { return k == 0 ? port_ck_pack : k == 1 ? port_arms_pack : k == 2 ? port_fx_pack : port_obj_pack; }

/* the Mods folder's sets and each kind's packs, read again at most once a second (P135: the
 * preset and the mod set were worked out from the folder and its zips several times each time
 * the screen was drawn: hundreds of directory listings a second) */
static struct {
    uint64_t at;                        /* when they were read (0: read them again) */
    int nsets, n[4], nmusic;
    char sets[32][64], packs[4][64][64], music[32][64];    /* music: the music packs (Music/) */
} mods;

static void mods_read(void)
{
    uint64_t now = SDL_GetTicks();
    int k;
    if (mods.at && now - mods.at < 1000)
        return;
    mods.at = now ? now : 1;
    mods.nsets = port_mod_sets(mods.sets, 32);
    for (k = 0; k < 4; k++)
        mods.n[k] = k == 0 ? port_ck_packs(mods.packs[k], 64) : k == 1 ? port_arms_packs(mods.packs[k], 64) :
                    k == 2 ? port_fx_packs(mods.packs[k], 64) : port_obj_packs(mods.packs[k], 64);
    mods.nmusic = port_music_packs(mods.music, 32);
}

static int kind_has(int k, const char *name)
{
    char own[64][64], (*l)[64] = own;
    int i, n;
    if (SDL_IsMainThread()) {
        mods_read();
        l = mods.packs[k];
        n = mods.n[k];
    } else                              /* (P137: help.c asks on the game's thread too, as H opens
                                         * the help: not the main thread's list, it may be being read) */
        n = k == 0 ? port_ck_packs(own, 64) : k == 1 ? port_arms_packs(own, 64) : k == 2 ? port_fx_packs(own, 64) :
            port_obj_packs(own, 64);
    for (i = 0; i < n; i++)
        if (!SDL_strcasecmp(l[i], name))
            return 1;
    return 0;
}

static int modset_is(const char *name)  /* every pack is name's (Off where that kind has none) */
{
    int k;
    for (k = 0; k < 4; k++)
        if (SDL_strcasecmp(kind_pack(k), name[0] && kind_has(k, name) ? name : ""))
            return 0;
    return 1;
}

static void modset_set(const char *name)
{
    int k;
    for (k = 0; k < 4; k++)
        SDL_snprintf(kind_pack(k), sizeof port_fx_pack, "%s", name[0] && kind_has(k, name) ? name : "");
}

static int modset_now(char *buf, size_t sz)    /* its set's name or Off: 1; Custom: 0 */
{
    int i;
    if (modset_is("")) {
        snprintf(buf, sz, "Off");
        return 1;
    }
    mods_read();
    for (i = 0; i < mods.nsets; i++)
        if (modset_is(mods.sets[i])) {
            snprintf(buf, sz, "%.30s", mods.sets[i]);
            return 1;
        }
    snprintf(buf, sz, "Custom");
    return 0;
}

static void modset_change(int dir)      /* Off, then each mod set */
{
    char name[64];
    int n, i, cur = -1;
    mods.at = 0;                        /* (as the folder is now) */
    mods_read();
    n = mods.nsets;
    for (i = 0; i < n; i++)
        if (modset_is(mods.sets[i]))
            cur = i;
    cur += dir < 0 ? -1 : 1;
    if (cur < -1)
        cur = n - 1;
    if (cur >= n)
        cur = -1;
    snprintf(name, sizeof name, "%s", cur < 0 ? "" : mods.sets[cur]);
    modset_set(name);
}

/* ------------------------------------------------------------------ presets */
/* the music pack preset p plays (owner): with Freefall the Freefall Music pack (the
 * game's music as MP3 by Rich "Weeds" Nagel, Music/Freefall-MP3.zip in the release) when it is
 * there, its name as the Music folder spells it; the game's own music ("") otherwise */
#define FREEFALL_MUSIC "Freefall-MP3"
static const char *preset_music(int p, char name[64])
{
    char own[32][64], (*l)[64] = own;
    int i, n;
    name[0] = 0;
    if (p != 2)
        return name;
    if (SDL_IsMainThread()) {           /* (as kind_has: the list read at most once a second) */
        mods_read();
        l = mods.music;
        n = mods.nmusic;
    } else
        n = port_music_packs(own, 32);
    for (i = 0; i < n; i++)
        if (!SDL_strcasecmp(l[i], FREEFALL_MUSIC))
            SDL_strlcpy(name, l[i], 64);
    return name;
}

static int preset_now(void)             /* 0 Classic, 1 Vanilla+, 2 Freefall, -1 Custom */
{
    char music[64];
    int p, on;
    for (p = 0; p < 3; p++) {
        on = p > 0;
        if (!port_smooth_fps == !on && !port_mouselook == !on && !port_auto_targcam == !on &&
            !port_target_label == !on && !port_map_cone == !on && !port_map_enemies == !(p == 2) &&
            port_aim_box == (p > 0 ? AIM_SMALL : AIM_CLASSIC) && !port_aim_sign == !(p == 2) &&
            port_screen_is_preset(p) &&
            modset_is(p == 2 ? "Freefall" : "") && !SDL_strcasecmp(port_music_pack, preset_music(p, music)))
            return p;
    }
    return -1;
}

/* help.c (game thread): H shows Freefall's help screen unless the settings are the Classic preset's */
int port_help_wanted(void)
{
    return preset_now() != 0;
}

/* something a preset turned on is still starting: the RIFE model (Smooth videos) or the GPU
 * renderer (Anti-aliasing) */
static int starting(void)
{
    return vsmooth_state() == 1 || port_renderer_switching();
}

static void apply_preset(int p)
{
    char music[64];
    port_set_smooth_fps(p > 0);
    port_mouselook = p > 0;
    port_auto_targcam = p > 0;
    port_target_label = p > 0;
    port_map_cone = p > 0;
    port_map_enemies = p == 2;          /* Show enemies without MFD: Freefall only (owner) */
    port_aim_box = p > 0 ? AIM_SMALL : AIM_CLASSIC;    /* Auto-aim: 20 needs the Classic controls */
    port_aim_sign = p == 2;             /* Auto-aim highlight: Freefall only (owner) */
    port_screen_preset(p);
    modset_set(p == 2 ? "Freefall" : "");
    if (SDL_strcasecmp(port_music_pack, preset_music(p, music))) {
        SDL_strlcpy(port_music_pack, music, sizeof port_music_pack);
        port_music_pack_changed();      /* (heard at once, as on the Music pack line) */
    }
}

/* main.c: a tnfree.ini from before "Auto-aim" (no auto_aim line): the game's box with the
 * Classic controls style, the small one with Advanced (as the presets set them); without an
 * aim_highlight line, Auto-aim highlight as its preset: on for the Freefall preset's settings */
void port_options_aim_legacy(int box, int highlight)
{
    if (box)
        port_aim_box = port_mouselook ? AIM_SMALL : AIM_CLASSIC;
    if (highlight) {
        port_aim_sign = 1;
        if (preset_now() != 2)
            port_aim_sign = 0;
    }
}

/* --preset (main.c): classic, vanilla+ (or vanillaplus) or freefall; 0 for another name */
int port_options_preset(const char *name)
{
    int p = !SDL_strcasecmp(name, "classic") ? 0 :
            !SDL_strcasecmp(name, "vanilla+") || !SDL_strcasecmp(name, "vanillaplus") ? 1 :
            !SDL_strcasecmp(name, "freefall") ? 2 : -1;
    if (p < 0)
        return 0;
    apply_preset(p);
    return 1;
}

/* ------------------------------------------------------------------ options.c's settings */
static const char *opt_labels[O_COUNT] = {
    "Preset", "Frame rate", "Controls style", "Mouse sensitivity", "Invert mouse", "Auto target camera",
    "Target label", "Head view cone", "Show FPS", "Music", "Music pack", "Mod set", "Fullscreen", "SoundFont for AWE32",
    "Show enemies without MFD", "Radar without MFD (Experimental)", "Auto-aim", "Auto-aim highlight",
    "Mission select",
};

static int music_index(void)            /* "fm" (earlier builds) is OPL3; "auto" is set to what it
                                         * chose when the music starts (miles.c) */
{
    int k;
    for (k = 0; k < NMUSIC; k++)
        if (!strcmp(port_music_mode, music_modes[k]))
            return k;
    return MUSIC_OPL3;
}

/* 0: the line needs another setting first (drawn grey, Left/Right do nothing) */
static int opt_usable(int i)
{
    if (i == O_SOUNDFONT)               /* only the SoundFont (AWE32) music plays it */
        return !strcmp(port_music_mode, "soundfont");
    if (i == O_RADAR)                   /* drawn at the 3D picture's pixels */
        return port_sharp3d != 0;
    if (i == O_ENEMIES)                 /* the radar shows the map's enemies itself */
        return !(port_map_radar && port_sharp3d);
    if (i == O_AIMSIGN)                 /* nothing to show with Auto-aim Off */
        return port_aim_box != 0;
    if (i == O_MAPCONE)                 /* the head turns with the Advanced controls only (owner) */
        return port_mouselook != 0;
    return 1;
}

/* 1: the value can't be used now and shows grey, but Left/Right still change it */
static int opt_dim(int i)
{
    return i == O_AIM && port_aim_box == AIM_CLASSIC && port_mouselook;   /* 20: Classic controls */
}

static void opt_value(int i, char *buf, size_t n)
{
    buf[0] = 0;
    switch (i) {
    case O_PRESET: {
        int p = preset_now();
        if (p >= 0 && starting())
            snprintf(buf, n, "%s (starting...)", p == 2 ? "Freefall" : preset_names[p]);
        else
            snprintf(buf, n, "%s", p < 0 ? "Custom" : preset_names[p]);
        break;
    }
    case O_FPS:
        snprintf(buf, n, "%s", port_smooth_fps ? "60 fps" : "25 fps (classic)");
        break;
    case O_CTRL:
        snprintf(buf, n, "%s", style_names[port_mouselook ? 1 : 0]);
        break;
    case O_SENS:
        snprintf(buf, n, "%d%%", port_mouselook_sens);
        break;
    case O_INV:
        snprintf(buf, n, "%s", port_mouselook_invert ? "On" : "Off");
        break;
    case O_TCAM:
        snprintf(buf, n, "%s", port_auto_targcam ? "On" : "Off");
        break;
    case O_TLABEL:
        snprintf(buf, n, "%s", port_target_label ? "Around the box" : "Classic");
        break;
    case O_MAPCONE:
        snprintf(buf, n, "%s", port_map_cone ? "On" : "Off");
        break;
    case O_ENEMIES:
        snprintf(buf, n, "%s", port_map_enemies ? "On" : "Off");
        break;
    case O_RADAR:
        snprintf(buf, n, "%s", port_map_radar ? "On" : "Off");
        break;
    case O_AIM:
        if (!port_aim_box)
            snprintf(buf, n, "Off");
        else
            snprintf(buf, n, "%d px%s", port_aim_box, port_aim_box == AIM_CLASSIC ? " (classic)" : "");
        break;
    case O_AIMSIGN:
        snprintf(buf, n, "%s", port_aim_sign ? "On" : "Off");
        break;
    case O_MSEL:
        snprintf(buf, n, "%s", port_ms_all ? "All (cheat)" : "Reached");
        break;
    case O_SHOWFPS:
        snprintf(buf, n, "%s", port_show_fps ? "On" : "Off");
        break;
    case O_MUSIC:
        snprintf(buf, n, "%s", music_names[music_index()]);
        break;
    case O_PACK:
        snprintf(buf, n, "%.30s", port_music_pack[0] ? port_music_pack : "Off (game's music)");
        break;
    case O_SOUNDFONT:
        port_soundfont_name(buf, n);
        break;
    case O_MODSET:
        modset_now(buf, n);
        break;
    case O_FULLSCREEN:
        snprintf(buf, n, "%s", port_fullscreen ? "On" : "Off (window)");
        break;
    }
}

/* the help for value v (a button's index; -1: the value now), shown for the button under the
 * mouse (owner: the help as you hover, not only after a click) */
static const char *opt_help_v(int i, int v)
{
    static char info[64];
    int now = v < 0;
    if (now) {
        switch (i) {                    /* (the buttons' order, as g_get) */
        case O_PRESET: { int p = preset_now(); v = p < 0 ? 3 : 2 - p; break; }
        case O_FPS: v = port_smooth_fps != 0; break;
        case O_AIM: v = port_aim_box == AIM_CLASSIC ? 2 : port_aim_box ? 1 : 0; break;
        case O_MSEL: v = port_ms_all != 0; break;
        case O_FULLSCREEN: v = port_fullscreen != 0; break;
        case O_MUSIC: v = music_index(); break;
        case O_TCAM: v = port_auto_targcam != 0; break;
        case O_TLABEL: v = port_target_label != 0; break;
        }
    }
    switch (i) {
    case O_PRESET:
        switch (v < 3 ? 2 - v : -1) {
        case 0: return "The original game, nothing changed";
        case 1: return "Classic look: wider, smoother, clearer";
        case 2: return "Every improvement on";
        }
        return "Your own mix; Left/Right picks a preset";
    case O_FPS: return v ? "Smooth gameplay at 60 frames a second" : "The game's own 25 frames a second";
    case O_CTRL: return "Classic: original controls; Advanced: mouselook";   /* (owner) */
    case O_SENS: return "How far the view moves for the mouse";
    case O_INV: return "Mouse forward looks down";
    case O_AIM:
        if (v == 2 && port_mouselook)
            return now ? "Requires Classic controls; 5 px used now" : "20 px needs the Classic controls";
        return v ? "Shots this close to the target aim at it" : "Shots go where you point, lock or not";
    case O_AIMSIGN:
        if (!opt_usable(O_AIMSIGN))
            return "Needs Auto-aim on";
        return "The crosshair turns red when a shot will connect";   /* (owner) */
    case O_MSEL: return v ? "Mission Selector: every campaign mission" :
                                      "Mission Selector: the missions you reached";
    case O_TCAM: return v ? "Left screen shows the target you pick" : "Left screen keeps the squad list";
    case O_TLABEL: return v ? "Distance above the box, Locked under" : "The game's own target text";
    case O_MAPCONE:
        if (!opt_usable(O_MAPCONE))
            return "Needs the Advanced controls";
        return "Shaded cone of your head's view";
    case O_ENEMIES:
        if (!opt_usable(O_ENEMIES))
            return "The radar shows the map's enemies";
        return "Show enemies even when MFD screen in Goggles is off";
    case O_RADAR:
        if (!opt_usable(O_RADAR))
            return "Needs 3D resolution 2x, 4x or 6x";
        return "Goggles' screens off (Tab): map as a radar";
    case O_SHOWFPS: return "Game fps, display fps, 3D drawing time";
    case O_FULLSCREEN: return v ? "The game fills the screen; Alt+Enter too" :
                                                "A window you can resize; Alt+Enter too";
    case O_MUSIC: {
        static const char *const help[NMUSIC] = { "Windows' MIDI synth", "Sound Blaster AWE32 sound",
                                                  "Sound Blaster 16 FM sound", "AdLib FM sound", "No music" };
        return v < NMUSIC ? help[v] : "";
    }
    case O_SOUNDFONT:
        if (!opt_usable(O_SOUNDFONT))
            return "Needs Music: SoundFont (AWE32)";
        if (port_soundfont_note()[0])
            return port_soundfont_note();
        return "For SoundFont music: .sf2 / .dls in Music";
    case O_PACK: return "OGG / MP3 files: a folder or zip in Music";
    case O_MODSET:
        mods_read();
        if (!mods.nsets)
            return "No mod zips in the Mods folder";
        if (modset_now(info, sizeof info) && strcmp(info, "Off")) {
            static char name[64], text[64];
            static uint64_t at;
            if (strcmp(name, info) || !at || SDL_GetTicks() - at >= 1000) {  /* (its mod.txt, P135) */
                snprintf(name, sizeof name, "%s", info);
                port_mod_set_info(name, text, 45);     /* at most 44 characters fit the help line (P83) */
                at = SDL_GetTicks() | 1;
            }
            return text[0] ? text : "The four mods below from one mod set";
        }
        return strcmp(info, "Off") ? "Mods from more than one mod set" : "Off: each mod chosen below";
    }
    return "";
}

static const char *opt_help(int i) { return opt_help_v(i, -1); }

static void opt_change(int i, int dir)
{
    if (!opt_usable(i))
        return;
    switch (i) {
    case O_PRESET: {
        int p = preset_now();
        p = p < 0 ? (dir < 0 ? 2 : 0) : (p + 3 + (dir < 0 ? -1 : 1)) % 3;
        apply_preset(p);
        break;
    }
    case O_FPS:
        port_set_smooth_fps(!port_smooth_fps);
        break;
    case O_CTRL:
        port_mouselook = !port_mouselook;
        break;
    case O_SENS:
        port_mouselook_sens += dir < 0 ? -10 : 10;
        if (port_mouselook_sens < 10)
            port_mouselook_sens = 10;
        if (port_mouselook_sens > 500)
            port_mouselook_sens = 500;
        break;
    case O_INV:
        port_mouselook_invert = !port_mouselook_invert;
        break;
    case O_AIM:
        if (port_mouselook)             /* Advanced: 5 px and Off (20 needs the Classic controls) */
            port_aim_box = port_aim_box == AIM_CLASSIC ? (dir < 0 ? 0 : AIM_SMALL) :
                           port_aim_box ? 0 : AIM_SMALL;
        else                            /* Classic: 20, 5, Off */
            port_aim_box = dir > 0 ? (port_aim_box == AIM_CLASSIC ? AIM_SMALL : port_aim_box ? 0 : AIM_CLASSIC)
                                   : (port_aim_box == AIM_CLASSIC ? 0 : port_aim_box ? AIM_CLASSIC : AIM_SMALL);
        break;
    case O_AIMSIGN:
        port_aim_sign = !port_aim_sign;
        break;
    case O_MSEL:                        /* (not part of the presets) */
        port_ms_all = !port_ms_all;
        port_missions_changed();
        break;
    case O_TCAM:
        port_auto_targcam = !port_auto_targcam;
        break;
    case O_TLABEL:
        port_target_label = !port_target_label;
        break;
    case O_MAPCONE:
        port_map_cone = !port_map_cone;
        break;
    case O_ENEMIES:
        port_map_enemies = !port_map_enemies;
        break;
    case O_RADAR:
        port_map_radar = !port_map_radar;
        break;
    case O_SHOWFPS:
        port_show_fps = !port_show_fps;
        break;
    case O_FULLSCREEN:                  /* the window changes in the main loop (platform.c) */
        port_fullscreen = !port_fullscreen;
        break;
    case O_MUSIC:
        snprintf(port_music_mode, sizeof port_music_mode, "%s",
                 music_modes[(music_index() + NMUSIC + (dir < 0 ? -1 : 1)) % NMUSIC]);
        port_music_reopen();            /* at once, the piece carries on */
        break;
    case O_SOUNDFONT: {                 /* each .sf2 / .dls / .sbk in Music/, Windows' gm.dls */
        static char fonts[32][64];
        int n = port_soundfonts(fonts, 32), k, cur = 0;     /* none named or found: the first plays */
        if (!n)
            break;
        for (k = 0; k < n; k++)
            if (!SDL_strcasecmp(fonts[k], port_soundfont))
                cur = k;
        cur = (cur + n + (dir < 0 ? -1 : 1)) % n;
        snprintf(port_soundfont, sizeof port_soundfont, "%s", fonts[cur]);
        if (!strcmp(port_music_mode, "soundfont"))
            port_music_reopen();
        break;
    }
    case O_PACK: {                      /* Off, then each folder and zip in Music/ */
        static char packs[32][64];
        int n = port_music_packs(packs, 32), k, cur = -1;
        for (k = 0; k < n; k++)
            if (!SDL_strcasecmp(packs[k], port_music_pack))
                cur = k;
        cur += dir < 0 ? -1 : 1;
        if (cur < -1)
            cur = n - 1;
        if (cur >= n)
            cur = -1;
        snprintf(port_music_pack, sizeof port_music_pack, "%s", cur < 0 ? "" : packs[cur]);
        port_music_pack_changed();
        break;
    }
    case O_MODSET:
        modset_change(dir);
        break;
    }
    port_save_settings();
}

/* ------------------------------------------------------------------ the screen's picture
 * A 640 x 480 picture in the colours of the game's own options screen (RESGAME.RES 0x188: its
 * palette, slider ticks 2 / 3, pointer 5, arrows 8 / 9) with the game's font 0x164, read by
 * gameart.c; drawn as big as the game's picture allows at 4:3, the logo and the preview pictures
 * over it at the window's own pixels */
#define CW 640
#define CH 480
static uint8_t cv[CH][CW];
static uint8_t opal[768];
static uint32_t orgba[CW * CH];
static GaBitmap bm_tick[2], bm_ptr, bm_arrow[2];
static GaFont ofont;
static int art = -1;                    /* loaded 1, failed 0, not tried -1 */
static SDL_Texture *ctex, *gbtex;  /* the screen; the button on the game's options screen */
enum { BLACK = 0, ORANGE = 248, BROWN = 249, GREY = 250, LIGHT = 252, WHITE = 247, DIM = 253,
       BLUE = 175, BLUE_LIT = 171 };
#define TH 15                           /* a title's height; buttons' */
#define BH 17
#define GAP 5
#define Y0 40                           /* the columns' top and bottom */
#define Y1 404
static const int colx[3] = { 1, 216, 430 };

static int red_c;                       /* (load_art) */

static int load_art(void)
{
    GaRes f = { 0 };
    uint8_t *b;
    uint32_t n, in;
    int c, ok;
    GaBitmap bg = { 0 };
    const uint8_t *it;

    if (art >= 0)
        return art;
    art = 0;
    if (!ga_res_open(&f, "C:\\TNOVA\\DATA\\RESGAME.RES")) {
        port_log("Options: RESGAME.RES not found\n");
        return 0;
    }
    b = ga_res_read(&f, 0x188, &n, &c);
    ok = b && c && ga_font_load(&f, 0x164, &ofont);
    ga_res_close(&f);
    if (!ok) {
        port_log("Options: the game's options art (RESGAME.RES 0x188, 0x164) not read\n");
        SDL_free(b);
        return 0;
    }
    it = ga_item(b, n, 0, &in);
    ok = ga_bm_decode(it, in, &bg, b, n, opal);
    SDL_free(bg.px);
    it = ga_item(b, n, 2, &in);
    ok = ok && ga_bm_decode(it, in, &bm_tick[0], NULL, 0, NULL);
    it = ga_item(b, n, 3, &in);
    ok = ok && ga_bm_decode(it, in, &bm_tick[1], NULL, 0, NULL);
    it = ga_item(b, n, 5, &in);
    ok = ok && ga_bm_decode(it, in, &bm_ptr, NULL, 0, NULL);
    it = ga_item(b, n, 8, &in);
    ok = ok && ga_bm_decode(it, in, &bm_arrow[0], NULL, 0, NULL);
    it = ga_item(b, n, 9, &in);
    ok = ok && ga_bm_decode(it, in, &bm_arrow[1], NULL, 0, NULL);
    SDL_free(b);
    if (ok) {                           /* the palette's red nearest a dark red (Difficulty level's title) */
        int i, best = 1 << 30;
        for (i = 1; i < 256; i++) {
            int dr = opal[i * 3] - 150, dg = opal[i * 3 + 1] - 20, db = opal[i * 3 + 2] - 12, d = dr * dr + dg * dg + db * db;
            if (d < best)
                best = d, red_c = i;
        }
    }
    art = ok;
    return art;
}

static void fill(int x0, int y0, int x1, int y1, int c)
{
    int y;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > CW) x1 = CW;
    if (y1 > CH) y1 = CH;
    for (y = y0; y < y1; y++)
        if (x1 > x0)
            memset(&cv[y][x0], c, (size_t)(x1 - x0));
}

static void panel(int x0, int y0, int x1, int y1)  /* grey, a light line left and above */
{
    fill(x0, y0, x1, y1, GREY);
    fill(x0, y0, x1, y0 + 1, LIGHT);
    fill(x0, y0, x0 + 1, y1, LIGHT);
}

static void box(int x0, int y0, int x1, int y1, int c)     /* a light line right and below */
{
    fill(x0, y0, x1, y1, c);
    fill(x0, y1, x1 + 1, y1 + 1, LIGHT);
    fill(x1, y0, x1 + 1, y1 + 1, LIGHT);
}

static void text_at(int x, int y, const char *s, int col)
{
    for (; *s; s++) {
        int x0, w = ga_glyph(&ofont, (unsigned char)*s, &x0), xx, yy;
        for (yy = 0; yy < ofont.h; yy++)
            for (xx = 0; xx < w; xx++) {
                int sx = x0 + xx, px = x + xx, py = y + yy;
                if (px < 0 || px >= CW || py < 0 || py >= CH)
                    continue;
                if (ofont.colour) {
                    if (ofont.b[ofont.buf + yy * ofont.row + sx])
                        cv[py][px] = (uint8_t)col;
                } else if (ofont.b[ofont.buf + yy * ofont.row + (sx >> 3)] & (0x80 >> (sx & 7)))
                    cv[py][px] = (uint8_t)col;
            }
        x += w;
    }
}

/* s centred between x0 and x1, cut (with "..") to fit */
static void ctext(int x0, int x1, int y, const char *s, int col)
{
    char t[128];
    size_t l;
    snprintf(t, sizeof t, "%s", s);
    l = strlen(t);
    while (l > 3 && ga_text_w(&ofont, t) > x1 - x0 - 4) {
        t[--l] = 0;
        t[l - 1] = t[l - 2] = '.';
    }
    text_at(x0 + (x1 - x0 - ga_text_w(&ofont, t)) / 2, y, t, col);
}

/* the arrow b turned a quarter clockwise (left: up, right: down), at x, y (b->h wide, b->w high) */
static void blit_turned(const GaBitmap *b, int x, int y)
{
    int xx, yy;
    if (b->px)
        for (yy = 0; yy < b->h; yy++)
            for (xx = 0; xx < b->w; xx++) {
                int dx = x + b->h - 1 - yy, dy = y + xx;
                if (b->px[yy * b->w + xx] && dx >= 0 && dx < CW && dy >= 0 && dy < CH)
                    cv[dy][dx] = b->px[yy * b->w + xx];
            }
}

static void blit(const GaBitmap *b, int x, int y)  /* 0 is see-through */
{
    int xx, yy;
    if (b->px)
        for (yy = 0; yy < b->h; yy++)
            for (xx = 0; xx < b->w; xx++)
                if (b->px[yy * b->w + xx] && x + xx >= 0 && x + xx < CW && y + yy >= 0 && y + yy < CH)
                    cv[y + yy][x + xx] = b->px[yy * b->w + xx];
}

/* ------------------------------------------------------------------ the pages
 * Each page: the left column (Fullscreen, Preset, the pages, Mods, Show FPS), its middle and right
 * columns, the help line and the boxes under them.  A column is panels of groups: a title and
 * buttons (one for each value, the chosen one orange), a slider, a picker (arrows either side), a
 * preview picture, a button.  The values are options.c's (O_), screen.c's (S_), the game's own
 * (sound) or the pages */
enum { G_END, G_PANEL, G_BUTTONS, G_SLIDER, G_HEADER, G_PICK, G_PREVIEW, G_PRESS, G_BOX };
enum { SRC_OPT, SRC_SCR, SRC_GAME, SRC_PAGE, SRC_MODS, SRC_BOX };
enum { BX_KEYS, BX_CLOSE, BX_WEB, BX_GAME };
typedef struct {
    unsigned char kind, src, id, cols;  /* cols: buttons a row (0: all in one) */
    const char *title, *vals;           /* vals: the buttons' names, "Off|On" */
} Group;
#define ONOFF "Off|On"                  /* (owner: On always on the right) */
/* sounds (RESSOUND.RES), played on the game's thread (port_game_sound, src/game/shell/options.c):
 * a setting's button (the game's options screen's 0x791), CLOSE and the boxes (its Accept, 0x777),
 * a preset's card on the first start (SND_PLAY) */
#define SND_CLICK 0x791
#define SND_BOX 0x777
#define SND_PLAY 0x78C                  /* (the outfitting's Review Video Briefing, termscr.c) */
void port_game_sound(int id);
#define GAME_FX 1                       /* the game's options (src/game/shell/options.c) */
#define GAME_MUSIC 2
#define GAME_SPEECH 3
#define GAME_STEREO 11
#define GAME_DIFF 8                     /* 1..5 Neighborhood Watch .. World War */
#define GAME_SUIT 21                    /* check boxes: 1 Voice, 2 Text */
#define GAME_DROP 22
#define GAME_BRIEF 23                   /* 1 Video, 2 Text */
#define GAME_TIPS 24                    /* 1 On, 2 Off */

static const Group left_col[] = {
    { G_BUTTONS, SRC_OPT, O_FULLSCREEN, 0, "Fullscreen", ONOFF }, { G_PANEL },
    { G_BUTTONS, SRC_OPT, O_PRESET, 1, "Preset", "Freefall (recommended)|Vanilla+|Classic|Custom" }, { G_PANEL },
    { G_BUTTONS, SRC_PAGE, 0, 1, "Settings", "Gameplay|Display|Distance & Detail|Cockpit & Maps|Music & Movies" },
    { G_PANEL },
    { G_BUTTONS, SRC_MODS, 0, 0, NULL, "Mods" }, { G_PANEL },
    { G_BUTTONS, SRC_OPT, O_SHOWFPS, 0, "Show FPS", ONOFF }, { G_END } };
#define LEFT_STRETCH 2                  /* the panel that fills the column (Settings) */

typedef struct { Group mid[12], right[12]; } Page;
static const Page pages[PG_KEYS] = {
    [PG_GAMEPLAY] = {
        { { G_BUTTONS, SRC_OPT, O_CTRL, 0, "Controls style", "Classic|Advanced" },
          { G_BUTTONS, SRC_OPT, O_AIM, 0, "Auto-aim", "Off|5 px|20 px" },
          { G_BUTTONS, SRC_OPT, O_AIMSIGN, 0, "Auto-aim highlight", ONOFF },
          { G_SLIDER, SRC_OPT, O_SENS, 0, "Mouse sensitivity" },
          { G_BUTTONS, SRC_OPT, O_INV, 0, "Invert mouse", ONOFF },
          { G_BUTTONS, SRC_GAME, GAME_STEREO, 1, "Stereo sound", "Normal|Reversed" }, { G_END } },
        { { G_HEADER, 0, 0, 0, "Sound" },
          { G_SLIDER, SRC_GAME, GAME_FX, 0, "FX volume" },
          { G_SLIDER, SRC_GAME, GAME_MUSIC, 0, "Music volume" },
          { G_SLIDER, SRC_GAME, GAME_SPEECH, 0, "Speech volume" },
          { G_BUTTONS, SRC_GAME, GAME_DIFF, 1, "Difficulty level",
            "Neighborhood Watch|Reserve Duty|Standard Duty|Hazardous Duty|World War" },
          { G_BUTTONS, SRC_OPT, O_MSEL, 0, "Mission select", "Reached|All (cheat)" },
          { G_BUTTONS, SRC_GAME, GAME_TIPS, 0, "Tool tips", ONOFF }, { G_END } } },
    [PG_DISPLAY] = {
        { { G_BUTTONS, SRC_OPT, O_FPS, 0, "Frame rate", "25 fps|60 fps" },
          { G_BUTTONS, SRC_SCR, S_SHARP3D, 0, "3D resolution", "Classic|2x|4x|6x" },
          { G_BUTTONS, SRC_SCR, S_WIDE, 0, "Widescreen", ONOFF },
          { G_BUTTONS, SRC_SCR, S_SCALE, 0, "Scaling", "Fit|Sharp" },
          { G_BUTTONS, SRC_SCR, S_AA, 0, "Anti-aliasing", "Off|FXAA" },
          { G_BUTTONS, SRC_SCR, S_GPULAND, 0, "GPU rendering (experimental)", ONOFF }, { G_PANEL }, { G_PREVIEW }, { G_END } },
        { { G_SLIDER, SRC_SCR, S_BRIGHT, 0, "Brightness" },
          { G_SLIDER, SRC_SCR, S_GAMMA, 0, "Gamma" },
          { G_SLIDER, SRC_SCR, S_CONTRAST, 0, "Contrast" },
          { G_SLIDER, SRC_SCR, S_COLOUR, 0, "Colour" },
          { G_SLIDER, SRC_SCR, S_SCAN, 0, "Scanlines" },
          { G_BUTTONS, SRC_SCR, S_EFFECT, 2, "Effect", "Off|Smooth edges|CRT|CRT curved" },
          { G_END } } },
    [PG_DETAIL] = {
        { { G_BUTTONS, SRC_SCR, S_TERRAIN, 0, "Terrain distance", "Classic|Medium|High" },
          { G_BUTTONS, SRC_SCR, S_OBJECTS, 0, "Object distance", "Classic|Far|Farthest" },
          { G_BUTTONS, SRC_SCR, S_ENEMIES, 0, "Enemy distance", "Classic|Far" },
          { G_BUTTONS, SRC_SCR, S_FOG, 0, "Fog", ONOFF },
          { G_BUTTONS, SRC_SCR, S_SMOOTH, 0, "Terrain smoothing", ONOFF }, { G_PANEL }, { G_PREVIEW }, { G_END } },
        { { G_BUTTONS, SRC_SCR, S_TRUECOL, 0, "True colour", "Off|On|Sun glow" },
          { G_BUTTONS, SRC_SCR, S_WATER, 1, "Water reflection", "Low|Medium|High (experimental)" },
          { G_BUTTONS, SRC_SCR, S_RIPPLES, 0, "Water ripples", ONOFF },
          { G_BUTTONS, SRC_SCR, S_WATERFX, 0, "Water effects", ONOFF },
          { G_BUTTONS, SRC_SCR, S_TREESIZE, 0, "Tree size", "Classic|Realistic" },
          { G_BUTTONS, SRC_SCR, S_BURN, 0, "Tree explosion", "Classic|Realistic" },
          { G_BUTTONS, SRC_SCR, S_INFRARED, 0, "Infrared", "Classic|Sci-fi" }, { G_END } } },
    [PG_COCKPIT] = {
        { { G_BUTTONS, SRC_OPT, O_TLABEL, 0, "Target label", "Classic|Around the box" },
          { G_BUTTONS, SRC_OPT, O_TCAM, 0, "Auto target camera", ONOFF },
          { G_BUTTONS, SRC_OPT, O_MAPCONE, 0, "Head view cone", ONOFF },
          { G_BUTTONS, SRC_OPT, O_ENEMIES, 0, "Show enemies without MFD", ONOFF }, { G_PANEL }, { G_PREVIEW }, { G_END } },
        { { G_BUTTONS, SRC_SCR, S_MAP, 0, "Map smoothing", ONOFF },
          { G_BUTTONS, SRC_OPT, O_RADAR, 0, "Radar without MFD (experimental)", ONOFF }, { G_END } } },
    [PG_SOUND] = {
        { { G_BUTTONS, SRC_OPT, O_MUSIC, 1, "Music", "General MIDI|SoundFont (AWE32)|OPL3 FM|OPL2 FM|Off" },
          { G_PICK, SRC_OPT, O_SOUNDFONT, 0, "SoundFont for AWE32" },
          { G_PICK, SRC_OPT, O_PACK, 0, "Music pack" }, { G_END } },
        { { G_BUTTONS, SRC_SCR, S_VIDEOS, 0, "Smooth videos", "Off|30 fps" },
          { G_BUTTONS, SRC_SCR, S_MOVIES, 0, "Movie size", "Classic|Fill screen" },
          { G_BUTTONS, SRC_GAME, GAME_SUIT, 0, "Suit speech", "Voice|Text" },
          { G_BUTTONS, SRC_GAME, GAME_DROP, 0, "Dropship speech", "Voice|Text" },
          { G_BUTTONS, SRC_GAME, GAME_BRIEF, 0, "Briefings", "Video|Text" }, { G_END } } },
    [PG_MODS] = {
        { { G_PICK, SRC_OPT, O_MODSET, 0, "Mod set" },
          { G_PICK, SRC_SCR, S_CKPACK, 0, "Cockpit mod" },
          { G_PICK, SRC_SCR, S_ARMS, 0, "Arms mod" },
          { G_PICK, SRC_SCR, S_EFFECTS, 0, "Effects mod" },
          { G_PICK, SRC_SCR, S_OBJPACK, 0, "Objects mod" }, { G_END } },
        { { G_BUTTONS, SRC_SCR, S_HMOTION, 0, "Dynamic helmet", ONOFF }, { G_END } } },
};
static const Group boxes[4] = { { G_BOX, SRC_BOX, BX_KEYS }, { G_BOX, SRC_BOX, BX_CLOSE },
                                { G_BOX, SRC_BOX, BX_WEB }, { G_BOX, SRC_BOX, BX_GAME } };
static const char *page_help[PG_KEYS] = { "Controls, auto-aim, mouse, sound, difficulty", "Frame rate, 3D resolution, filters",
                                          "How far land and objects show, sky, water, trees",
                                          "Target label and camera, maps", "Music, music packs, movies, speech",
                                          "Mod zips from the Mods folder" };
#define WEBSITE "https://cyber1on.github.io/terranova-freefall-site/"

/* the preview pictures (built into tnfree, dist/Previews/<name>.jpg; see pic_get), one for each
 * value of the settings below (owner's stills of the game) */
typedef struct { unsigned char src, id; const char *names; } Stills;
static const Stills stills[] = {
    { SRC_SCR, S_SHARP3D, "res-classic|res-2x|res-4x|res-6x" }, { SRC_SCR, S_WIDE, "wide-off|wide-on" },
    { SRC_SCR, S_AA, "aa-off|aa-fxaa" }, { SRC_SCR, S_EFFECT, "effect-off|effect-smooth|effect-crt|effect-crt-curved" },
    { SRC_SCR, S_TERRAIN, "terrain-classic|terrain-medium|terrain-high" },
    { SRC_SCR, S_OBJECTS, "objects-classic|objects-far|objects-farthest" }, { SRC_SCR, S_FOG, "fog-off|fog-on" },
    { SRC_SCR, S_SMOOTH, "smooth-off|smooth-on" }, { SRC_SCR, S_TRUECOL, "truecolour-off|truecolour-on|truecolour-glow" },
    { SRC_SCR, S_WATER, "water-low|water-medium|water-high" }, { SRC_SCR, S_RIPPLES, "ripples-off|ripples-on" },
    { SRC_SCR, S_WATERFX, "waterfx-off|waterfx-on" }, { SRC_SCR, S_TREESIZE, "trees-classic|trees-realistic" },
    { SRC_SCR, S_INFRARED, "infrared-classic|infrared-scifi" }, { SRC_OPT, O_TLABEL, "label-classic|label-around" },
    { SRC_SCR, S_BURN, "burn-classic|burn-realistic" },
    { SRC_OPT, O_TCAM, "targcam-off|targcam-on" }, { SRC_OPT, O_MAPCONE, "cone-off|cone-on" },
    { SRC_SCR, S_MAP, "mapsmooth-off|mapsmooth-on" }, { SRC_OPT, O_RADAR, "radar-off|radar-on" },
    { SRC_OPT, O_ENEMIES, "enemies-off|enemies-on" },
};

/* the n-th of "a|b|c" into out; 0 if there is none */
static int nth(const char *list, int n, char *out, size_t sz)
{
    const char *e;
    if (!list || n < 0)
        return 0;
    for (; n > 0 && list; n--)
        list = (list = strchr(list, '|')) != NULL ? list + 1 : NULL;
    if (!list)
        return 0;
    e = strchr(list, '|');
    snprintf(out, sz, "%.*s", e ? (int)(e - list) : (int)strlen(list), list);
    return 1;
}

static int count(const char *list)
{
    int n = 1;
    if (!list)
        return 0;
    while ((list = strchr(list, '|')) != NULL)
        n++, list++;
    return n;
}

static const Stills *stills_of(const Group *g)
{
    size_t i;
    for (i = 0; i < sizeof stills / sizeof stills[0]; i++)
        if (stills[i].src == g->src && stills[i].id == g->id && g->kind == G_BUTTONS)
            return &stills[i];
    return NULL;
}

/* in a mission: its controls ran a moment ago, or it stands still for this screen (P134: after
 * a quarter of a second of standing still the mission counted as over, and GAME OPTIONS stayed
 * grey and did nothing) */
static int in_mission(void)
{
    return port_options_held || port_keys_in_mission();
}

/* ------------------------------------------------------------------ a group's value */
static int last_page = PG_GAMEPLAY;     /* the settings page shown (and shown again on opening) */

/* the game takes its O (opens its options screen) everywhere but in a yes / no box, while a saved
 * game's name is typed and on the Mission Selector (owner: in the base too) */
static int game_o_ok(void)
{
    return port_game_options_up || (!port_game_dialog_up && !port_game_typing && !port_missions_open);
}

static int g_usable(const Group *g)
{
    switch (g->src) {
    case SRC_OPT: return opt_usable(g->id);
    case SRC_SCR: return port_screen_usable(g->id);
    case SRC_GAME: return port_game_option_allowed(g->id);
    case SRC_BOX: return g->id != BX_GAME || game_o_ok();
    }
    return 1;
}

static int g_get(const Group *g)        /* the chosen button; -1 none */
{
    switch (g->src) {
    case SRC_OPT:
        switch (g->id) {
        case O_FULLSCREEN: return port_fullscreen != 0;
        case O_SHOWFPS: return port_show_fps != 0;
        case O_PRESET: { int p = preset_now(); return p < 0 ? 3 : 2 - p; }
        case O_FPS: return port_smooth_fps != 0;
        case O_CTRL: return port_mouselook != 0;
        case O_INV: return port_mouselook_invert != 0;
        case O_AIM: return port_aim_box == AIM_CLASSIC ? 2 : port_aim_box ? 1 : 0;
        case O_AIMSIGN: return port_aim_sign != 0;
        case O_MSEL: return port_ms_all != 0;
        case O_TCAM: return port_auto_targcam != 0;
        case O_TLABEL: return port_target_label != 0;
        case O_MAPCONE: return port_map_cone != 0;
        case O_ENEMIES: return port_map_enemies != 0;
        case O_RADAR: return port_map_radar != 0;
        case O_MUSIC: return music_index();
        }
        return 0;
    case SRC_SCR: return port_screen_get(g->id);
    case SRC_GAME:
        switch (g->id) {
        case GAME_STEREO: case GAME_DIFF: case GAME_BRIEF: return (int)port_game_option(g->id) - 1;
        case GAME_TIPS: return port_game_option(g->id) == 1;    /* (Off|On: the game's 2 is Off) */
        case GAME_SUIT: case GAME_DROP: return -1;              /* both can be lit (g_lit) */
        }
        return (int)port_game_option(g->id);
    case SRC_PAGE: return page < PG_MODS ? page : -1;
    case SRC_MODS: return page == PG_MODS ? 0 : -1;
    }
    return -1;
}

/* the game's speech options are two check boxes each (Voice, Text): either, both or none lit */
static int g_multi(const Group *g)
{
    return g->src == SRC_GAME && (g->id == GAME_SUIT || g->id == GAME_DROP);
}

static int g_lit(const Group *g, int v)
{
    return g_multi(g) ? ((int)port_game_option(g->id) >> v & 1) : v == g_get(g);
}

/* button v of g usable (Auto-aim's 20 px needs the Classic controls; Custom is no choice) */
static int g_val_usable(const Group *g, int v)
{
    if (g->src == SRC_OPT && g->id == O_AIM && v == 2)
        return !port_mouselook;
    if (g->src == SRC_OPT && g->id == O_PRESET && v == 3)
        return 0;
    return 1;
}

static void go_page(int pg);

static void g_set(const Group *g, int v)
{
    int k;
    if (!g_usable(g) || !g_val_usable(g, v) || v < 0)
        return;
    switch (g->src) {
    case SRC_OPT:
        if (g->id == O_PRESET)
            apply_preset(2 - v);
        else if (g->id == O_MUSIC && v < NMUSIC) {
            /* straight to it: stepping there opened each music between (P132: the SoundFont's
             * synth loaded on the way from General MIDI to OPL3, the window stood still) */
            snprintf(port_music_mode, sizeof port_music_mode, "%s", music_modes[v]);
            port_music_reopen();
        } else if (g->id == O_AIM)
            port_aim_box = v == 2 ? AIM_CLASSIC : v ? AIM_SMALL : 0;
        else
            for (k = 0; k < 8 && g_get(g) != v; k++)
                opt_change(g->id, 1);
        port_save_settings();
        break;
    case SRC_SCR:
        port_screen_set(g->id, v);
        port_save_settings();
        break;
    case SRC_GAME:
        switch (g->id) {
        case GAME_STEREO: case GAME_DIFF: case GAME_BRIEF: v++; break;
        case GAME_TIPS: v = v ? 1 : 2; break;
        case GAME_SUIT: case GAME_DROP: v = (int)port_game_option(g->id) ^ (1 << v); break;  /* a check box */
        }
        port_game_option_set(g->id, v);
        break;
    case SRC_PAGE:
        go_page(v);
        break;
    case SRC_MODS:
        go_page(PG_MODS);
        break;
    }
}

/* a slider: 30 ticks (the game's); its value's tick 0..29 and the value for a tick */
static void s_range(const Group *g, int *lo, int *hi, int *step)
{
    *lo = 0, *hi = 29, *step = 1;
    if (g->src == SRC_OPT)              /* Mouse sensitivity */
        *lo = 10, *hi = 500, *step = 10;
    else if (g->src == SRC_SCR)
        port_screen_range(g->id, lo, hi, step);
}

static int s_value(const Group *g)
{
    return g->src == SRC_OPT ? port_mouselook_sens : g->src == SRC_SCR ? port_screen_get(g->id) :
                                                     (int)port_game_option(g->id);
}

static int s_tick(const Group *g)
{
    int lo, hi, step;
    s_range(g, &lo, &hi, &step);
    return hi > lo ? ((s_value(g) - lo) * 29 * 2 + (hi - lo)) / ((hi - lo) * 2) : 0;
}

static void s_set(const Group *g, int v)
{
    int lo, hi, step;
    s_range(g, &lo, &hi, &step);
    v = v < lo ? lo : v > hi ? hi : lo + (v - lo + step / 2) / step * step;
    if (!g_usable(g))
        return;
    if (g->src == SRC_OPT) {
        port_mouselook_sens = v;
        port_save_settings();
    } else if (g->src == SRC_SCR) {
        port_screen_set(g->id, v);
        port_save_settings();
    } else
        port_game_option_set(g->id, v);
}

static void s_set_tick(const Group *g, int k)
{
    int lo, hi, step;
    s_range(g, &lo, &hi, &step);
    s_set(g, lo + (k * (hi - lo) * 2 + 29) / 58);
}

/* Left / Right (dir -1 / 1), the wheel: the next value */
/* the click of a setting's button (Stereo sound plays its own "left, right" test instead) */
static int g_click(const Group *g)
{
    return g->src == SRC_GAME && g->id == GAME_STEREO ? 0 : SND_CLICK;
}

static void g_step(const Group *g, int dir, int wrap)
{
    int lo, hi, step, n, v, k;
    if (g->kind == G_BUTTONS || g->kind == G_PICK)
        port_game_sound(g_click(g));
    switch (g->kind) {
    case G_SLIDER:
        s_range(g, &lo, &hi, &step);
        s_set(g, s_value(g) + dir * step);
        break;
    case G_PICK:
        if (g->src == SRC_OPT)
            opt_change(g->id, dir);
        else if (g_usable(g)) {
            port_screen_change(g->id, dir);
            port_save_settings();
        }
        break;
    case G_BUTTONS:
        n = count(g->vals);
        v = g_get(g);
        if (g_multi(g)) {               /* Voice, Voice and Text, Text, none */
            static const int next[4] = { 1, 3, 0, 2 }, prev[4] = { 2, 0, 3, 1 };
            v = (int)port_game_option(g->id) & 3;
            if (g_usable(g))
                port_game_option_set(g->id, dir > 0 ? next[v] : prev[v]);
            break;
        }
        for (k = 1; k < n; k++) {       /* the next usable one */
            int w = v < 0 ? (dir > 0 ? k - 1 : n - k) : v + dir * k;
            if (!wrap && (w < 0 || w >= n))
                break;
            w = (w % n + n) % n;
            if (g_val_usable(g, w)) {
                g_set(g, w);
                break;
            }
        }
        break;
    }
}

static const Group *hover_g;            /* under the mouse, and its button (-1) */
static int hover_v;

static const char *g_help(const Group *g)
{
    static char t[160];
    /* the button under the mouse: its own help (a slider, a list: -1, the value now) */
    int v = g == hover_g && g->kind == G_BUTTONS ? hover_v : -1;
    if (v >= 0 && v == g_get(g))
        v = -1;
    switch (g->src) {
    case SRC_OPT:
        if (g->id == O_PRESET && preset_now() >= 0 && starting())
            return "Starting...";
        return opt_help_v(g->id, v);
    case SRC_SCR:
        if (g->id == S_HMOTION && port_widescreen && port_ck_pack[0])
            return (v >= 0 ? v : port_helmet_motion) ? "The helmet sways with your motion" : "Off: the helmet is still";
        if (v >= 0)
            return port_screen_help_v(g->id, v);
        if (g->id == S_SHARP3D && port_sharp3d) {     /* with the size it is drawn at */
            char v[48], *p;
            port_screen_value(S_SHARP3D, v, sizeof v);
            p = strchr(v, '(');
            snprintf(t, sizeof t, "%s %s", port_screen_help(g->id), p ? p : "");
            return t;
        }
        return port_screen_help(g->id);
    case SRC_GAME:
        if (g->id == GAME_DIFF && !g_usable(g))
            return "Set it between missions, not in one";
        if (!g_usable(g))
            return "Not available with this sound card";
        if (v < 0 && !g_multi(g))
            v = g_get(g);
        switch (g->id) {
        case GAME_FX: return "How loud the sound effects are";
        case GAME_MUSIC: return "How loud the music is";
        case GAME_SPEECH: return "How loud the voices are";
        case GAME_STEREO: return v ? "Reversed: left and right swapped" : "Left and right as the game has them";
        case GAME_DIFF: {
            /* the game's own tables by level (enemy damage 60/100/140/175/200%, aim and fire
             * delay 140/100/70/50/30, the squad's health and armour 100/100/100/130/180%; owner's
             * wording) */
            static const char *const d[5] = {
                "Easiest: enemies do 60% damage, aim and fire slower",
                "Easy: enemies do normal damage at normal speed",
                "Game's default: enemies do 1.4x damage, aim better",
                "Hard: enemies do 1.75x damage; squad 1.3x tougher",
                "Hardest: enemies do 2x damage, fire fastest; squad 1.8x tougher" };
            return v >= 0 && v < 5 ? d[v] : "";
        }
        case GAME_TIPS: return v ? "Tips show over the game's buttons" : "No tips over the game's buttons";
        case GAME_BRIEF: return v ? "Mission briefings as text" : "Mission briefings as videos";
        case GAME_SUIT: case GAME_DROP:
            if (v == 1)
                return g->id == GAME_SUIT ? "The suit's messages as text; click: on/off" :
                                            "The dropship's words as text; click: on/off";
            if (v == 0)
                return g->id == GAME_SUIT ? "The suit's messages spoken; click: on/off" :
                                            "The dropship's words spoken; click: on/off";
            return g->id == GAME_SUIT ? "Your suit's messages: voice, text or both" :
                                        "The dropship pilot: voice, text or both";
        }
        return "";
    case SRC_PAGE:
        if (v >= 0 && v < PG_MODS)
            return page_help[v];
        return page < PG_MODS ? page_help[page] : "The settings, a page at a time";
    case SRC_MODS: return page_help[PG_MODS];
    case SRC_BOX:
        switch (g->id) {
        case BX_KEYS: return page == PG_KEYS ? "The controls style's own keys again" : "Rebind your keys";
        case BX_CLOSE: return page == PG_KEYS ? "Back to the settings; Esc too" : "Esc or O closes this screen too";
        case BX_WEB: return "Open the Freefall website in your browser";
        case BX_GAME: return port_game_options_up ? "Back to the game's own options screen" :
                             "The game's own options screen (Alt+O)";
        }
    }
    return "";
}

/* the title of a slider: "Brightness: 0" (the sound volumes without their number) */
static void g_title(const Group *g, char *t, size_t n)
{
    char v[48];
    snprintf(t, n, "%s", g->title ? g->title : "");
    if (g->kind == G_SLIDER && g->src != SRC_GAME) {
        if (g->src == SRC_OPT)
            opt_value(g->id, v, sizeof v);
        else
            port_screen_value(g->id, v, sizeof v);
        snprintf(t, n, "%s: %s", g->title, v);
    }
}

/* ------------------------------------------------------------------ drawing a page
 * Drawing also notes what is where (hits) and the groups that can be chosen */
enum { H_VAL, H_SLIDER, H_PICK, H_ACT };
typedef struct { short x0, y0, x1, y1; const Group *g; short v; unsigned char what; } Hit;
static Hit hits[200];
static int nhits;
static const Group *order[64];          /* the groups that can be chosen, in order */
static int norder, focus = -1;          /* focus: the chosen one (last clicked or under the mouse), its place */
static const Group *focus_g;
static int mx = -1, my = -1;            /* the mouse on the picture (-1 outside) */
static const Group *drag;               /* a slider dragged */
static SDL_Rect prev_box;               /* the preview's box (inside), w 0 none */
static SDL_Rect logo_box;
static char prev_name[48];              /* the picture it shows */

static void hit(int x0, int y0, int x1, int y1, const Group *g, int v, int what)
{
    if (nhits < (int)(sizeof hits / sizeof hits[0])) {
        Hit *h = &hits[nhits++];
        h->x0 = (short)x0, h->y0 = (short)y0, h->x1 = (short)x1, h->y1 = (short)y1;
        h->g = g, h->v = (short)v, h->what = (unsigned char)what;
    }
    if (mx >= x0 && mx < x1 && my >= y0 && my < y1) {
        hover_g = g;
        hover_v = what == H_VAL ? v : -1;
    }
}

static void add_order(const Group *g)
{
    if (norder < (int)(sizeof order / sizeof order[0]))
        order[norder++] = g;
}

/* the mouse over x0..x1, y0..y1: the button there shows its text orange (owner); with the
 * keys the chosen one is orange with its text in the panels' grey, as the game's arrows */
static int under(int x0, int y0, int x1, int y1)
{
    return mx >= x0 && mx < x1 && my >= y0 && my < y1;
}

static int title_bg = BLACK;            /* (draw_group: Difficulty level's red) */
static int title(int x, int y, const char *t, int dim)
{
    char u[96];
    int i;
    for (i = 0; t[i] && i < (int)sizeof u - 1; i++)
        u[i] = (char)SDL_toupper((unsigned char)t[i]);
    u[i] = 0;
    box(x, y, x + 184, y + TH, title_bg);
    ctext(x, x + 184, y + 2, u, dim ? DIM : WHITE);
    return y + TH + 3;
}

static int draw_buttons(int x, int y, const Group *g)
{
    int n = count(g->vals), cols = g->cols ? g->cols : n, rows = (n + cols - 1) / cols, w = (184 - (cols - 1) * 3) / cols;
    int i, sel = g_get(g), dis = !g_usable(g);
    char v[48];
    for (i = 0; i < n; i++) {
        int r = i / cols, c = i % cols, bx = x + c * (w + 3), bw = c < cols - 1 ? w : 184 - c * (w + 3), by = y + r * (BH + 3);
        int on = g->src == SRC_MODS ? BLUE_LIT : ORANGE, off = g->src == SRC_MODS ? BLUE : BROWN;
        int col = dis || !g_val_usable(g, i) ? DIM : WHITE;
        nth(g->vals, i, v, sizeof v);
        int hov = col == WHITE && under(bx, by, bx + bw + 1, by + BH + 1);
        if (g->src == SRC_OPT && g->id == O_PRESET && i == 3) {  /* Custom: lit only when it is so */
            col = sel == 3 ? WHITE : DIM;
            hov = 0;                    /* (not chosen by a click) */
        }
        /* under the mouse its text orange; the chosen one (lit) in the panels' grey under the mouse
         * (owner) */
        box(bx, by, bx + bw, by + BH, g_lit(g, i) ? on : off);
        ctext(bx, bx + bw, by + 3, v, g_lit(g, i) && hov && col == WHITE ? LIGHT : hov ? ORANGE : col);
        hit(bx, by, bx + bw + 1, by + BH + 1, g, i, H_VAL);
    }
    return y + rows * (BH + 3);
}

static int draw_slider(int x, int y, const Group *g)
{
    int i, k = s_tick(g), dis = !g_usable(g);
    box(x, y, x + 184, y + BH, BLACK);
    for (i = 0; i < 30; i++)
        blit(&bm_tick[i == k && !dis], x + 3 + i * 6, y + 3);
    hit(x, y, x + 185, y + BH + 1, g, 0, H_SLIDER);
    return y + BH + 3;
}

static int draw_pick(int x, int y, const Group *g)
{
    char v[64];
    int dis = !g_usable(g);
    if (g->src == SRC_OPT)
        opt_value(g->id, v, sizeof v);
    else
        port_screen_value(g->id, v, sizeof v);
    blit(&bm_arrow[0], x, y);
    blit(&bm_arrow[1], x + 184 - bm_arrow[1].w, y);
    box(x + 21, y, x + 184 - 21, y + BH, dis ? BROWN : ORANGE);
    ctext(x + 21, x + 184 - 21, y + 3, v, dis ? DIM : under(x, y, x + 185, y + BH + 1) ? LIGHT : WHITE);
    hit(x, y, x + 20, y + BH + 1, g, -1, H_PICK);
    hit(x + 21, y, x + 164, y + BH + 1, g, 1, H_PICK);
    hit(x + 164, y, x + 185, y + BH + 1, g, 1, H_PICK);
    return y + BH + 3;
}

static int group_h(const Group *g)
{
    int n;
    switch (g->kind) {
    case G_BUTTONS:
        n = count(g->vals);
        return (g->title ? TH + 3 : 0) + (n + (g->cols ? g->cols : n) - 1) / (g->cols ? g->cols : n) * (BH + 3);
    case G_PRESS: return BH + 3;
    case G_HEADER: return TH + 3;
    case G_PREVIEW: return 60;
    case G_SLIDER: case G_PICK: return TH + 3 + BH + 3;
    }
    return 0;
}

static int panel_h(const Group *g)      /* the panel's groups up to G_PANEL / G_END */
{
    int h = 2;
    for (; g->kind != G_END && g->kind != G_PANEL; g++)
        h += group_h(g) + GAP;
    return h + 3 - GAP;
}

static int draw_group(int x, int y, const Group *g)
{
    char t[96];
    int dis = !g_usable(g);
    switch (g->kind) {
    case G_HEADER:
        return title(x, y, g->title, 0);
    case G_PREVIEW:
        box(x, y, x + 184, Y1 - 4, BLACK);
        prev_box.x = x + 1, prev_box.y = y + 1, prev_box.w = 182, prev_box.h = Y1 - 4 - y - 1;
        return Y1;
    case G_PRESS:
        add_order(g);
        {
            int hov = under(x, y, x + 185, y + BH + 1);
            box(x, y, x + 184, y + BH, BROWN);
            ctext(x, x + 184, y + 3, g->title, hov ? ORANGE : WHITE);
        }
        hit(x, y, x + 185, y + BH + 1, g, 0, H_ACT);
        return y + BH + 3;
    }
    add_order(g);
    g_title(g, t, sizeof t);
    /* Difficulty level: red under its title, which stays white while a mission greys the
     * buttons (owner) */
    title_bg = g->src == SRC_GAME && g->id == GAME_DIFF && red_c ? red_c : BLACK;
    if (g->title)
        y = title(x, y, t, dis && title_bg == BLACK);
    title_bg = BLACK;
    switch (g->kind) {
    case G_BUTTONS: return draw_buttons(x, y, g);
    case G_SLIDER: return draw_slider(x, y, g);
    case G_PICK: return draw_pick(x, y, g);
    }
    return y;
}

/* a column of panels from Y0 to Y1: the panel stretch (or the last) fills what the others leave */
static void column(int x0, const Group *gs, int stretch)
{
    const Group *p[8], *g = gs;
    int np = 0, k, y = Y0;
    p[np++] = gs;
    for (; g->kind != G_END; g++)
        if (g->kind == G_PANEL && np < 8)
            p[np++] = g + 1;
    for (k = 0; k < np; k++) {
        int bot, j, yy;
        if (stretch < 0)
            bot = k == np - 1 ? Y1 : y + panel_h(p[k]);
        else if (k == stretch) {
            bot = Y1;
            for (j = k + 1; j < np; j++)
                bot -= panel_h(p[j]) + 6;
        } else
            bot = y + panel_h(p[k]);
        panel(x0, y, x0 + 209, bot);
        yy = y + 3;
        for (g = p[k]; g->kind != G_END && g->kind != G_PANEL; g++)
            yy = draw_group(x0 + 13, yy, g) + GAP;
        y = bot + 6;
    }
}

static void header(void)
{
    panel(1, 4, 639, 34);
    box(14, 6, 626, 31, BLACK);
    logo_box.x = 14, logo_box.y = 9, logo_box.w = 612, logo_box.h = 19;
}

static void help_line(int x1, const char *h)
{
    panel(110, 412, x1, 436);
    box(123, 415, x1 - 13, 431, BLACK);
    ctext(123, x1 - 13, 417, h, WHITE);
}

static void version_box(void)
{
    char v[32];
    panel(4, 412, 104, 465);
    snprintf(v, sizeof v, "v%s", FREEFALL_VERSION);
    ctext(4, 104, 423, "FREEFALL", WHITE);
    ctext(4, 104, 435, v, WHITE);
    ctext(4, 104, 447, "(website)", under(4, 412, 105, 466) ? ORANGE : DIM);
    hit(4, 412, 105, 466, &boxes[BX_WEB], 0, H_ACT);
}

static void bottom(const char *help, const char *b1, const char *b2)
{
    static const int bx[2] = { 110, 324 };
    const char *t[2] = { b1, b2 };
    int i, game = g_usable(&boxes[BX_GAME]);
    help_line(534, help);
    for (i = 0; i < 2; i++) {
        const Group *g = &boxes[i ? BX_CLOSE : BX_KEYS];
        add_order(g);
        int hov = under(bx[i], 441, bx[i] + 211, 466);
        panel(bx[i], 441, bx[i] + 210, 465);
        box(bx[i] + 13, 444, bx[i] + 197, 460, BLACK);
        ctext(bx[i] + 13, bx[i] + 197, 446, t[i], hov ? ORANGE : WHITE);
        hit(bx[i], 441, bx[i] + 211, 466, g, 0, H_ACT);
    }
    version_box();
    panel(536, 412, 636, 465);
    {
        int c = !game ? DIM : under(536, 412, 637, 466) ? ORANGE : WHITE;
        ctext(536, 636, 423, "GAME", c);
        ctext(536, 636, 435, "OPTIONS", c);
    }
    ctext(536, 636, 447, "(Alt+O)", DIM);
    hit(536, 412, 637, 466, &boxes[BX_GAME], 0, H_ACT);
}

/* the preview: for the chosen group (or the one under the mouse) with pictures, the picture of
 * the button under the mouse, else of its value; kept while a group without pictures is chosen */
static void preview_pick(void)
{
    const Group *g = hover_g && stills_of(hover_g) ? hover_g : focus_g && stills_of(focus_g) ? focus_g : NULL;
    const Stills *s;
    int v;
    if (!g) {
        /* a setting of the page's own without pictures (Enemy distance, Scaling...): no picture,
         * not the last one (owner) */
        const Group *o = hover_g ? hover_g : focus_g, *c;
        if (o && page < PG_KEYS)
            for (c = pages[page].mid; c; c = c == pages[page].mid ? pages[page].right : NULL) {
                const Group *k;
                for (k = c; k->kind != G_END; k++)
                    if (k == o)
                        prev_name[0] = 0;
            }
        return;
    }
    s = stills_of(g);
    v = g == hover_g && hover_v >= 0 ? hover_v : g_get(g);
    nth(s->names, v, prev_name, sizeof prev_name);
}

static void preview_first(void)         /* a page's first group with pictures */
{
    const Group *g;
    prev_name[0] = 0;
    if (page >= PG_KEYS)
        return;
    for (g = pages[page].mid; g->kind != G_END; g++)
        if (stills_of(g)) {
            nth(stills_of(g)->names, g_get(g), prev_name, sizeof prev_name);
            return;
        }
    for (g = pages[page].right; g->kind != G_END; g++)
        if (stills_of(g)) {
            nth(stills_of(g)->names, g_get(g), prev_name, sizeof prev_name);
            return;
        }
}

static void go_page(int pg)
{
    page = pg;
    if (pg < PG_KEYS)
        last_page = pg;
    if (pg == PG_KEYS) {
        ksel = kcol = ktop = capturing = 0;
        kstatus[0] = 0;
    }
    preview_first();
}

static void draw_page(void)
{
    const Group *h;
    header();
    column(colx[0], left_col, LEFT_STRETCH);
    column(colx[1], pages[page].mid, -1);
    column(colx[2], pages[page].right, -1);
    h = hover_g ? hover_g : focus_g;
    bottom(h ? g_help(h) : page_help[page], "KEY BINDINGS", "CLOSE");
}

/* ------------------------------------------------------------------ the Keys page */
#define KV 16                           /* lines shown at once */
static int knum(void) { return port_keys_count(); }

static void kbind(int k)
{
    int a = ksel, got;
    capturing = 0;
    got = port_keys_set(a, kcol, k);
    if (got >= 0 && got != a)
        snprintf(kstatus, sizeof kstatus, "Swapped with %s", port_keys_label(got));
    else
        kstatus[0] = 0;
    port_save_settings();
}

static void kactivate(void)
{
    if (ksel < knum() && port_keys_slot_used(ksel, kcol)) {
        capturing = 1;
        kstatus[0] = 0;
    }
}

static void kscroll(int d)              /* the list d lines on */
{
    ktop += d;
    if (ktop > knum() - KV)
        ktop = knum() - KV;
    if (ktop < 0)
        ktop = 0;
}

static void kdefaults(void)
{
    port_keys_reset();
    port_save_settings();
    snprintf(kstatus, sizeof kstatus, "Default keys for %s controls", port_mouselook ? "Advanced" : "Classic");
}

static void draw_keys(void)
{
    int i, c, n = knum();
    char t[64];
    const char *h;
    header();
    panel(1, 40, 639, 64);
    box(14, 43, 626, 59, BLACK);
    ctext(14, 626, 45, port_mouselook ? "KEY BINDINGS: ADVANCED CONTROLS" : "KEY BINDINGS: CLASSIC CONTROLS", WHITE);
    panel(1, 70, 639, Y1);
    if (ktop > n - KV)
        ktop = n - KV > 0 ? n - KV : 0;
    for (i = 0; i < KV && ktop + i < n; i++) {
        int a = ktop + i, y = 74 + i * (BH + 3);
        box(14, y, 318, y + BH, BLACK);
        text_at(20, y + 3, port_keys_label(a), a == ksel ? ORANGE : WHITE);
        for (c = 0; c < 2; c++) {
            int x0 = c ? 469 : 326, x1 = c ? 603 : 461, on = a == ksel && c == kcol;
            if (!port_keys_slot_used(a, c))
                continue;
            snprintf(t, sizeof t, "%s", capturing && on ? "..." : port_keys_name(port_keys_get(a, c)));
            {
                int hov = !capturing && under(x0, y, x1 + 1, y + BH + 1);
                box(x0, y, x1, y + BH, on ? ORANGE : BROWN);
                ctext(x0, x1, y + 3, t, on ? LIGHT : hov ? ORANGE : WHITE);
            }
            hit(x0, y, x1 + 1, y + BH + 1, NULL, a * 2 + c, H_VAL);
        }
        hit(14, y, 319, y + BH + 1, NULL, a * 2, H_VAL);
    }
    if (n > KV) {                       /* where the list is: a bar at the right, between the game's
                                         * arrows turned up and down (owner) */
        int aw = bm_arrow[0].h, ah = bm_arrow[0].w, ax = 626 - aw, bot = 74 + KV * (BH + 3) - 3;
        int top = 74 + ah + 3, len = bot - ah - 3 - top, y0 = top + len * ktop / n, y1 = top + len * (ktop + KV) / n;
        blit_turned(&bm_arrow[0], ax, 74);
        blit_turned(&bm_arrow[1], ax, bot - ah);
        hit(ax, 74, ax + aw + 1, 74 + ah + 1, NULL, -1, H_PICK);
        hit(ax, bot - ah, ax + aw + 1, bot + 1, NULL, 1, H_PICK);
        fill(ax + aw / 2 - 2, top, ax + aw / 2 + 2, top + len, BLACK);
        fill(ax + aw / 2 - 2, y0, ax + aw / 2 + 2, y1, ORANGE);
    }
    h = capturing ? "Press a key (Esc: cancel, Backspace: clear)" : kstatus[0] ? kstatus :
        hover_g ? g_help(hover_g) : "Click a key to change it";
    bottom(h, "DEFAULT KEYS", "BACK");
}

/* ------------------------------------------------------------------ the first start's question
 * (owner's mockup): the three presets side by side, each with a picture of its look, what
 * it changes and its PLAY button */
static int wsel;                        /* 0 Freefall, 1 Vanilla+, 2 Classic */
static SDL_Rect wprev[3];
static const char *const wnames[3] = { "Freefall (recommended)", "Vanilla+", "Classic" };
static const char *const wstill[3] = { "preset-freefall", "preset-vanilla", "preset-classic" };
static const char *const wdesc[3][9] = {
    { "The best way to play", "Terra Nova: SFC", "", "Widescreen, 3D at 4x", "Fog and true colour",
      "Far objects and enemies", "Realistic trees", "Advanced controls", "60 fps" },
    { "The original look,", "made smoother", "", "Classic 3D at 2x, 4:3", "Fog and far land", "Water reflections",
      "Smooth videos", "Advanced controls", "60 fps" },
    { "The original 1996 game,", "nothing changed", "", "Classic 3D, 4:3", "Classic far land", "Classic water",
      "Classic videos", "Classic controls", "25 fps" } };

static void draw_welcome(void)
{
    int k, i;
    header();
    panel(1, 40, 639, 64);
    box(14, 43, 626, 59, BLACK);
    ctext(14, 626, 45, "HOW DO YOU WANT TO PLAY?", WHITE);
    for (k = 0; k < 3; k++) {
        int x0 = colx[k], x = x0 + 13, y, bot, ty;
        panel(x0, 70, x0 + 209, Y1);
        box(x, 73, x + 184, 73 + BH, k == wsel ? ORANGE : BROWN);
        ctext(x, x + 184, 76, wnames[k], under(x0, 70, x0 + 210, Y1) ? LIGHT : WHITE);
        hit(x, 73, x + 185, 74 + BH, NULL, k, H_VAL);
        y = 73 + BH + 3 + GAP;
        box(x, y, x + 184, y + 104, BLACK);
        wprev[k].x = x + 1, wprev[k].y = y + 1, wprev[k].w = 182, wprev[k].h = 102;
        hit(x, y, x + 185, y + 105, NULL, k, H_VAL);
        y += 104 + 3 + GAP;
        bot = 380 - 3 - GAP;
        box(x, y, x + 184, bot, BLACK);
        ty = y + (bot - y - 9 * 14) / 2 + 1;
        for (i = 0; i < 9; i++)
            ctext(x, x + 184, ty + i * 14, wdesc[k][i], i < 2 ? ORANGE : WHITE);
        hit(x, y, x + 185, bot + 1, NULL, k, H_VAL);
        box(x, 380, x + 184, 380 + BH, k == wsel ? ORANGE : BROWN);
        ctext(x, x + 184, 383, "PLAY", under(x0, 70, x0 + 210, Y1) ? LIGHT : WHITE);
        hit(x, 380, x + 185, 381 + BH, NULL, k, H_ACT);
    }
    panel(110, 412, 636, 436);
    box(123, 415, 623, 431, BLACK);
    ctext(123, 623, 417, "In a mission:  H = help and controls   O = options and presets", WHITE);
    panel(110, 441, 636, 465);
    box(123, 444, 623, 460, BLACK);
    ctext(123, 623, 446, "Left/Right: choose     Enter: play", WHITE);
    version_box();
}

static void welcome_play(int k)
{
    static const int choice[3] = { 2, 1, 0 };     /* Freefall, Vanilla+, Classic */
    apply_preset(choice[k]);
    port_save_settings();
    port_options_open = 0;
    go_page(PG_GAMEPLAY);
}

/* ------------------------------------------------------------------ input */
static SDL_Renderer *last_r;            /* the renderer and the picture's place at the last drawing */
static SDL_FRect last_rect;

static void to_canvas(float wx, float wy, int *x, int *y)
{
    float k = last_rect.w / CW;
    if (last_r)
        SDL_RenderCoordinatesFromWindow(last_r, wx, wy, &wx, &wy);
    if (k <= 0 || wx < last_rect.x || wy < last_rect.y || wx >= last_rect.x + last_rect.w ||
        wy >= last_rect.y + last_rect.h) {
        *x = *y = -1;
        return;
    }
    *x = (int)((wx - last_rect.x) / k);
    *y = (int)((wy - last_rect.y) / k);
}

static const Hit *hit_at(int x, int y)
{
    int i;
    for (i = 0; i < nhits; i++)
        if (x >= hits[i].x0 && x < hits[i].x1 && y >= hits[i].y0 && y < hits[i].y1)
            return &hits[i];
    return NULL;
}

static void set_focus(const Group *g)
{
    int i;
    focus_g = g;
    focus = -1;
    for (i = 0; i < norder; i++)
        if (order[i] == g)
            focus = i;
}

static void open_web(void)
{
    if (!SDL_OpenURL(WEBSITE))
        port_log("Options: could not open %s: %s\n", WEBSITE, SDL_GetError());
}

void port_key_tap(SDL_Scancode s);      /* platform.c */

static void box_act(int id)
{
    switch (id) {
    case BX_KEYS:
        if (page == PG_KEYS)
            kdefaults();
        else
            go_page(PG_KEYS);
        break;
    case BX_CLOSE:
        if (page == PG_KEYS)
            go_page(last_page);
        else
            port_options_open = 0;
        break;
    case BX_WEB:
        open_web();
        break;
    case BX_GAME:                       /* the game's own options screen: its O key */
        if (port_game_options_up)       /* (P136: it is under this screen already; O there is its
                                         * Accept and would close it) */
            port_options_open = 0;
        else if (game_o_ok()) {
            port_options_open = 0;
            port_key_tap(SDL_SCANCODE_O);
        }
        break;
    }
}

/* a click (button 1 left, 3 right) on the picture's x, y */
static void click(int x, int y, int button)
{
    const Hit *h = hit_at(x, y);
    if (!h)
        return;
    if (page == PG_WELCOME) {
        if (h->g)
            box_act(h->g->id);          /* the website */
        else {                          /* anywhere on a preset's card plays it (owner) */
            port_game_sound(SND_PLAY);
            welcome_play(h->v);
        }
        return;
    }
    if (page == PG_KEYS && !h->g && h->what == H_PICK) {   /* the list's arrows */
        port_game_sound(SND_CLICK);
        kscroll(h->v);
        return;
    }
    if (page == PG_KEYS && !h->g) {
        ksel = h->v / 2;
        if (port_keys_slot_used(ksel, h->v & 1))
            kcol = h->v & 1;
        if (x >= 326)
            kactivate();
        return;
    }
    if (!h->g)
        return;
    set_focus(h->g);
    if (h->what == H_VAL)               /* the game's options screen's click (g_step: the lists') */
        port_game_sound(g_click(h->g));
    else if (h->what == H_ACT)
        port_game_sound(SND_BOX);
    switch (h->what) {
    case H_VAL:
        g_set(h->g, h->v);
        break;
    case H_SLIDER:
        drag = h->g;
        s_set_tick(h->g, (x - h->x0 - 3) / 6);
        break;
    case H_PICK:
        g_step(h->g, button == 3 ? -h->v : h->v, 1);
        break;
    case H_ACT:
        if (h->g->kind == G_BOX)
            box_act(h->g->id);
        else if (h->g->kind == G_PRESS) {
            port_screen_change(h->g->id, 1);
            port_save_settings();
        }
        break;
    }
}

static int capture_event(const SDL_Event *e)   /* the Keys page waits for a key */
{
    if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {
        if (e->key.scancode == SDL_SCANCODE_ESCAPE)
            capturing = 0;
        else if (e->key.scancode == SDL_SCANCODE_BACKSPACE || e->key.scancode == SDL_SCANCODE_DELETE)
            kbind(SDL_SCANCODE_UNKNOWN);
        else
            kbind(e->key.scancode);
    } else if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        if (e->button.button == SDL_BUTTON_MIDDLE)
            kbind(PORT_KEY_MOUSE_MIDDLE);
        else if (e->button.button == SDL_BUTTON_X1)
            kbind(PORT_KEY_MOUSE_4);
        else if (e->button.button == SDL_BUTTON_X2)
            kbind(PORT_KEY_MOUSE_5);
        else
            capturing = 0;              /* left and right stay the game's */
    } else if (e->type == SDL_EVENT_MOUSE_WHEEL && e->wheel.y != 0) {
        kbind((e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e->wheel.y : e->wheel.y) > 0 ?
              PORT_KEY_WHEEL_UP : PORT_KEY_WHEEL_DOWN);
    }
    return 1;
}

static int screen_event(const SDL_Event *e)
{
    int x, y;
    if (page == PG_KEYS && capturing)
        return capture_event(e);
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        if (page == PG_WELCOME) {
            switch (e->key.scancode) {
            case SDL_SCANCODE_LEFT: case SDL_SCANCODE_KP_4: case SDL_SCANCODE_UP: wsel = (wsel + 2) % 3; break;
            case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_KP_6: case SDL_SCANCODE_DOWN: case SDL_SCANCODE_TAB:
                wsel = (wsel + 1) % 3;
                break;
            case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE: welcome_play(wsel); break;
            default: break;
            }
            return 1;
        }
        /* no keyboard control on the pages: the mouse only, Esc closes (owner) */
        if (e->key.scancode == SDL_SCANCODE_ESCAPE && !e->key.repeat) {
            if (page == PG_KEYS)
                go_page(last_page);
            else
                port_options_open = 0;
        }
        return 1;
    case SDL_EVENT_MOUSE_MOTION:
        to_canvas(e->motion.x, e->motion.y, &x, &y);
        if (drag && (e->motion.state & SDL_BUTTON_LMASK)) {
            const Hit *h;
            int i;
            for (i = 0, h = NULL; i < nhits; i++)
                if (hits[i].g == drag && hits[i].what == H_SLIDER)
                    h = &hits[i];
            if (h && x >= 0)
                s_set_tick(drag, SDL_clamp((x - h->x0 - 3) / 6, 0, 29));
        } else {
            const Hit *h;
            drag = NULL;
            if ((h = hit_at(x, y)) != NULL && h->g && h->g->kind != G_BOX) {
                set_focus(h->g);
            }
            if (h && page == PG_WELCOME && !h->g)
                wsel = h->v;
        }
        return 1;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        to_canvas(e->button.x, e->button.y, &x, &y);
        if (e->button.button == SDL_BUTTON_LEFT || e->button.button == SDL_BUTTON_RIGHT)
            click(x, y, e->button.button == SDL_BUTTON_RIGHT ? 3 : 1);
        return 1;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        drag = NULL;
        return 1;
    case SDL_EVENT_MOUSE_WHEEL:
        if (e->wheel.y != 0) {
            const Hit *h;
            int d = (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e->wheel.y : e->wheel.y) > 0 ? 1 : -1;
            to_canvas(e->wheel.mouse_x, e->wheel.mouse_y, &x, &y);
            if (page == PG_KEYS)
                kscroll(-d * 3);
            else if ((h = hit_at(x, y)) != NULL && h->g && h->g->kind != G_BOX && h->g->src != SRC_PAGE &&
                       h->g->src != SRC_MODS)
                g_step(h->g, d, 0);
        }
        return 1;
    case SDL_EVENT_KEY_UP:
        return 1;
    }
    return 0;
}

/* Esc opens the options in a mission when the game has no use for it there: not with the squad's
 * orders menu up (Esc steps back in it), the game's yes / no box (Esc answers No) or its options
 * screen, nor with Ctrl (Ctrl+Esc asks to end the mission) or Alt.  In the menus Esc stays the
 * game's (it skips movies, goes back, closes windows) */
static int esc_opens(const SDL_Event *e)
{
    return e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_ESCAPE && !e->key.repeat &&
           !(e->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) && !port_bench &&
           port_keys_in_mission() && !hud_38FC80 && !port_game_dialog_up && !port_game_options_up;
}

static void open_screen(void)
{
    mods.at = 0;                        /* the Mods folder as it is now */
    port_options_open = 1;
    capturing = 0;
    drag = NULL;
    focus_g = &left_col[4];             /* Settings */
    focus = -1;
    go_page(last_page);
}

/* key, mouse: 1 when the event was the options screen's (the game must not see it) */
int port_options_event(const SDL_Event *e)
{
    static int o_held;                  /* an O this screen took: its release is this screen's too */
    if (page != PG_WELCOME && e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_O) {
        int mod = e->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI);
        if (mod == SDL_KMOD_LALT || mod == SDL_KMOD_RALT || mod == SDL_KMOD_ALT) {
            /* Alt+O: the game's own options screen (its O key), wherever the game takes it; on that screen O
             * is its Accept, so Alt+O closes it there */
            if (!e->key.repeat && game_o_ok()) {
                port_options_open = 0;
                port_key_tap(SDL_SCANCODE_O);
            }
            o_held = 1;
            return 1;
        }
        /* O: this screen, but not while a key is being bound, the game's own options screen is
         * up (its Accept), a yes / no box is up or a saved game's name is typed */
        if (!mod && !(port_options_open && capturing) && !port_game_options_up && !port_game_dialog_up &&
            !port_game_typing) {
            if (!e->key.repeat) {
                if (port_options_open)
                    port_options_open = 0;
                else
                    open_screen();
            }
            o_held = 1;
            return 1;
        }
    }
    if (!port_options_open && esc_opens(e)) {
        open_screen();
        esc_held = 1;                   /* its release is the options screen's too */
        return 1;
    }
    if (esc_held && e->type == SDL_EVENT_KEY_UP && e->key.scancode == SDL_SCANCODE_ESCAPE) {
        esc_held = 0;
        return 1;
    }
    if (o_held && e->type == SDL_EVENT_KEY_UP && e->key.scancode == SDL_SCANCODE_O) {
        o_held = 0;
        return 1;
    }
    if (!port_options_open) {
        /* the Freefall button on the game's options screen, or on its main menu */
        const SDL_FRect *b = port_game_options_up && ffbutton.w > 0 ? &ffbutton :
                             port_menu_ffbutton && ffmenu.w > 0 && !port_game_dialog_up && !port_missions_open &&
                             !port_ms_fading ?
                             &ffmenu : NULL;
        if (b && (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP) &&
            e->button.button == SDL_BUTTON_LEFT && e->button.x >= b->x && e->button.x < b->x + b->w &&
            e->button.y >= b->y && e->button.y < b->y + b->h) {
            if (e->type == SDL_EVENT_MOUSE_BUTTON_UP)
                open_screen();
            return 1;
        }
        return 0;
    }
    return screen_event(e);
}


static void text(SDL_Renderer *r, float x, float y, float s, const char *t)
{
    SDL_SetRenderScale(r, s, s);
    SDL_RenderDebugText(r, x / s, y / s, t);
    SDL_SetRenderScale(r, 1, 1);
}

/* the game's mouse pointer (src/tnui/tnmouse.c): TnCursor = a grs_bitmap (bits +0, type +4,
 * flags +6, w +8, h +10, row +12) and the hotspot (+0x10); drawn at tnm_draw_pos */
extern unsigned char tnm_cursor[0x14], tnm_cursor_drawn;
extern short tnm_draw_pos[2];

/* a pointer's pixels, flat: -1 transparent */
typedef struct {
    int w, h, hx, hy;
    short px[64 * 64];
} Pointer;

/* the game's pointer (TnCursor: a grs_bitmap and the hotspot) to flat pixels; 0 when it is none or
 * a kind of bitmap not read here (flat 8-bit and RSD8, run-length, are the game's) */
static int pointer_read(const unsigned char *cur, Pointer *o)
{
    const unsigned char *bits = *(unsigned char *const *)cur;
    int type = cur[4], w = *(const short *)(cur + 8), h = *(const short *)(cur + 10);
    int row = *(const unsigned short *)(cur + 12), i, j, n = 0;

    if (!bits || (type != 2 && type != 4) || w <= 0 || h <= 0 || w > 64 || h > 64)
        return 0;
    o->w = w;
    o->h = h;
    o->hx = *(const short *)(cur + 0x10);
    o->hy = *(const short *)(cur + 0x12);
    for (i = 0; i < w * h; i++)
        o->px[i] = -1;
    if (type == 2) {
        for (j = 0; j < h; j++)
            for (i = 0; i < w; i++)
                if (bits[j * row + i])
                    o->px[j * w + i] = bits[j * row + i];
        return 1;
    }
    {                                   /* RSD8 codes: src/2d/fl8rsd8.c */
        const unsigned char *p = bits;
        while (n < w * h) {
            int b = *p++, cnt, lit = 0, run = -1;
            if (b == 0) {
                cnt = *p++;
                run = *p++;
            } else if (b < 0x80) {
                cnt = b;
                lit = 1;
            } else if (b > 0x80) {
                cnt = b & 0x7F;
            } else {
                int v = p[0] | p[1] << 8;
                p += 2;
                if (v == 0)
                    break;
                if (v < 0x8000)
                    cnt = v;
                else if (v < 0xC000) {
                    cnt = v & 0x7FFF;
                    lit = 1;
                } else {
                    cnt = v & 0x3FFF;
                    run = *p++;
                }
            }
            for (; cnt > 0; cnt--, n++) {
                int c = lit ? *p++ : run;
                if (n < w * h && c >= 0)
                    o->px[n] = (short)c;
            }
        }
    }
    return 1;
}

/* the game's pointers, kept by the game's thread as it sets them (src/tnui/tnmouse.c
 * MouseSetCursor): the one it shows now (its ref cur_ref), its arrow (the menus' arrow, or in a
 * mission the HUD's thin cross; not the aim it shows over the view), and the menus' arrow (in a
 * mission the options screen shows that one, in the menus' colours, menu_dac: the palette is the
 * mission's then) */
typedef struct { Pointer p[2]; volatile int pub; } Kept;
static Kept cur_kept = { .pub = -1 }, arrow_kept = { .pub = -1 }, menu_kept = { .pub = -1 };
static volatile long cur_ref;
static unsigned char menu_dac[768];
static int menu_dac_ok;

static void keep(Kept *k, const void *c)
{
    int n = k->pub == 0 ? 1 : 0;
    if (pointer_read((const unsigned char *)c, &k->p[n]))
        k->pub = n;
}

void port_keep_cursor(long ref, const void *c)
{
    cur_ref = ref;
    keep(&cur_kept, c);
    if (ref == 0x16B0000 || ref == 0x16B0001 || ref == 0x1F90016)
        keep(&arrow_kept, c);
    if (ref == 0x16B0000)
        keep(&menu_kept, c);
}

/* the colours of the menus' arrow (menu_kept): used, the palette entries it draws with; its
 * brightest colour in dac (0..63) */
static int arrow_colours(const unsigned char *dac, unsigned char *used)
{
    int ix = menu_kept.pub, i, c, best = 0;
    const Pointer *pt;
    SDL_memset(used, 0, 256);
    if (ix < 0)
        return 0;
    pt = &menu_kept.p[ix];
    for (i = 0; i < pt->w * pt->h; i++)
        if ((c = pt->px[i]) >= 0 && c < 256) {
            int m = dac[c * 3] > dac[c * 3 + 1] ? dac[c * 3] : dac[c * 3 + 1];
            if (dac[c * 3 + 2] > m)
                m = dac[c * 3 + 2];
            if (m > best)
                best = m;
            used[c] = 1;
        }
    return best;
}

/* each display: the menus' colours for their arrow, the game's palette while a 640 x 480 menu
 * shows that arrow and the arrow's colours have stayed the same for 4 displays (not in a fade),
 * bright (the owner's test: the main menu cycles other colours of its palette all the time, so
 * the palette as a whole was still only on the black screen of a fade, where it stays while a
 * mission loads, and the arrow was kept black) */
static void menu_colours(void)
{
    static unsigned char last[768];
    static int still;
    unsigned char used[256];
    int i, light = arrow_colours(port_video.dac, used), same = 1;
    for (i = 0; i < 256 && same; i++)
        if (used[i] && SDL_memcmp(last + i * 3, port_video.dac + i * 3, 3))
            same = 0;
    SDL_memcpy(last, port_video.dac, sizeof last);
    if (!same) {
        still = 0;
        return;
    }
    if (++still >= 4 && cur_ref == 0x16B0000 && port_video.w == 640 && !port_keys_in_mission() && light >= 32) {
        SDL_memcpy(menu_dac, last, sizeof menu_dac);
        menu_dac_ok = 1;
    }
}

/* draws a pointer with its hotspot at x, y, kx x ky window pixels a game pixel, in the colours of
 * dac (the game's palette, 6 bits) */
static void draw_pointer(SDL_Renderer *r, const Pointer *pt, float x, float y, float kx, float ky,
                         const unsigned char *dac, const SDL_FRect *clip)
{
    float x0 = x - pt->hx * kx, y0 = y - pt->hy * ky;
    int i, j;

    if (clip && (x0 > clip->x + clip->w || y0 > clip->y + clip->h || x0 + pt->w * kx < clip->x ||
                 y0 + pt->h * ky < clip->y))
        return;
    for (j = 0; j < pt->h; j++)
        for (i = 0; i < pt->w; i++) {
            int c = pt->px[j * pt->w + i];
            float l = SDL_floorf(x0 + i * kx), t = SDL_floorf(y0 + j * ky);
            SDL_FRect px1 = { l, t, SDL_floorf(x0 + (i + 1) * kx) - l, SDL_floorf(y0 + (j + 1) * ky) - t };
            if (c < 0)
                continue;               /* transparent */
            SDL_SetRenderDrawColor(r, dac[c * 3] << 2 | dac[c * 3] >> 4, dac[c * 3 + 1] << 2 | dac[c * 3 + 1] >> 4,
                                   dac[c * 3 + 2] << 2 | dac[c * 3 + 2] >> 4, 255);
            SDL_RenderFillRect(r, &px1);
        }
}

/* draws the game's pointer again over Freefall's button (the game drew it into its own picture,
 * under the button) */
static void game_pointer(SDL_Renderer *r, const SDL_FRect *view, float k)
{
    static Pointer pt;
    if (tnm_cursor_drawn && pointer_read(tnm_cursor, &pt))
        draw_pointer(r, &pt, view->x + tnm_draw_pos[0] * k, view->y + tnm_draw_pos[1] * k, k, k, port_video.dac,
                     &ffbutton);
}

/* the game's palette colour nearest r, g, b (0..63): the main menu's Freefall button
 * (src/game/shell/uiwidget.c) */
int port_nearest_colour(int r, int g, int b)
{
    int i, best = 0, bd = 1 << 30;
    for (i = 0; i < 256; i++) {
        int dr = port_video.dac[i * 3] - r, dg = port_video.dac[i * 3 + 1] - g, db = port_video.dac[i * 3 + 2] - b;
        int d = dr * dr + dg * dg + db * db;
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

/* the options screen's pointer: the game's own, where the mouse is, on the game's pixels, as big
 * as the game draws it (the system pointer only if the game's cannot be drawn; P86).  In the menus
 * the game's pointer follows the mouse under the options screen (platform.c) and this one is drawn
 * over it.  While a mission stands still its pointer stays where it was (often the aim over the
 * view): the menus' arrow is drawn, as big as in the base (its 640 x 480 pixels) in the base's
 * colours (P90: it was the HUD's cross, twice as tall as the game draws it in the 320 x 400
 * screen; the HUD's cross, at the screen's pixels, when no arrow was kept) */
static void options_pointer(SDL_Renderer *r, const SDL_FRect *view)
{
    float x, y, kx = port_video.w > 0 ? view->w / port_video.w : 1, ky = port_video.h > 0 ? view->h / port_video.h : 1;
    int c = cur_kept.pub, a = arrow_kept.pub, m = menu_kept.pub, ix, drawn = 1;    /* 1 too when outside */
    const Kept *kp = NULL;
    const Pointer *pt;
    if (port_options_held && m >= 0 && menu_dac_ok) {
        kp = &menu_kept;
        kx = ky = view->h / 480;
    } else if (port_options_held)
        kp = a >= 0 ? &arrow_kept : c >= 0 ? &cur_kept : NULL;
    else
        kp = c >= 0 ? &cur_kept : a >= 0 ? &arrow_kept : NULL;
    ix = kp ? kp->pub : -1;             /* the game's thread may publish the other one meanwhile */
    pt = ix >= 0 ? &kp->p[ix] : NULL;
    if (!pt)
        drawn = 0;
    else if (SDL_GetMouseFocus() && kx > 0 && ky > 0) {
        SDL_GetMouseState(&x, &y);
        SDL_RenderCoordinatesFromWindow(r, x, y, &x, &y);
        x = view->x + SDL_floorf((x - view->x) / kx) * kx;  /* on the game's pixel, as its own */
        y = view->y + SDL_floorf((y - view->y) / ky) * ky;
        draw_pointer(r, pt, x, y, kx, ky, kp == &menu_kept ? menu_dac : port_video.dac, NULL);
    }
    if (drawn && SDL_CursorVisible())
        SDL_HideCursor();
    else if (!drawn && !SDL_CursorVisible())
        SDL_ShowCursor();
}

/* the Mission Selector (missions.c): the game's pointer over its screen; its Freefall Options
 * button opens this screen over it */
void port_options_pointer(SDL_Renderer *r, const SDL_FRect *view)
{
    options_pointer(r, view);
}

void port_options_show(void)
{
    open_screen();
}

/* on the game's options screen, in the middle above its Cancel / Accept buttons (game pixels
 * 215..425 x 387..411, owner), drawn as those buttons are, in the game's font; under the
 * mouse it lights up as this screen's buttons do */
#define GB_X0 215
#define GB_Y0 387
#define GB_W 211
#define GB_H 25
static void game_options_button(SDL_Renderer *r, const SDL_FRect *view)
{
    static uint32_t px[GB_W * GB_H];
    float k = view->w / 640, wx, wy;
    int x, y, hov;
    SDL_FRect at;
    if (!load_art())
        return;
    SDL_GetMouseState(&wx, &wy);
    hov = wx >= view->x + GB_X0 * k && wx < view->x + (GB_X0 + GB_W) * k && wy >= view->y + GB_Y0 * k &&
          wy < view->y + (GB_Y0 + GB_H) * k;
    /* (the options screen's canvas is free while it is closed) */
    fill(GB_X0, GB_Y0, GB_X0 + GB_W, GB_Y0 + GB_H, BLACK);
    panel(GB_X0, GB_Y0, GB_X0 + GB_W - 1, GB_Y0 + GB_H - 1);
    box(GB_X0 + 13, GB_Y0 + 3, GB_X0 + 197, GB_Y0 + 19, BLACK);
    ctext(GB_X0 + 13, GB_X0 + 197, GB_Y0 + 5, "FREEFALL OPTIONS", hov ? ORANGE : WHITE);
    for (y = 0; y < GB_H; y++)
        for (x = 0; x < GB_W; x++) {
            int c = cv[GB_Y0 + y][GB_X0 + x];
            px[y * GB_W + x] = 0xFF000000u | (uint32_t)opal[c * 3] << 16 | (uint32_t)opal[c * 3 + 1] << 8 | opal[c * 3 + 2];
        }
    if (!gbtex) {
        gbtex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, GB_W, GB_H);
        if (!gbtex)
            return;
        SDL_SetTextureScaleMode(gbtex, SDL_SCALEMODE_NEAREST);
    }
    SDL_UpdateTexture(gbtex, NULL, px, GB_W * 4);
    at.x = view->x + GB_X0 * k, at.y = view->y + GB_Y0 * k, at.w = GB_W * k, at.h = GB_H * k;
    SDL_RenderTexture(r, gbtex, NULL, &at);
    ffbutton = at;
    game_pointer(r, view, k);
}

/* ------------------------------------------------------------------ pictures over the screen
 * The logo and the preview pictures (dist/Previews), both built into tnfree (CMakeLists.txt,
 * tools/embed_previews.py; Previews/<name>.jpg or .png next to tnfree.exe goes over a built-in
 * one, for trying new stills), drawn at the window's own pixels; textures of the renderer they
 * were made for */
extern const unsigned char port_logo_nostar[];
extern const unsigned port_logo_nostar_len;
typedef struct { const char *name; const unsigned char *data; unsigned len; } PortPreview;
extern const PortPreview port_previews[];
extern const int port_npreviews;
unsigned char *stbi_load_from_memory(const unsigned char *buffer, int len, int *x, int *y, int *comp, int req);
void stbi_image_free(void *p);
typedef struct { SDL_Texture *tex; int w, h, tried; char name[48]; unsigned used; } Pic;
static Pic logo;
#define NPICS 12
static Pic pics[NPICS];                 /* the previews loaded (the least used goes first) */
static unsigned pic_clock;

static void pic_free(Pic *p)
{
    if (p->tex)
        SDL_DestroyTexture(p->tex);
    p->tex = NULL;
    p->tried = 0;
    p->name[0] = 0;
}

void port_options_renderer_gone(void)
{
    int i;
    pic_free(&logo);
    for (i = 0; i < NPICS; i++)
        pic_free(&pics[i]);
    if (ctex)
        SDL_DestroyTexture(ctex);
    ctex = NULL;
    if (gbtex)
        SDL_DestroyTexture(gbtex);
    gbtex = NULL;
    last_r = NULL;                      /* (the mouse's place is worked out without it till the next drawing) */
}

static void pic_make(SDL_Renderer *r, Pic *p, const unsigned char *data, size_t len)
{
    int n;
    unsigned char *px = data ? stbi_load_from_memory(data, (int)len, &p->w, &p->h, &n, 4) : NULL;
    SDL_Surface *sf;
    if (!px)
        return;
    sf = SDL_CreateSurfaceFrom(p->w, p->h, SDL_PIXELFORMAT_RGBA32, px, p->w * 4);
    if (sf) {
        p->tex = SDL_CreateTextureFromSurface(r, sf);
        SDL_DestroySurface(sf);
    }
    stbi_image_free(px);
    if (p->tex) {
        SDL_SetTextureBlendMode(p->tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(p->tex, SDL_SCALEMODE_LINEAR);
    }
}

static Pic *logo_get(SDL_Renderer *r)
{
    if (!logo.tried) {
        logo.tried = 1;
        pic_make(r, &logo, port_logo_nostar, port_logo_nostar_len);
    }
    return logo.tex ? &logo : NULL;
}

static Pic *pic_get(SDL_Renderer *r, const char *name)
{
    static const char *const ext[2] = { "jpg", "png" };
    Pic *p = NULL;
    int i;
    if (!name[0])
        return NULL;
    for (i = 0; i < NPICS; i++)
        if (pics[i].tried && !strcmp(pics[i].name, name)) {
            pics[i].used = ++pic_clock;
            return pics[i].tex ? &pics[i] : NULL;
        }
    for (i = 0; i < NPICS; i++)
        if (!p || pics[i].used < p->used)
            p = &pics[i];
    pic_free(p);
    snprintf(p->name, sizeof p->name, "%s", name);
    p->tried = 1;
    p->used = ++pic_clock;
    for (i = 0; i < 2 && !p->tex; i++) {
        char path[1300];
        size_t len;
        void *data;
        snprintf(path, sizeof path, "%s/Previews/%s.%s", port_exe_dir[0] ? port_exe_dir : ".", name, ext[i]);
        if ((data = SDL_LoadFile(path, &len)) != NULL) {
            pic_make(r, p, data, len);
            SDL_free(data);
        }
    }
    for (i = 0; i < port_npreviews && !p->tex; i++)
        if (!strcmp(port_previews[i].name, name))
            pic_make(r, p, port_previews[i].data, port_previews[i].len);
    return p->tex ? p : NULL;
}

/* the picture in the box (b: the picture's pixels, k window pixels each): as big as covers it,
 * its middle */
static void pic_draw(SDL_Renderer *r, Pic *p, const SDL_Rect *b, const SDL_FRect *at, float k)
{
    SDL_FRect d = { at->x + b->x * k, at->y + b->y * k, b->w * k, b->h * k }, s = { 0, 0, 0, 0 };
    if (!p || b->w <= 0 || b->h <= 0)
        return;
    s.w = (float)p->w;
    s.h = (float)p->h;
    if (s.w * b->h > s.h * b->w) {      /* wider than the box: its sides cut */
        s.w = s.h * b->w / b->h;
        s.x = (p->w - s.w) / 2;
    } else {
        s.h = s.w * b->h / b->w;
        s.y = (p->h - s.h) / 2;
    }
    SDL_RenderTexture(r, p->tex, &s, &d);
}

/* the options screen's pointer (RESGAME.RES 0x188 item 5, its hotspot at its top left) at the
 * mouse, on the picture's pixels */
static void draw_ptr(SDL_Renderer *r, const SDL_FRect *at, float k)
{
    int i, j;
    if (mx < 0 || !bm_ptr.px || !SDL_GetMouseFocus())
        return;
    for (j = 0; j < bm_ptr.h; j++)
        for (i = 0; i < bm_ptr.w; i++) {
            int c = bm_ptr.px[j * bm_ptr.w + i];
            float l = SDL_floorf(at->x + (mx + i) * k), t = SDL_floorf(at->y + (my + j) * k);
            SDL_FRect px1 = { l, t, SDL_floorf(at->x + (mx + i + 1) * k) - l, SDL_floorf(at->y + (my + j + 1) * k) - t };
            if (!c || mx + i >= CW || my + j >= CH)
                continue;
            SDL_SetRenderDrawColor(r, opal[c * 3], opal[c * 3 + 1], opal[c * 3 + 2], 255);
            SDL_RenderFillRect(r, &px1);
        }
}

/* view: where the game's picture is in the window */
void port_options_render(SDL_Renderer *r, const SDL_FRect *view)
{
    static uint64_t menu_since;         /* the first start: since when the menu is up */
    SDL_FRect at;
    float k;
    int i;

    menu_colours();
    ffbutton.w = 0;
    ffmenu.w = 0;
    if (port_menu_ffbutton && port_video.w == 640 && port_video.h == 480) {
        float kk = view->w / 640;       /* game pixels 4..148 x 454..478 */
        ffmenu.x = view->x + 4 * kk;
        ffmenu.y = view->y + 454 * kk;
        ffmenu.w = 144 * kk;
        ffmenu.h = 24 * kk;
    }
    if (port_options_welcome && !port_options_open) {
        /* the first start: the question as soon as the game's main menu is up (its buttons drawn:
         * port_menu_ffbutton; owner, it came a second late), else once the game's menu
         * (640 x 480, no movie) has stayed up 1 s */
        if (port_lockstep || port_bench || port_video.w != 640 || port_video.h != 480 || port_movies_open > 0)
            menu_since = 0;
        else if (!menu_since && !port_menu_ffbutton)
            menu_since = SDL_GetTicks();
        else if (port_menu_ffbutton || SDL_GetTicks() - menu_since >= 1000) {
            port_options_welcome = 0;
            port_options_open = 1;
            page = PG_WELCOME;
            wsel = 0;
        }
    }
    if (!port_options_open) {
        drag = NULL;
        if (port_game_options_up && port_video.w == 640 && port_video.h == 480)
            game_options_button(r, view);
        return;
    }
    if (!load_art()) {                  /* (the game's data could not be read: no screen) */
        port_options_open = 0;
        return;
    }
    at.h = view->h;
    at.w = view->h * 4 / 3;
    if (at.w > view->w) {
        at.w = view->w;
        at.h = view->w * 3 / 4;
    }
    at.x = SDL_floorf(view->x + (view->w - at.w) / 2);
    at.y = SDL_floorf(view->y + (view->h - at.h) / 2);
    k = at.w / CW;
    last_r = r;
    last_rect = at;
    {
        float wx, wy;
        SDL_GetMouseState(&wx, &wy);
        to_canvas(wx, wy, &mx, &my);
    }

    /* the picture */
    memset(cv, BLACK, sizeof cv);
    nhits = norder = 0;
    hover_g = NULL;
    hover_v = -1;
    prev_box.w = 0;
    if (page == PG_WELCOME)
        draw_welcome();
    else if (page == PG_KEYS)
        draw_keys();
    else {
        int pass;                       /* twice: the mouse's group is known after the first */
        for (pass = 0; pass < 2; pass++) {
            if (pass) {
                memset(cv, BLACK, sizeof cv);
                nhits = norder = 0;
                prev_box.w = 0;
            }
            draw_page();
            if (!pass && !hover_g)
                break;
        }
        if (focus_g) {
            set_focus(focus_g);
            if (focus < 0)              /* not on this page */
                focus_g = NULL;
        }
        preview_pick();
    }
    for (i = 0; i < CW * CH; i++) {
        int c = ((uint8_t *)cv)[i];
        orgba[i] = 0xFF000000u | (uint32_t)opal[c * 3] << 16 | (uint32_t)opal[c * 3 + 1] << 8 | opal[c * 3 + 2];
    }
    if (!ctex) {
        ctex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, CW, CH);
        if (ctex)
            SDL_SetTextureScaleMode(ctex, SDL_SCALEMODE_NEAREST);
    }
    if (!ctex)
        return;
    SDL_UpdateTexture(ctex, NULL, orgba, CW * 4);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderFillRect(r, view);
    SDL_RenderTexture(r, ctex, NULL, &at);

    {   /* the logo in the header's box, 19 of the picture's pixels high, in the middle */
        Pic *l = logo_get(r);
        if (l) {
            SDL_FRect d;
            d.h = SDL_roundf(logo_box.h * k);
            d.w = SDL_roundf(d.h * l->w / l->h);
            d.x = SDL_roundf(at.x + (logo_box.x + logo_box.w / 2.f) * k - d.w / 2);
            d.y = SDL_roundf(at.y + logo_box.y * k);
            SDL_RenderTexture(r, l->tex, NULL, &d);
        }
    }
    if (page == PG_WELCOME) {
        for (i = 0; i < 3; i++) {
            Pic *p = pic_get(r, wstill[i]);
            if (p)
                pic_draw(r, p, &wprev[i], &at, k);
        }
    } else if (prev_box.w > 0) {
        Pic *p = pic_get(r, prev_name);
        if (p)
            pic_draw(r, p, &prev_box, &at, k);
    }
    draw_ptr(r, &at, k);
    if (SDL_CursorVisible())
        SDL_HideCursor();
}
