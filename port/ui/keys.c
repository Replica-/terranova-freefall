/* keys.c -- keybindings (Terra Nova: Freefall quality of life).
 *
 * The game's code is not changed: Freefall translates each physical key into one of the
 * original keys before the game sees it (platform.c key_event), so both ways the game reads the
 * keyboard (the held-key table and the key events) follow the bindings.  Each action has up to
 * two keys, each standing for one of the game's own keys (slot 1 for W, slot 2 for the Up arrow,
 * ...), so combinations the game treats differently (Alt + arrows strafe, Alt + A does not) stay
 * as they are.  The middle mouse button, mouse buttons 4 and 5 and the wheel can be bound too, and
 * Freefall's own actions (mouselook switch, mouselook hold, gimbal mode, show FPS) are actions
 * like the others.
 *
 * Each controls style (port_mouselook: 0 Vanilla, 1 Advanced) has its own list, order, names,
 * defaults and bindings.  Vanilla is the original game's keys; Advanced moves to S back and Z
 * slow forward, strafes with A and D and turns with Q and E, frees R V C and Left Shift (the mouse
 * looks), zooms with the wheel, and adds the mouselook actions and gimbal mode.
 *
 * Bindings apply only while a mission's controls run; in the menus, the base and the briefing
 * every key is the game's own.  With no binding changed in Vanilla nothing is translated.
 * Changed actions are saved in tnfree.ini as key.<name>=<key>,<key> (Vanilla) or
 * advkey.<name>=... (Advanced) (SDL key names, "MMB", "Mouse 4", "Mouse 5", "Wheel Up",
 * "Wheel Down", or "none").
 */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include "platform.h"
#include "keys.h"

#define NONE SDL_SCANCODE_UNKNOWN
typedef struct {
    const char *name, *label;
    int game[2];                        /* the game's keys this action's two slots stand for */
} KeyAction;

enum {
    A_FORWARD, A_FORWARD_SLOW, A_BACK, A_TURN_LEFT, A_TURN_RIGHT, A_STRAFE_LEFT, A_STRAFE_RIGHT,
    A_STRAFE_MOD, A_BOUND, A_JUMP, A_LOOK_UP, A_LOOK_DOWN, A_HEAD_LEFT, A_HEAD_RIGHT, A_LOOK_AHEAD,
    A_MOUSELOOK, A_MOUSELOOK_HOLD, A_GIMBAL, A_FIRE, A_WEAPON1, A_WEAPON2, A_WEAPON3, A_WEAPON4,
    A_TARGET_PREV, A_TARGET_NEXT, A_DRONE, A_HELMET, A_VIEW360, A_INFRARED, A_MAP, A_NAV,
    A_ZOOM_IN, A_ZOOM_OUT
};

static const KeyAction actions[] = {
    { "forward", "Forward", { SDL_SCANCODE_W, SDL_SCANCODE_UP } },
    { "forward_slow", "Forward (half speed)", { SDL_SCANCODE_S, NONE } },
    { "back", "Back", { SDL_SCANCODE_X, SDL_SCANCODE_DOWN } },
    { "turn_left", "Turn left", { SDL_SCANCODE_A, SDL_SCANCODE_LEFT } },
    { "turn_right", "Turn right", { SDL_SCANCODE_D, SDL_SCANCODE_RIGHT } },
    { "strafe_left", "Strafe left", { SDL_SCANCODE_Z, NONE } },
    { "strafe_right", "Strafe right", { SDL_SCANCODE_C, NONE } },
    { "strafe_mod", "Arrows strafe (hold)", { SDL_SCANCODE_LALT, SDL_SCANCODE_RALT } },
    { "bound", "Short bound (hold)", { SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RSHIFT } },
    { "jump", "Jump jets", { SDL_SCANCODE_SPACE, NONE } },
    { "look_up", "Look up", { SDL_SCANCODE_R, SDL_SCANCODE_KP_8 } },
    { "look_down", "Look down", { SDL_SCANCODE_V, SDL_SCANCODE_KP_2 } },
    { "head_left", "Head left", { SDL_SCANCODE_Q, SDL_SCANCODE_KP_4 } },
    { "head_right", "Head right", { SDL_SCANCODE_E, SDL_SCANCODE_KP_6 } },
    { "look_ahead", "Look straight ahead", { SDL_SCANCODE_F, SDL_SCANCODE_KP_5 } },
    { "mouselook", "Mouselook", { PORT_KEY_MOUSELOOK, NONE } },
    { "mouselook_hold", "Mouselook (hold)", { PORT_KEY_MOUSELOOK_HOLD, NONE } },
    { "gimbal", "Gimbal mode", { PORT_KEY_GIMBAL, NONE } },
    { "fire", "Fire at target", { SDL_SCANCODE_5, NONE } },
    { "weapon1", "Weapon 1", { SDL_SCANCODE_1, NONE } },
    { "weapon2", "Weapon 2", { SDL_SCANCODE_2, NONE } },
    { "weapon3", "Weapon 3", { SDL_SCANCODE_3, NONE } },
    { "weapon4", "Weapon 4", { SDL_SCANCODE_4, NONE } },
    { "target_prev", "Previous target", { SDL_SCANCODE_T, SDL_SCANCODE_LEFTBRACKET } },
    { "target_next", "Next target", { SDL_SCANCODE_Y, SDL_SCANCODE_RIGHTBRACKET } },
    { "drone", "Drone control (hold)", { SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL } },
    { "helmet", "Full-screen goggles", { SDL_SCANCODE_G, NONE } },
    { "view360", "360-degree view", { SDL_SCANCODE_B, NONE } },
    { "infrared", "Infrared", { SDL_SCANCODE_I, NONE } },
    { "map", "Full-screen map", { SDL_SCANCODE_M, NONE } },
    { "nav", "Next nav point", { SDL_SCANCODE_N, NONE } },
    { "zoom_in", "Zoom in", { SDL_SCANCODE_EQUALS, SDL_SCANCODE_KP_PLUS } },
    { "zoom_out", "Zoom out", { SDL_SCANCODE_MINUS, SDL_SCANCODE_KP_MINUS } },
    { "map_out", "Map zoom out", { SDL_SCANCODE_COMMA, NONE } },
    { "map_in", "Map zoom in", { SDL_SCANCODE_PERIOD, NONE } },
    { "view_9", "View mode (9)", { SDL_SCANCODE_9, NONE } },
    { "view_0", "View mode (0)", { SDL_SCANCODE_0, NONE } },
    { "mfd", "MFD panels", { SDL_SCANCODE_TAB, NONE } },
    { "command", "Command panel", { SDL_SCANCODE_RETURN, NONE } },
    { "squad1", "Talk to squadmate 1", { SDL_SCANCODE_F1, NONE } },
    { "squad2", "Talk to squadmate 2", { SDL_SCANCODE_F2, NONE } },
    { "squad3", "Talk to squadmate 3", { SDL_SCANCODE_F3, NONE } },
    { "squad_all", "Talk to the squad", { SDL_SCANCODE_F4, NONE } },
    { "objectives", "Mission objectives", { SDL_SCANCODE_F8, NONE } },
    { "map_squad", "Map: squad info", { SDL_SCANCODE_F9, NONE } },
    { "map_sensor", "Map: sensor range", { SDL_SCANCODE_F10, NONE } },
    { "map_weapon", "Map: weapon range", { SDL_SCANCODE_F11, NONE } },
    { "map_mission", "Map: mission info", { SDL_SCANCODE_F12, NONE } },
    { "light", "L key (game function)", { SDL_SCANCODE_L, NONE } },
    { "biped", "` key (game function)", { SDL_SCANCODE_GRAVE, NONE } },
    { "pause", "Pause", { SDL_SCANCODE_P, NONE } },
    { "options", "Help screen", { SDL_SCANCODE_H, NONE } },
    { "show_fps", "Show FPS", { PORT_KEY_SHOW_FPS, NONE } },
};
#define NACT ((int)(sizeof actions / sizeof actions[0]))
#define NSTYLE 2

/* each style's list: these actions first, then every later action in table order */
static const signed char order_head[NSTYLE][32] = {
    { A_FORWARD, A_BACK, A_TURN_LEFT, A_TURN_RIGHT, A_FORWARD_SLOW, A_STRAFE_LEFT, A_STRAFE_RIGHT,
      A_JUMP, A_LOOK_UP, A_LOOK_DOWN, A_HEAD_LEFT, A_HEAD_RIGHT, A_LOOK_AHEAD, A_FIRE, A_WEAPON1,
      A_WEAPON2, A_WEAPON3, A_WEAPON4, A_TARGET_PREV, A_TARGET_NEXT, A_STRAFE_MOD, A_BOUND, -1 },
    { A_MOUSELOOK, A_MOUSELOOK_HOLD, A_FORWARD, A_BACK, A_TURN_LEFT, A_TURN_RIGHT, A_FORWARD_SLOW,
      A_STRAFE_LEFT, A_STRAFE_RIGHT, A_LOOK_AHEAD, A_GIMBAL, A_JUMP, A_LOOK_UP, A_LOOK_DOWN,
      A_HEAD_LEFT, A_HEAD_RIGHT, A_FIRE, A_WEAPON1, A_WEAPON2, A_WEAPON3, A_WEAPON4,
      A_TARGET_PREV, A_TARGET_NEXT, A_STRAFE_MOD, A_BOUND, -1 },
};

/* Advanced's defaults that differ from the game's keys: action, slot, key */
static const short adv_defaults[][3] = {
    { A_BACK, 0, SDL_SCANCODE_S },
    { A_FORWARD_SLOW, 0, SDL_SCANCODE_Z },
    { A_LOOK_UP, 0, NONE },
    { A_LOOK_DOWN, 0, NONE },
    { A_HEAD_LEFT, 0, NONE },
    { A_HEAD_RIGHT, 0, NONE },
    { A_TURN_LEFT, 0, SDL_SCANCODE_Q },   /* (A and D strafe, as most games have it) */
    { A_TURN_RIGHT, 0, SDL_SCANCODE_E },
    { A_STRAFE_LEFT, 0, SDL_SCANCODE_A },
    { A_STRAFE_RIGHT, 0, SDL_SCANCODE_D },
    { A_BOUND, 0, NONE },
    { A_MOUSELOOK, 0, PORT_KEY_MOUSE_MIDDLE },
    { A_MOUSELOOK_HOLD, 0, SDL_SCANCODE_LSHIFT },
    { A_GIMBAL, 0, SDL_SCANCODE_X },
    { A_ZOOM_IN, 1, PORT_KEY_WHEEL_UP },
    { A_ZOOM_OUT, 1, PORT_KEY_WHEEL_DOWN },
};

static int list[NSTYLE][NACT], nlist[NSTYLE];   /* each style's actions in their shown order */
static int defkey[NSTYLE][NACT][2];
static int bound[NSTYLE][NACT][2];      /* the physical key of each slot (NONE: no key) */
static short managed[PORT_KEY_COUNT];   /* 1: a game key some slot stands for */
static short xlat[PORT_KEY_COUNT];      /* physical key -> game key, 0 = nothing, -1 = itself */
static volatile uint64_t mission_seen_us;
static int built = -1;                  /* the style xlat was built for */

static int style(void) { return port_mouselook ? 1 : 0; }

static int in_style(int st, int a)
{
    return st == 1 || (a != A_MOUSELOOK && a != A_MOUSELOOK_HOLD && a != A_GIMBAL);
}

static void make_lists(void)
{
    int st, i, a, seen[NACT];
    if (nlist[0])
        return;
    for (st = 0; st < NSTYLE; st++) {
        memset(seen, 0, sizeof seen);
        for (i = 0; order_head[st][i] >= 0; i++) {
            list[st][nlist[st]++] = order_head[st][i];
            seen[order_head[st][i]] = 1;
        }
        for (a = 0; a < NACT; a++)
            if (!seen[a] && in_style(st, a))
                list[st][nlist[st]++] = a;
        for (a = 0; a < NACT; a++)
            for (i = 0; i < 2; i++) {
                int g = actions[a].game[i];
                defkey[st][a][i] = g == NONE || g >= PORT_KEY_MOUSELOOK || !in_style(st, a) ? NONE : g;
            }
    }
    for (i = 0; i < (int)(sizeof adv_defaults / sizeof adv_defaults[0]); i++)
        defkey[1][adv_defaults[i][0]][adv_defaults[i][1]] = adv_defaults[i][2];
}

int port_keys_count(void) { make_lists(); return nlist[style()]; }
const char *port_keys_label(int i)
{
    int a = list[style()][i];
    return a == A_LOOK_AHEAD && style() == 1 ? "Align body to headview" : actions[a].label;
}
int port_keys_get(int i, int slot) { return bound[style()][list[style()][i]][slot]; }
int port_keys_slot_used(int i, int s) { return actions[list[style()][i]].game[s] != NONE; }

/* help.c: the key in slot `slot` (0, 1) of the action named `name` in the current controls
 * style, or with slot -1 the first one it has; NONE (0) when there is none */
int port_keys_bound(const char *name, int slot)
{
    int a, st = style();
    make_lists();
    for (a = 0; a < NACT; a++)
        if (!strcmp(actions[a].name, name)) {
            if (!in_style(st, a))
                return NONE;
            if (slot >= 0)
                return slot < 2 ? bound[st][a][slot] : NONE;
            return bound[st][a][0] != NONE ? bound[st][a][0] : bound[st][a][1];
        }
    return NONE;
}

const char *port_keys_name(int k)
{
    switch (k) {
    case NONE: return "none";
    case PORT_KEY_MOUSE_MIDDLE: return "MMB";
    case PORT_KEY_MOUSE_4: return "Mouse 4";
    case PORT_KEY_MOUSE_5: return "Mouse 5";
    case PORT_KEY_WHEEL_UP: return "Wheel Up";
    case PORT_KEY_WHEEL_DOWN: return "Wheel Down";
    default: {
        const char *n = k > 0 && k < SDL_SCANCODE_COUNT ? SDL_GetScancodeName((SDL_Scancode)k) : "";
        return n && n[0] ? n : "?";
    }
    }
}

static int key_from_name(const char *s)
{
    if (!SDL_strcasecmp(s, "none") || !s[0]) return NONE;
    if (!SDL_strcasecmp(s, "MMB") || !SDL_strcasecmp(s, "Mouse Middle")) return PORT_KEY_MOUSE_MIDDLE;
    if (!SDL_strcasecmp(s, "Mouse 4")) return PORT_KEY_MOUSE_4;
    if (!SDL_strcasecmp(s, "Mouse 5")) return PORT_KEY_MOUSE_5;
    if (!SDL_strcasecmp(s, "Wheel Up")) return PORT_KEY_WHEEL_UP;
    if (!SDL_strcasecmp(s, "Wheel Down")) return PORT_KEY_WHEEL_DOWN;
    return (int)SDL_GetScancodeFromName(s);
}

static void rebuild(void)
{
    int a, s, k, st = style();

    memset(managed, 0, sizeof managed);
    for (k = 0; k < PORT_KEY_COUNT; k++)
        xlat[k] = -1;
    for (a = 0; a < NACT; a++)
        for (s = 0; s < 2; s++) {
            int g = actions[a].game[s];
            if (g != NONE && g < PORT_KEY_MOUSELOOK)
                managed[g] = 1;
        }
    managed[PORT_KEY_MOUSE_MIDDLE] = 1;
    /* a key that some slot used to have but no longer has does nothing in missions */
    for (k = 0; k < PORT_KEY_COUNT; k++)
        if (managed[k])
            xlat[k] = 0;
    for (a = 0; a < NACT; a++)
        for (s = 0; s < 2; s++)
            if (actions[a].game[s] != NONE && bound[st][a][s] != NONE && in_style(st, a))
                xlat[bound[st][a][s]] = (short)actions[a].game[s];
    built = st;
}

/* the selected style's keys back to its defaults */
void port_keys_reset(void)
{
    make_lists();
    memcpy(bound[style()], defkey[style()], sizeof bound[0]);
    rebuild();
}

/* bind slot s of list entry i to physical key k; the slot that had k gets this slot's old key;
 * returns the list entry that gave its key up, or -1 */
int port_keys_set(int i, int s, int k)
{
    int st = style(), a = list[st][i], j, t, old = bound[st][a][s], swapped = -1;

    if (actions[a].game[s] == NONE)
        return -1;
    if (k != NONE)
        for (j = 0; j < nlist[st]; j++)
            for (t = 0; t < 2; t++) {
                int b = list[st][j];
                if (bound[st][b][t] == k && !(b == a && t == s)) {
                    bound[st][b][t] = old;
                    swapped = j;
                }
            }
    bound[st][a][s] = k;
    rebuild();
    return swapped;
}

static const char *prefix[NSTYLE] = { "key.", "advkey." };

static int known(const char *s)
{
    return !SDL_strcasecmp(s, "none") || key_from_name(s) != NONE;
}

/* "<key>,<key>": cut at the comma between the two names (P133: the first comma was taken, so the
 * comma key ",", "Keypad ," and anything after them came back as none); the second name or NULL */
static char *split_keys(char *v)
{
    char *p;
    for (p = strchr(v + 1, ','); p; p = strchr(p + 1, ',')) {
        int ok;
        *p = 0;
        ok = known(v) && known(p + 1);
        if (ok)
            return p + 1;
        *p = ',';
    }
    if (known(v) || (p = strchr(v, ',')) == NULL)
        return NULL;                    /* one key */
    *p = 0;                             /* (a name not known: as before) */
    return p + 1;
}

void port_keys_load(void (*get)(const char *key, char *out, int size))
{
    int a, st;
    make_lists();
    memcpy(bound, defkey, sizeof bound);
    for (st = 0; st < NSTYLE; st++)
        for (a = 0; a < NACT; a++) {
            char key[64], v[128] = "", *comma;
            if (!in_style(st, a))
                continue;
            snprintf(key, sizeof key, "%s%s", prefix[st], actions[a].name);
            get(key, v, sizeof v);
            if (!v[0])
                continue;
            comma = split_keys(v);
            bound[st][a][0] = actions[a].game[0] == NONE ? NONE : key_from_name(v);
            if (comma && actions[a].game[1] != NONE)
                bound[st][a][1] = key_from_name(comma);
        }
    rebuild();
}

void port_keys_save(FILE *f)
{
    int a, st;
    for (st = 0; st < NSTYLE; st++)
        for (a = 0; a < NACT; a++) {
            if (!in_style(st, a) || (bound[st][a][0] == defkey[st][a][0] && bound[st][a][1] == defkey[st][a][1]))
                continue;
            fprintf(f, "%s%s=%s", prefix[st], actions[a].name, port_keys_name(bound[st][a][0]));
            if (actions[a].game[1] != NONE)
                fprintf(f, ",%s", port_keys_name(bound[st][a][1]));
            fprintf(f, "\n");
        }
}

/* CtrlApplySlots runs (control.c via port_mouselook_take): a mission's controls are live */
void port_keys_mission_frame(void)
{
    mission_seen_us = port_now_us();
}

uint64_t port_keys_mission_seen(void)
{
    return mission_seen_us;
}

int port_keys_in_mission(void)
{
    uint64_t t = mission_seen_us;
    return t && port_now_us() - t < 250000;
}

/* physical key -> the game key to send: outside missions a key is itself (a mouse button
 * nothing); in missions the bound game key, a Freefall action (PORT_KEY_MOUSELOOK...), itself (keys no action has) or
 * NONE (the key does nothing) */
extern long game_paused_menu;           /* paused with the pause key (src/game/main/playkey.c) */
extern unsigned char game_in_mission;

int port_keys_translate(int k)
{
    int g;
    if (k <= 0 || k >= PORT_KEY_COUNT)
        return NONE;
    if (!port_keys_in_mission()) {
        /* P139: paused with the pause key, the mission's controls stand still, so it no longer
         * counted as in a mission: a key of the player's own for Pause paused the game but did not
         * unpause it.  It goes on being the game's P then (the game takes no other key) */
        if (game_paused_menu && game_in_mission && !port_game_dialog_up && !port_game_options_up) {
            if (built != style())
                rebuild();
            if (xlat[k] == SDL_SCANCODE_P)
                return SDL_SCANCODE_P;
        }
        return k >= SDL_SCANCODE_COUNT ? NONE : k;
    }
    if (built != style())
        rebuild();                      /* the controls style changed */
    g = xlat[k];
    return g < 0 ? (k >= SDL_SCANCODE_COUNT ? NONE : k) : g;
}
