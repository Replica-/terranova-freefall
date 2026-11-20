/* main.c -- entry point of the standalone build (Terra Nova: Freefall).
 *
 * usage: tnfree [--game DIR] [--cd DIR] [--user DIR] [--music M] [--fps N] [--mouselook] [--mission N]
 *               [--preset P] [--bench]
 *               [--check] [game arguments...]
 *   --game DIR  the game's TNOVA folder (holding __FF.EXE, DATA, ...).  Without it: the current
 *               folder if __FF.EXE is there, else the folder chosen last time, else an installed
 *               Steam or GOG copy found by itself (findgame.c), else a folder
 *               dialog asks (the choice is kept in tnfree.ini, next to tnfree.exe).
 *   --cd DIR    the contents of the game CD (MAPS, MOVIE, ...), if they are not in the game
 *               folder
 *   --user DIR  where the game's own files go (config, saves) and tnfree.ini; default: the
 *               folder Saves next to tnfree.exe (with tnfree.ini beside the exe).  Freefall
 *               keeps everything it writes in its own folder, nothing in the system's
 *               application-data places.
 *   --music M   music output: opl3 (OPL3 FM synthesis, the Sound Blaster 16 / Pro 2 sound), opl2
 *               (OPL2 FM, the AdLib / Sound Blaster sound), gm (the system's General MIDI
 *               synthesizer), soundfont (the Sound Blaster AWE32 sound: FluidSynth with a
 *               SoundFont from the Music folder and the game's AWE32 bank) or off; kept in tnfree.ini.  auto (the default) takes what the
 *               game's sound setup chose and is saved as that; fm is opl3 (earlier builds)
 *   --fps N     at most N game frames a second (default 25, as on the PCs of its time); 0 = no
 *               limit.  Above 25 the projectile hit test is fixed so shots still hit (the
 *               original's never hit above about 30 frames a second, docs/BUGS.md G4)
 *   --mouselook the Advanced controls style: mouselook in missions (the reticle stays at the
 *               view's centre; the middle button frees the pointer for the HUD); also
 *               controls=advanced in tnfree.ini (classic, before vanilla: the game's), with
 *               mouse_sensitivity=100 (percent) and mouse_invert=on|off
 * tnfree.ini fullscreen=on starts the game full screen (off: a window; Alt+Enter switches); with
 * no fullscreen line yet (the first start) it starts full screen, in a window if that fails.
 * tnfree.ini smooth_fps=true runs at 60 frames a second (with the hit test fix), false (the
 * default) at the game's 25; --fps overrides it.  In the game, O (in a mission Esc too)
 * opens the options screen (options.c), which changes these settings and saves them here.
 *   --mission N New Campaign starts at the campaign's mission N (the game's mission numbers:
 *               README.txt), with its briefing and outfitting, and the campaign goes on from
 *               there; only for this run (the game's own start mission, ff_options[0x50], camplist.c)
 *   --preset P  classic, vanilla+ or freefall: the options screen's Preset, chosen before the game
 *               starts and saved in tnfree.ini (options.c)
 *   --bench     the benchmark: a fixed walk through mission 3 with the settings in tnfree.ini,
 *               timed; the results are added to tnfree-bench.txt next to tnfree.ini (bench.c)
 *   --check     only load the original data from __FF.EXE, check it and exit
 *   --smooth-test [--gpu N] [--out DIR] [MOVIE ...]
 *               smooths movies with RIFE into AVI files to look at, then exits (smoothtest.c)
 * Other arguments go to the game (e.g. -d, +8M, as for the DOS version).
 */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include "keys.h"
#include "port.h"
#include "screen.h"
#include "sharp3d.h"
#include "version.h"
#include "videosmooth.h"
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#endif

int game_main(int argc, char **argv);
char port_exe_dir[1024];
int port_start_mission, port_new_campaign;     /* --mission N (camplist.c, ffmain.c) */
extern int port_ms_all;                         /* Mission select: All (missions.c) */
int port_platform_init(void);
int port_run(int (*fn)(int, char **), int argc, char **argv);
void port_files_init(const char *user_dir, const char *cd_copy_dir);
void port_divtrap_install(void);
void port_lockstep_init(void);

/* Pointers the game leaves at 0 and then uses: on DOS/4GW linear address 0 is the start of
 * conventional memory, so these reads and writes went to low memory and did no harm.  Here
 * they get a zeroed block of their own. */
unsigned char port_low_mem[1 << 20];    /* see TN_LOWMEM in lg_types.h */

static void port_low_memory_init(void)
{
}

static int count_code_ptrs(void)
{
    /* how many words of the data object hold one of our function pointers */
    unsigned long i;
    int n = 0;
    for (i = 0; i + 4 <= TN_DATA_SIZE; i += 4) {
        unsigned long v;
        memcpy(&v, tn_data_obj + i, 4);
        if (v && port_orig_addr((void *)v))
            n++;
    }
    return n;
}

/* how many of our routines lie where the original program's code was (0 when the program is
 * loaded at its own base; see P40 in docs/BUGS.md) */
static int code_overlap(void)
{
    int i, n = 0;
    for (i = 0; i < port_nfuncs; i++)
        if ((unsigned long)port_funcs[i].fn >= TN_CODE_BASE && (unsigned long)port_funcs[i].fn < TN_CODE_BASE + TN_CODE_SIZE)
            n++;
    return n;
}

static int has_exe(const char *dir)
{
    char p[1100];
    FILE *f;
    snprintf(p, sizeof p, "%s/__FF.EXE", dir);
    f = fopen(p, "rb");
#ifndef _WIN32
    if (!f) {
        snprintf(p, sizeof p, "%s/__ff.exe", dir);
        f = fopen(p, "rb");
    }
#endif
    if (f)
        fclose(f);
    return f != NULL;
}

/* ---------------------------------------------------------------- tnfree.ini */
static char ini_path[1100];

static void ini_read(const char *key, char *out, int size)
{
    FILE *f = fopen(ini_path, "r");
    char line[1200];
    size_t n = strlen(key);
    if (!f)
        return;
    while (fgets(line, sizeof line, f))
        if (strncmp(line, key, n) == 0 && line[n] == '=') {
            char *v = line + n + 1;
            v[strcspn(v, "\r\n")] = 0;
            snprintf(out, size, "%s", v);
        }
    fclose(f);
}

static void ini_write_dirs(const char *game, const char *cd)
{
    FILE *f = fopen(ini_path, "w");
    if (!f)
        return;
    if (game[0])
        fprintf(f, "game=%s\n", game);
    if (cd[0])
        fprintf(f, "cd=%s\n", cd);
    if (port_help_seen)
        fprintf(f, "help_seen=on\n");
    fprintf(f, "music=%s\n", port_music_mode);
    fprintf(f, "controls=%s\n", port_mouselook ? "advanced" : "classic");    /* (vanilla before) */
    fprintf(f, "mouse_sensitivity=%d\n", port_mouselook_sens);
    fprintf(f, "mouse_invert=%s\n", port_mouselook_invert ? "on" : "off");
    fprintf(f, "fullscreen=%s\n", port_fullscreen ? "on" : "off");
    fprintf(f, "smooth_fps=%s\n", port_smooth_fps ? "true" : "false");
    fprintf(f, "auto_target_camera=%s\n", port_auto_targcam ? "on" : "off");
    fprintf(f, "target_label=%s\n", port_target_label ? "box" : "original");
    fprintf(f, "map_view_cone=%s\n", port_map_cone ? "on" : "off");
    fprintf(f, "enemies_without_mfd=%s\n", port_map_enemies ? "on" : "off");
    fprintf(f, "radar_without_mfd=%s\n", port_map_radar ? "on" : "off");
    if (port_aim_box)
        fprintf(f, "auto_aim=%d\n", port_aim_box);
    else
        fprintf(f, "auto_aim=off\n");
    fprintf(f, "aim_highlight=%s\n", port_aim_sign ? "on" : "off");
    fprintf(f, "mission_select=%s\n", port_ms_all ? "all" : "reached");
    fprintf(f, "show_fps=%s\n", port_show_fps ? "on" : "off");
    fprintf(f, "smooth_videos=%s\n", port_smooth_videos ? "30" : "off");
    fprintf(f, "music_pack=%s\n", port_music_pack);
    if (port_soundfont[0])
        fprintf(f, "soundfont=%s\n", port_soundfont);
    port_keys_save(f);                  /* key. / advkey.<action>= lines, only the changed ones */
    port_screen_save(f);                /* screen_<filter>= lines, only the changed ones */
    fclose(f);
}

static void ini_write(void)
{
    ini_write_dirs(port_game_dir, port_cd_dir);
}

/* the options screen (options.c, main thread): the settings change, the saved folders stay */
void port_save_settings(void)
{
    char g[1024] = "", c[1024] = "";
    ini_read("game", g, sizeof g);
    ini_read("cd", c, sizeof c);
    ini_write_dirs(g[0] ? g : port_game_dir, g[0] ? c : port_cd_dir);
}

static int music_mode_ok(const char *m)
{
    return !strcmp(m, "auto") || !strcmp(m, "fm") || !strcmp(m, "opl3") || !strcmp(m, "opl2") ||
           !strcmp(m, "gm") || !strcmp(m, "soundfont") || !strcmp(m, "off");
}

/* ---------------------------------------------------------------- folder dialog */
static volatile int dialog_done;

static void SDLCALL folder_chosen(void *ud, const char *const *list, int filter)
{
    (void)ud;
    (void)filter;
    if (list && list[0])
        snprintf(port_game_dir, sizeof port_game_dir, "%s", list[0]);
    else
        port_game_dir[0] = 0;
    dialog_done = 1;
}

static int ask_game_dir(void)
{
    for (;;) {
        SDL_Event e;
        dialog_done = 0;
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Terra Nova: Freefall",
                                 "Please choose the folder of your Terra Nova game: its Steam or GOG "
                                 "installation folder (the one that holds the TNOVA folder on Steam, "
                                 "the TN folder on GOG).", NULL);
        SDL_ShowOpenFolderDialog(folder_chosen, NULL, NULL, NULL, false);
        while (!dialog_done) {
            while (SDL_PollEvent(&e))
                ;
            SDL_Delay(20);
        }
        if (!port_game_dir[0])
            return 0;
        if (has_exe(port_game_dir))
            return 1;
        {                           /* the game's folder in it: TNOVA (Steam), TN (GOG) */
            static const char *const names[] = { "TNOVA", "TN" };
            char sub[1100];
            int i;
            for (i = 0; i < 2; i++) {
                snprintf(sub, sizeof sub, "%s/%s", port_game_dir, names[i]);
                if (has_exe(sub)) {
                    snprintf(port_game_dir, sizeof port_game_dir, "%s", sub);
                    return 1;
                }
            }
        }
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Terra Nova: Freefall",
                                 "That folder has no Terra Nova game.  Please choose the game's Steam "
                                 "or GOG installation folder (the one that holds the TNOVA or TN "
                                 "folder).", NULL);
    }
}

/* 1 if a file can be made in folder dir */
static int folder_writable(const char *dir)
{
    char p[1100];
    FILE *f;
    snprintf(p, sizeof p, "%s/tnfree-write-test.tmp", dir);
    if ((f = fopen(p, "w")) == NULL)
        return 0;
    fclose(f);
    remove(p);
    return 1;
}

int main(int argc, char **argv)
{
    char exe[1100], user[1024] = "", exe_dir[1024], own_dir[1024], cd_copy[1100];
    const char *err;
    char music[16] = "";
    int check = 0, smooth_test = 0, i, gargc = 1, have_game = 0, mouselook_arg = 0, fps_arg = 0, ini_old = 0;
    int first_start = 0, aim_legacy = 0, hl_legacy = 0;
    const char *preset = NULL;
    static char *gargv[32];

    port_log("Terra Nova: Freefall %s (commit %s)\n", FREEFALL_VERSION_FULL, FREEFALL_COMMIT);
#ifdef _WIN32
    _fmode = _O_BINARY;     /* the Watcom library opened files in binary mode */
#endif
    gargv[0] = argv[0];
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--game") == 0 && i + 1 < argc) {
            snprintf(port_game_dir, sizeof port_game_dir, "%s", argv[++i]);
            have_game = 1;
        } else if (strcmp(argv[i], "--cd") == 0 && i + 1 < argc)
            snprintf(port_cd_dir, sizeof port_cd_dir, "%s", argv[++i]);
        else if (strcmp(argv[i], "--user") == 0 && i + 1 < argc)
            snprintf(user, sizeof user, "%s", argv[++i]);
        else if (strcmp(argv[i], "--music") == 0 && i + 1 < argc) {
            snprintf(music, sizeof music, "%s", argv[++i]);
            if (!music_mode_ok(music)) {
                port_log("--music takes opl3, opl2, gm, soundfont, off or auto\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            int fps = atoi(argv[++i]);
            port_frame_us = fps > 0 ? 1000000 / fps : 0;
            port_highfps_fix = fps <= 0 || fps > 25;
            fps_arg = 1;
        } else if (strcmp(argv[i], "--mouselook") == 0)
            mouselook_arg = 1;
        else if (strcmp(argv[i], "--bench") == 0)
            port_bench = 1;
        else if (strcmp(argv[i], "--mission") == 0 && i + 1 < argc) {
            port_start_mission = atoi(argv[++i]);
            if (port_start_mission < 1 || port_start_mission > 0x2F) {
                port_log("--mission takes a mission number from 1 to 47 (README.txt lists them)\n");
                return 1;
            }
        }
        else if (strcmp(argv[i], "--preset") == 0 && i + 1 < argc) {
            preset = argv[++i];
            if (SDL_strcasecmp(preset, "classic") && SDL_strcasecmp(preset, "vanilla+") &&
                SDL_strcasecmp(preset, "vanillaplus") && SDL_strcasecmp(preset, "freefall")) {
                port_log("--preset takes classic, vanilla+ or freefall\n");
                return 1;
            }
        }
        else if (strcmp(argv[i], "--check") == 0)
            check = 1;
        else if (strcmp(argv[i], "--smooth-test") == 0) {
            smooth_test = i + 1;            /* the rest of the arguments are the test's */
            break;
        }
        else if (strcmp(argv[i], "--run") == 0)
            ;                       /* the default now (kept for old scripts) */
        else if (gargc < 31)
            gargv[gargc++] = argv[i];
    }
    gargv[gargc] = 0;
    for (i = 1; i < gargc; i++)
        port_log("Game argument: %s\n", gargv[i]);

    if (!check && !smooth_test && port_platform_init() != 0)
        return 1;
    {                               /* the folder of tnfree.exe */
        const char *b = SDL_GetBasePath();
        size_t n;
        snprintf(exe_dir, sizeof exe_dir, "%s", b && b[0] ? b : ".");
        n = strlen(exe_dir);
        while (n > 1 && (exe_dir[n - 1] == '/' || exe_dir[n - 1] == '\\'))
            exe_dir[--n] = 0;
        snprintf(port_exe_dir, sizeof port_exe_dir, "%s", exe_dir);     /* a Previews folder (options.c) */
    }
    if (!user[0]) {                 /* Saves, CD and tnfree.ini next to tnfree.exe */
        snprintf(user, sizeof user, "%s/Saves", exe_dir);
        snprintf(ini_path, sizeof ini_path, "%s/tnfree.ini", exe_dir);
        snprintf(cd_copy, sizeof cd_copy, "%s/CD", exe_dir);
        snprintf(own_dir, sizeof own_dir, "%s", exe_dir);
    } else {                        /* --user: all of it in that folder */
        snprintf(ini_path, sizeof ini_path, "%s/tnfree.ini", user);
        snprintf(cd_copy, sizeof cd_copy, "%s/CD", user);
        snprintf(own_dir, sizeof own_dir, "%s", user);
#ifdef _WIN32
        _mkdir(user);
#else
        mkdir(user, 0755);
#endif
    }
    if (!check && !folder_writable(own_dir)) {
        char msg[1300];
        snprintf(msg, sizeof msg, "Terra Nova: Freefall keeps its settings, saved games and its copy "
                 "of the CD files in its own folder, but it cannot write to\n%s\n\nPlease put "
                 "tnfree.exe in a folder of its own that you can write to (not Program Files).",
                 own_dir);
        port_log("%s\n", msg);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Terra Nova: Freefall", msg, NULL);
        return 1;
    }
    {
        char m[16] = "";
        ini_read("music", m, sizeof m);
        if (music_mode_ok(m))
            snprintf(port_music_mode, sizeof port_music_mode, "%s", m);
        if (music[0])
            snprintf(port_music_mode, sizeof port_music_mode, "%s", music);
    }
    {                               /* controls style (platform.c), Classic unless asked for */
        char v[16] = "";
        ini_read("controls", v, sizeof v);
        if (!v[0]) {                /* older tnfree.ini: mouselook=on was the Advanced style */
            ini_read("mouselook", v, sizeof v);
            first_start = !v[0];    /* no settings yet: the options screen asks for the preset */
            port_mouselook = !strcmp(v, "on");
            ini_old = 1;
        } else
            port_mouselook = !strcmp(v, "advanced");
        port_mouselook |= mouselook_arg;
        v[0] = 0;
        ini_read("mouse_sensitivity", v, sizeof v);
        if (atoi(v) > 0)                /* (P141: as the options screen's slider: 10..500%) */
            port_mouselook_sens = atoi(v) < 10 ? 10 : atoi(v) > 500 ? 500 : atoi(v);
        v[0] = 0;
        ini_read("mouse_invert", v, sizeof v);
        port_mouselook_invert = !strcmp(v, "on");
        v[0] = 0;
        ini_read("auto_target_camera", v, sizeof v);
        port_auto_targcam = !strcmp(v, "on");
        v[0] = 0;
        ini_read("target_label", v, sizeof v);
        port_target_label = !strcmp(v, "box");
        v[0] = 0;
        ini_read("map_view_cone", v, sizeof v);
        port_map_cone = !strcmp(v, "on");
        v[0] = 0;
        ini_read("enemies_without_mfd", v, sizeof v);
        port_map_enemies = !strcmp(v, "on");
        v[0] = 0;
        ini_read("radar_without_mfd", v, sizeof v);
        port_map_radar = !strcmp(v, "on");
        v[0] = 0;
        ini_read("auto_aim", v, sizeof v);          /* the box: 20 (the game's, default), 5 or off */
        i = atoi(v);                                /* (test builds: marker, 10) */
        port_aim_box = !strcmp(v, "off") ? 0 : i == 5 || i == 10 || !strcmp(v, "marker") ? AIM_SMALL
                                                                                          : AIM_CLASSIC;
        aim_legacy = !v[0] && !first_start;         /* an older tnfree.ini: by its controls */
        ini_old |= aim_legacy;
        v[0] = 0;
        ini_read("aim_highlight", v, sizeof v);     /* off (default) or on */
        port_aim_sign = !strcmp(v, "on");
        if (!v[0]) {                                /* an older tnfree.ini: by its preset */
            ini_read("aim_sign", v, sizeof v);      /* (test builds: off, cross or centre) */
            port_aim_sign = !strcmp(v, "cross") || !strcmp(v, "centre");
            hl_legacy = !port_aim_sign && !first_start;
            ini_old |= !first_start;
        }
        v[0] = 0;
        ini_read("mission_select", v, sizeof v);    /* reached (default) or all: missions.c */
        port_ms_all = !strcmp(v, "all");
        v[0] = 0;
        ini_read("fullscreen", v, sizeof v);        /* none yet (the first start): full screen, */
        port_fullscreen = v[0] ? !strcmp(v, "on") : /* but tests ask for a window */
                          !SDL_getenv("FREEFALL_WINDOW") && !SDL_getenv("FREEFALL_LOCKSTEP");
        v[0] = 0;
        ini_read("show_fps", v, sizeof v);
        port_show_fps = !strcmp(v, "on");
        v[0] = 0;
        ini_read("smooth_videos", v, sizeof v);     /* off (default) or 30: videosmooth.c */
        port_smooth_videos = !strcmp(v, "30");
    }
    port_keys_load(ini_read);       /* keybindings (keys.c) */
    port_screen_load(ini_read);     /* screen filters (screen.c) */
    {                               /* music packs (musicpack.c): Music/<pack> */
        char v[128] = "x";
        snprintf(port_music_dir, sizeof port_music_dir, "%s/Music", own_dir);
        snprintf(port_mods_dir, sizeof port_mods_dir, "%s/Mods", own_dir);     /* mod packs (fxpack.c) */
        ini_read("music_pack", v, sizeof v);
        ini_old |= !strcmp(v, "x");
        snprintf(port_music_pack, sizeof port_music_pack, "%s", strcmp(v, "x") ? v : "");
        v[0] = 0;
        ini_read("soundfont", v, sizeof v);     /* SoundFont music's file in Music/ (sfsynth.c) */
        snprintf(port_soundfont, sizeof port_soundfont, "%s", v);
    }
    {                               /* help_seen: the help screen was opened once (help.c: no hint) */
        char v[16] = "";
        ini_read("help_seen", v, sizeof v);
        port_help_seen = !strcmp(v, "on");
    }
    {                               /* smooth_fps: 60 frames a second, unless --fps says otherwise */
        char v[16] = "";
        ini_read("smooth_fps", v, sizeof v);
        port_smooth_fps = !strcmp(v, "true") || !strcmp(v, "on") || !strcmp(v, "1");
        ini_old |= !v[0];
        if (port_smooth_fps && !fps_arg)
            port_set_smooth_fps(1);
    }
    if (!have_game) {
        if (has_exe("."))
            snprintf(port_game_dir, sizeof port_game_dir, ".");
        else {
            port_game_dir[0] = 0;
            ini_read("game", port_game_dir, sizeof port_game_dir);
            if (!port_cd_dir[0])
                ini_read("cd", port_cd_dir, sizeof port_cd_dir);
            if (!port_game_dir[0] || !has_exe(port_game_dir)) {     /* an installed game (findgame.c) */
                const char *shop;
                char found[1024];
                if (port_find_game(found, sizeof found, &shop)) {
                    snprintf(port_game_dir, sizeof port_game_dir, "%s", found);
                    port_cd_dir[0] = 0;
                    port_log("Found the game by itself (%s): %s\n", shop, port_game_dir);
                }
            }
            if ((!port_game_dir[0] || !has_exe(port_game_dir)) && (check || smooth_test || !ask_game_dir())) {
                port_log("Terra Nova: Freefall needs the original game. Start it with --game <the TNOVA folder>.\n");
                return 2;
            }
        }
    }

    if (smooth_test) {
        port_files_init(user, cd_copy);
        return vsmooth_test_main(argc - smooth_test, argv + smooth_test);
    }

    snprintf(exe, sizeof exe, "%s/__FF.EXE", port_game_dir);
#ifndef _WIN32
    {
        FILE *f = fopen(exe, "rb");
        if (f)
            fclose(f);
        else
            snprintf(exe, sizeof exe, "%s/__ff.exe", port_game_dir);
    }
#endif
    err = port_load_image(exe);
    if (err) {
        /* Terra Nova Plus changes __FF.EXE for its HD mode and keeps the original next to it */
        char orig[1100];
        snprintf(orig, sizeof orig, "%s.tnplus-original", exe);
        if (!port_load_image(orig)) {
            port_log("%s\n%s was changed by Terra Nova Plus: using its copy of the original.\n", err, exe);
            snprintf(exe, sizeof exe, "%s", orig);
            err = 0;
        }
    }
    if (err) {
        port_log("Terra Nova: Freefall needs the original game (English v1.09, GOG / Steam).\n%s\n", err);
        if (!check)
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Terra Nova: Freefall", err, NULL);
        return 2;
    }
    port_log("Loaded the original data from %s: %d functions mapped, %d code pointers in the data.\n",
             exe, port_nfuncs, count_code_ptrs());
    if (code_overlap())
        port_log("Note: the system loaded tnfree low, over the original program's addresses (%d of "
                 "our routines there, the first at %p).\n", code_overlap(), (void *)port_funcs[0].fn);
    if (check)
        return 0;
    port_low_memory_init();
    port_divtrap_install();
    port_static_init();
    port_files_init(user, cd_copy);
    if (aim_legacy || hl_legacy)    /* (the Mods folder known: the preset's mod set) */
        port_options_aim_legacy(aim_legacy, hl_legacy);
    if (preset) {                   /* --preset: as if chosen on the options screen */
        port_options_preset(preset);
        port_log("--preset %s\n", preset);
        port_save_settings();       /* saved as the options screen saves it (also with --game) */
    }
    /* the first start's question (options.c); FREEFALL_WELCOME=1 asks anyway (tests), and test
     * scripts (FREEFALL_SCRIPT) are not asked unless that is set */
    port_options_welcome = !preset && ((first_start && !SDL_getenv("FREEFALL_SCRIPT")) ||
                                       (SDL_getenv("FREEFALL_WELCOME") && *SDL_getenv("FREEFALL_WELCOME") == '1'));
    if (!have_game || music[0] || ini_old) {   /* ini_old: write the newer settings into it */
        if (have_game || !strcmp(port_game_dir, ".")) {    /* keep the saved folders */
            char g[1024] = "", c[1024] = "";
            ini_read("game", g, sizeof g);
            ini_read("cd", c, sizeof c);
            if (g[0])
                ini_write_dirs(g, c);
        } else
            ini_write();
    }
    snprintf(port_bench_path, sizeof port_bench_path, "%s/tnfree-bench.txt", own_dir);
    port_lockstep_init();
    return port_run(game_main, gargc, gargv);   /* (it starts loading RIFE: platform.c) */
}

/* the credits' first page (src/game/shell/debrief.c shell_2BF760): Freefall's, in the codes of the
 * game's credits (resource 0x5B0): ^Cca columns, ^n n blank lines, ^Rn colour n, ^P the page's
 * end (owner) */
const char *port_credit_line(int i)
{
    static const char *const lines[] = {
        "^C10", "^3", "^R0",            /* (white, capitals: owner) */
        "TERRA NOVA: FREEFALL",
        "^2", "^R0",
        "A modern SDL3 port of",
        "Terra Nova: Strike Force Centauri",
        "(Looking Glass Technologies, 1996)",
        "^2", "^R1",                    /* (the game's tan, as its roles) */
        "Created By",
        "^R0",
        "Nikolay \"Cyberion\" Ivliev",
        "^1", "^R1",
        "MP3 Music Synthesized By",     /* the Freefall Music pack (Music/Freefall-MP3.zip) */
        "^R0",
        "Rich \"Weeds\" Nagel",
        "^3",                           /* the version at the page's bottom */
        "Version " FREEFALL_VERSION_FULL,
        "^P" };
    return i >= 0 && i < (int)(sizeof lines / sizeof lines[0]) ? lines[i] : "^P";
}
