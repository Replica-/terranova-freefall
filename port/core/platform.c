/* platform.c -- SDL3 platform layer: window and display, events, the game thread, the
 * interrupt lock and the I/O ports the game touches.  See platform.h for the overall model. */
#include <SDL3/SDL.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include "cockpitpack.h"
#include "arms.h"
#include "keys.h"
#include "port.h"
#include "screen.h"
#include "sharp3d.h"
#include "version.h"
#include "videosmooth.h"
#include "mapedge.h"
#include "helmetfx.h"

port_video_state port_video;
volatile int port_quit_requested;

/* options.c */
int port_options_event(const SDL_Event *e);
void port_options_render(SDL_Renderer *r, const SDL_FRect *view);
void port_options_renderer_gone(void);    /* before the renderer is destroyed (the logo) */
int port_missions_event(const SDL_Event *e);    /* the Mission Selector (missions.c) */
void port_missions_render(SDL_Renderer *r, const SDL_FRect *view);
void port_missions_renderer_gone(void);
extern int port_missions_open, port_ms_fading;
void port_help_renderer_gone(void);       /* ... help.c's pictures */
int port_help_present(SDL_Renderer *r, int ww, int wh, const SDL_FRect *shown, int w, int ex, int stretched);

static SDL_Mutex *irq_mutex;
static __thread int irq_off;            /* this thread holds the interrupt lock */
static uint64_t t0;

/* ------------------------------------------------------------------ interrupt lock */
void port_cli(void)
{
    if (!irq_off) {
        SDL_LockMutex(irq_mutex);
        irq_off = 1;
    }
}

void port_sti(void)
{
    if (irq_off) {
        irq_off = 0;
        SDL_UnlockMutex(irq_mutex);
    }
}

/* cli that remembers whether interrupts were already off (pushf / cli ... popf) */
int port_irq_save(void)
{
    int was = irq_off;
    port_cli();
    return was;
}

void port_irq_restore(int was)
{
    if (!was)
        port_sti();
}

void port_irq_enter(void) { port_cli(); }
void port_irq_leave(void) { port_sti(); }

/* ------------------------------------------------------------------ pausing the game thread
 * Windows: SuspendThread.  Elsewhere: a signal makes the game thread wait in its handler. */
#ifdef _WIN32
#include <windows.h>
static HANDLE game_thread_handle;
static DWORD game_thread_tid;

static void note_game_thread(void)
{
    game_thread_tid = GetCurrentThreadId();
    game_thread_handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, game_thread_tid);
}

static int paused;

void port_game_pause(void)
{
    CONTEXT c;
    if (!game_thread_handle || GetCurrentThreadId() == game_thread_tid)
        return;
    if (SuspendThread(game_thread_handle) == (DWORD)-1)
        return;
    c.ContextFlags = CONTEXT_CONTROL;
    GetThreadContext(game_thread_handle, &c);   /* returns once the thread has really stopped */
    paused = 1;
}

void port_game_resume(void)
{
    if (paused) {
        paused = 0;
        ResumeThread(game_thread_handle);
    }
}
#else
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
static pthread_t game_tid;
static volatile int game_tid_set;
static sem_t park_in, park_out;
static int paused;

static void park(int sig)
{
    int e = errno;
    (void)sig;
    sem_post(&park_in);
    while (sem_wait(&park_out) != 0 && errno == EINTR)
        ;
    errno = e;
}

static void note_game_thread(void)
{
    struct sigaction sa;
    sem_init(&park_in, 0, 0);
    sem_init(&park_out, 0, 0);
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = park;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, NULL);
    game_tid = pthread_self();
    game_tid_set = 1;
    {
        void port_lockstep_game_thread(void);
        port_lockstep_game_thread();    /* frame comparisons: its own heap (lockstep.c) */
    }
}

void port_game_pause(void)
{
    if (!game_tid_set || pthread_equal(pthread_self(), game_tid))
        return;
    if (pthread_kill(game_tid, SIGUSR2) != 0)
        return;
    while (sem_wait(&park_in) != 0 && errno == EINTR)
        ;
    paused = 1;
}

void port_game_resume(void)
{
    if (paused) {
        paused = 0;
        sem_post(&park_out);
    }
}
#endif

/* ------------------------------------------------------------------ time */
uint64_t port_now_us(void)
{
    return SDL_GetTicksNS() / 1000 - t0;
}

void port_sleep_us(uint64_t us)
{
    SDL_DelayPrecise(us * 1000);
}

/* ------------------------------------------------------------------ frame rate
 * The game's frame loop runs as fast as it can.  On the PCs of 1996 that was 15-30 frames a
 * second, and the game relies on it: a projectile checks for units only over the distance it
 * moved in one frame, rounded down to whole steps, so above about 30 frames a second shots
 * never hit anyone.  GameTimeFrame calls this once per frame to keep to port_frame_us (in microseconds, so 60 a
 * second is 16667 us and not 16 ms, which ran 62.5 frames against a 60 Hz display: P22). */
int port_frame_us = 40000;              /* 25 frames a second; 0 = no limit (--fps) */
static volatile unsigned long game_frames;   /* frames the game finished (the FPS meter) */

/* the display's refreshes (P57, P59): their length from the display's mode (else the presents'
 * average), counted by time as each present returns (vsync), so that the game's frames can start
 * on them.  A present that missed refreshes (the window's work for a new picture took longer than
 * one, at 240 Hz and 4K) counts each refresh it missed: counting the presents, a 240 Hz display
 * showing 180 to 210 a second matched no frame rate and the frames ran on their own clock */
static SDL_Mutex *disp_mutex;
static SDL_Condition *disp_cond;
static unsigned long disp_ticks;
static uint64_t disp_phase;             /* when the last refresh counted began */
static volatile uint64_t disp_avg_us;   /* the presents' average length, 0: not known yet */
static volatile double disp_per_us;     /* a refresh's length from the display's mode, 0: none */

static void display_refreshed(double hz)
{
    static uint64_t last;
    uint64_t now = port_now_us(), d = now - last, a = disp_avg_us;
    double per = hz >= 30 && hz <= 1000 ? 1e6 / hz : 0;
    unsigned long k = 1;
    if (last && d > 0 && d < 100000)
        disp_avg_us = a ? (a * 15 + d) / 16 : d;
    SDL_LockMutex(disp_mutex);
    disp_per_us = per;
    if (per > 0 && last && d < 100000 && now > disp_phase) {
        /* the refreshes since the last one counted (each present returns as one begins); the
         * refreshes' start followed slowly, as the returns come a little after it */
        double n = (now - disp_phase) / per;
        k = n < 1.5 ? 1 : (unsigned long)(n + 0.5);
        disp_phase += (uint64_t)(k * per);
        disp_phase += (int64_t)(now - disp_phase) / 8;
    } else
        disp_phase = now;
    last = now;
    disp_ticks += k;
    SDL_BroadcastCondition(disp_cond);
    SDL_UnlockMutex(disp_mutex);
}

/* refreshes per game frame when the display's rate is a whole number of times the frame rate
 * (60, 120, 180, 240 ... Hz for 60 frames a second, to 2.5%) and its presents keep to the
 * refreshes, else 0 (no vsync or a variable refresh: the presents come faster) */
static int refreshes_per_frame(void)
{
    uint64_t a = disp_avg_us;
    double per = disp_per_us;
    long n, d;
    if (!a || port_frame_us <= 0)
        return 0;
    if (per <= 0)
        per = (double)a;
    else if (a < per * 0.75)
        return 0;
    n = (long)(port_frame_us / per + 0.5);
    d = (long)(n * per) - port_frame_us;
    if (n < 1 || n > 8 || (d < 0 ? -d : d) * 40 > port_frame_us)
        return 0;
    return (int)n;
}
/* a mission stands still for the options screen (options.c: its pointer) */
volatile int port_options_held;

/* 1 when --fps asks for more than the original's 25 frames a second: the projectile hit test
 * then counts the last part step too (src/edms/ff_ftl.c; docs/BUGS.md G4) */
int port_highfps_fix;

void port_frame_limit(void)
{
    static uint64_t next, seen_last;
    uint64_t now, seen = port_keys_mission_seen();
    int mission_frame = seen != seen_last;  /* the mission's controls ran in this frame */

    seen_last = seen;
    game_frames++;
    port_game_options_apply();          /* the options screen's sound settings (options.c) */
    port_pf_frame();                    /* the 3D view's copy of the map (mapedge.c) */
    if (port_lockstep) {                /* frame comparison runs: no real time (lockstep.c) */
        port_lockstep_frame();
        return;
    }
    if (port_options_open) {            /* the options screen: the mission stands still */
        extern unsigned long tn_ticks, game_last_tick;
        unsigned long t = tn_ticks;
        /* not the menus: they run frames too (P140: also after a frame longer than the quarter
         * of a second port_keys_in_mission allows) */
        port_options_held = mission_frame || port_keys_in_mission();
        while (port_options_open && !port_quit_requested) {
            port_game_options_apply();  /* the sound settings at once, while it stands still */
            port_sleep_us(10000);
        }
        port_options_held = 0;
        game_last_tick += tn_ticks - t; /* the wait is not game time */
        next = 0;
    }
    now = port_now_us();
    if (port_frame_us > 0) {
        int n = refreshes_per_frame();
        if (n) {
            /* Frame rate 60 fps on a display at that rate: each frame starts as a refresh begins,
             * so every frame is shown once, for as long as the others (P57).  Frames drawn on
             * their own clock drifted against the refreshes: one that took a little longer to
             * draw (by water, with True colour) missed its refresh, the last one stayed on the
             * screen twice and the next was never shown, so walking jerked */
            static unsigned long start_tick;
            uint64_t limit = (next ? next : now) + (uint64_t)port_frame_us / 2;
            double per;
            SDL_LockMutex(disp_mutex);
            per = disp_per_us;
            if (per > 0) {
                /* the refresh n after the last frame's: when it begins by the refreshes' clock
                 * (not when its present returns: a present that missed one returns late, P59) */
                long ahead = (long)(start_tick + (unsigned long)n - disp_ticks);
                uint64_t at = disp_phase + (uint64_t)(ahead > 0 ? ahead * per : 0);
                SDL_UnlockMutex(disp_mutex);
                if (ahead > 0 && ahead <= 2 * n && at > now) {
                    port_sleep_us((at < limit ? at : limit) - now);
                    start_tick += (unsigned long)n;
                } else {                /* late: from the refresh now */
                    SDL_LockMutex(disp_mutex);
                    int64_t since = (int64_t)(port_now_us() - disp_phase);
                    start_tick = disp_ticks + (unsigned long)(since > 0 ? since / per : 0);
                    SDL_UnlockMutex(disp_mutex);
                }
            } else {
                while ((long)(disp_ticks - (start_tick + (unsigned long)n)) < 0 && port_now_us() < limit)
                    SDL_WaitConditionTimeout(disp_cond, disp_mutex, 2);
                start_tick = disp_ticks;
                SDL_UnlockMutex(disp_mutex);
            }
            next = port_now_us() + (uint64_t)port_frame_us;
        } else {
            if (now < next)
                port_sleep_us(next - now);
            else
                next = now;
            next += (uint64_t)port_frame_us;
        }
    }
    {                                   /* the frame's time to the millisecond (timer.c, P55) */
        int was = port_irq_save();
        port_tick_sync();
        port_irq_restore(was);
    }
}

/* ------------------------------------------------------------------ video timing
 * VGA 320x200 / 320x400 / 640x400 modes refresh at 70 Hz, 640x480 and up at 60 Hz.  The vertical
 * retrace takes about 4% of the frame, the horizontal one about 20% of a line. */
static uint64_t frame_us(void)
{
    return port_video.h == 480 || port_video.h >= 600 || port_video.h == 240 ? 16683 : 14268;
}

int port_in_vretrace(void)
{
    uint64_t p = frame_us();
    return port_now_us() % p >= p - p / 25;
}

int port_in_hretrace(void)
{
    uint64_t line = frame_us() * 1000 / (port_video.h == 480 || port_video.h == 240 ? 525 : 449);
    return (port_now_us() * 1000) % line >= line - line / 5;
}

void port_video_set_mode(int w, int h, int pitch)
{
    port_video.w = w;
    port_video.h = h;
    port_video.pitch = pitch;
    port_video.start = 0;
}

/* ------------------------------------------------------------------ I/O ports */
static uint8_t kb_latch;                /* last byte of the keyboard controller (port 60h) */
static int dac_windex, dac_rindex, dac_wcomp, dac_rcomp;
extern uint8_t port_joy_port(void);     /* joy.c: game port 201h */
extern void port_joy_trigger(void);

uint8_t port_inb(int port)
{
    switch (port & 0xFFFF) {
    case 0x60:
        return kb_latch;
    case 0x64:
        return 0;                       /* controller ready, input buffer empty */
    case 0x201:
        return port_joy_port();
    case 0x3C9: {
        uint8_t v = port_video.dac[dac_rindex * 3 + dac_rcomp];
        if (++dac_rcomp == 3) {
            dac_rcomp = 0;
            dac_rindex = (dac_rindex + 1) & 0xFF;
        }
        return v;
    }
    case 0x3DA:
    case 0x3BA:
        if (port_lockstep) {            /* frame comparison: as the emulator's VGA, every read toggles */
            static uint8_t st;
            return st ^= 9;
        }
        return (port_in_vretrace() ? 9 : 0) | (port_in_hretrace() ? 1 : 0);
    case 0x40:
    case 0x42:
        if (port_lockstep) {            /* as the emulator's PIT: counts down 0x100 per read */
            static uint16_t cnt[2];
            static uint8_t hi[2];
            int k = (port & 0xFFFF) == 0x42, h = hi[k];
            cnt[k] -= 0x100;
            hi[k] ^= 1;
            return h ? cnt[k] >> 8 : cnt[k] & 0xFF;
        }
        return 0;
    }
    return 0;
}

void port_outb(int port, int v)
{
    switch (port & 0xFFFF) {
    case 0x201:
        port_joy_trigger();
        break;
    case 0x3C7:
        dac_rindex = v & 0xFF;
        dac_rcomp = 0;
        break;
    case 0x3C8:
        dac_windex = v & 0xFF;
        dac_wcomp = 0;
        break;
    case 0x3C9:
        port_video.dac[dac_windex * 3 + dac_wcomp] = v & 0x3F;
        if (++dac_wcomp == 3) {
            dac_wcomp = 0;
            dac_windex = (dac_windex + 1) & 0xFF;
            port_video.pal_serial++;
        }
        break;
    }
    /* 0x20 (PIC end of interrupt), 0x60 (keyboard commands), 0x3C4 / 0x3CE / 0x3D4 (sequencer,
     * graphics controller, CRTC of the VGA / Mode X driver) need nothing here */
}

void port_outw(int port, int v)
{
    port_outb(port, v & 0xFF);
    port_outb(port + 1, (v >> 8) & 0xFF);
}

/* ------------------------------------------------------------------ keyboard
 * SDL scan codes to PC/AT scan code set 1 (what the IRQ1 handler reads from port 60h).
 * 0x100 = E0-prefixed. */
static const unsigned short set1[SDL_SCANCODE_COUNT] = {
    [SDL_SCANCODE_ESCAPE] = 0x01, [SDL_SCANCODE_1] = 0x02, [SDL_SCANCODE_2] = 0x03,
    [SDL_SCANCODE_3] = 0x04, [SDL_SCANCODE_4] = 0x05, [SDL_SCANCODE_5] = 0x06,
    [SDL_SCANCODE_6] = 0x07, [SDL_SCANCODE_7] = 0x08, [SDL_SCANCODE_8] = 0x09,
    [SDL_SCANCODE_9] = 0x0A, [SDL_SCANCODE_0] = 0x0B, [SDL_SCANCODE_MINUS] = 0x0C,
    [SDL_SCANCODE_EQUALS] = 0x0D, [SDL_SCANCODE_BACKSPACE] = 0x0E, [SDL_SCANCODE_TAB] = 0x0F,
    [SDL_SCANCODE_Q] = 0x10, [SDL_SCANCODE_W] = 0x11, [SDL_SCANCODE_E] = 0x12,
    [SDL_SCANCODE_R] = 0x13, [SDL_SCANCODE_T] = 0x14, [SDL_SCANCODE_Y] = 0x15,
    [SDL_SCANCODE_U] = 0x16, [SDL_SCANCODE_I] = 0x17, [SDL_SCANCODE_O] = 0x18,
    [SDL_SCANCODE_P] = 0x19, [SDL_SCANCODE_LEFTBRACKET] = 0x1A, [SDL_SCANCODE_RIGHTBRACKET] = 0x1B,
    [SDL_SCANCODE_RETURN] = 0x1C, [SDL_SCANCODE_LCTRL] = 0x1D, [SDL_SCANCODE_A] = 0x1E,
    [SDL_SCANCODE_S] = 0x1F, [SDL_SCANCODE_D] = 0x20, [SDL_SCANCODE_F] = 0x21,
    [SDL_SCANCODE_G] = 0x22, [SDL_SCANCODE_H] = 0x23, [SDL_SCANCODE_J] = 0x24,
    [SDL_SCANCODE_K] = 0x25, [SDL_SCANCODE_L] = 0x26, [SDL_SCANCODE_SEMICOLON] = 0x27,
    [SDL_SCANCODE_APOSTROPHE] = 0x28, [SDL_SCANCODE_GRAVE] = 0x29, [SDL_SCANCODE_LSHIFT] = 0x2A,
    [SDL_SCANCODE_BACKSLASH] = 0x2B, [SDL_SCANCODE_Z] = 0x2C, [SDL_SCANCODE_X] = 0x2D,
    [SDL_SCANCODE_C] = 0x2E, [SDL_SCANCODE_V] = 0x2F, [SDL_SCANCODE_B] = 0x30,
    [SDL_SCANCODE_N] = 0x31, [SDL_SCANCODE_M] = 0x32, [SDL_SCANCODE_COMMA] = 0x33,
    [SDL_SCANCODE_PERIOD] = 0x34, [SDL_SCANCODE_SLASH] = 0x35, [SDL_SCANCODE_RSHIFT] = 0x36,
    [SDL_SCANCODE_KP_MULTIPLY] = 0x37, [SDL_SCANCODE_LALT] = 0x38, [SDL_SCANCODE_SPACE] = 0x39,
    [SDL_SCANCODE_CAPSLOCK] = 0x3A, [SDL_SCANCODE_F1] = 0x3B, [SDL_SCANCODE_F2] = 0x3C,
    [SDL_SCANCODE_F3] = 0x3D, [SDL_SCANCODE_F4] = 0x3E, [SDL_SCANCODE_F5] = 0x3F,
    [SDL_SCANCODE_F6] = 0x40, [SDL_SCANCODE_F7] = 0x41, [SDL_SCANCODE_F8] = 0x42,
    [SDL_SCANCODE_F9] = 0x43, [SDL_SCANCODE_F10] = 0x44, [SDL_SCANCODE_NUMLOCKCLEAR] = 0x45,
    [SDL_SCANCODE_SCROLLLOCK] = 0x46, [SDL_SCANCODE_KP_7] = 0x47, [SDL_SCANCODE_KP_8] = 0x48,
    [SDL_SCANCODE_KP_9] = 0x49, [SDL_SCANCODE_KP_MINUS] = 0x4A, [SDL_SCANCODE_KP_4] = 0x4B,
    [SDL_SCANCODE_KP_5] = 0x4C, [SDL_SCANCODE_KP_6] = 0x4D, [SDL_SCANCODE_KP_PLUS] = 0x4E,
    [SDL_SCANCODE_KP_1] = 0x4F, [SDL_SCANCODE_KP_2] = 0x50, [SDL_SCANCODE_KP_3] = 0x51,
    [SDL_SCANCODE_KP_0] = 0x52, [SDL_SCANCODE_KP_PERIOD] = 0x53, [SDL_SCANCODE_NONUSBACKSLASH] = 0x56,
    [SDL_SCANCODE_F11] = 0x57, [SDL_SCANCODE_F12] = 0x58,
    [SDL_SCANCODE_KP_ENTER] = 0x11C, [SDL_SCANCODE_RCTRL] = 0x11D, [SDL_SCANCODE_KP_DIVIDE] = 0x135,
    [SDL_SCANCODE_PRINTSCREEN] = 0x137, [SDL_SCANCODE_RALT] = 0x138, [SDL_SCANCODE_HOME] = 0x147,
    [SDL_SCANCODE_UP] = 0x148, [SDL_SCANCODE_PAGEUP] = 0x149, [SDL_SCANCODE_LEFT] = 0x14B,
    [SDL_SCANCODE_RIGHT] = 0x14D, [SDL_SCANCODE_END] = 0x14F, [SDL_SCANCODE_DOWN] = 0x150,
    [SDL_SCANCODE_PAGEDOWN] = 0x151, [SDL_SCANCODE_INSERT] = 0x152, [SDL_SCANCODE_DELETE] = 0x153,
    [SDL_SCANCODE_LGUI] = 0x15B, [SDL_SCANCODE_RGUI] = 0x15C, [SDL_SCANCODE_APPLICATION] = 0x15D,
};

extern int kb_isr_body(unsigned long *frame);
extern unsigned long kb_conv_linear;           /* nonzero once the game started its keyboard handler */

void port_kb_scancode(uint8_t sc)
{
    unsigned long frame[3] = { 0, 0, 0x202 };

    port_irq_enter();
    kb_latch = sc;
    if (kb_conv_linear)
        kb_isr_body(frame);
    port_irq_leave();
}

static uint8_t keys_down[PORT_KEY_COUNT];   /* physical keys (and mouse buttons) held */
static short keys_sent[PORT_KEY_COUNT];     /* what each held one stands for (keys.c) */
static void ml_toggle(void);

/* a physical key (or a bindable mouse button) goes down or up: the game gets the original key
 * it is bound to (keys.c; outside missions, or with the game's keys, the key itself) */
static int ml_shift;                    /* Mouselook (hold) held: the pointer is free */
int port_gimbal;                        /* gimbal mode: the body keeps turning to the view */
static int ml_shift_mission(void);
static void ml_update(void);

static void phys_event(int k, int down)
{
    unsigned c;
    int g;

    if ((unsigned)k >= PORT_KEY_COUNT)
        return;
    if (!down && !keys_down[k])
        return;                         /* released already (options_toggled) */
    if (down && !keys_down[k])
        keys_sent[k] = (short)(port_lockstep ? (k < SDL_SCANCODE_COUNT ? k : 0) : port_keys_translate(k));
    keys_down[k] = (uint8_t)down;
    g = keys_sent[k];
    if (g == PORT_KEY_MOUSELOOK) {
        if (down)
            ml_toggle();
        return;
    }
    if (g == PORT_KEY_MOUSELOOK_HOLD) {
        ml_shift = down && ml_shift_mission();
        ml_update();
        return;
    }
    if (g == PORT_KEY_GIMBAL) {
        if (down)
            port_gimbal = !port_gimbal;
        return;
    }
    if (g == PORT_KEY_SHOW_FPS) {
        if (down)
            port_show_fps = !port_show_fps;
        return;
    }
    if (g <= 0 || g >= SDL_SCANCODE_COUNT || !(c = set1[g]))
        return;
    if (c & 0x100)
        port_kb_scancode(0xE0);
    port_kb_scancode((c & 0x7F) | (down ? 0 : 0x80));
}

/* the wheel: each notch presses its key for a moment, one after another */
static int wheel_queue[2];
static uint64_t wheel_up_at;
static int wheel_down_key;

static void wheel_notch(int up)
{
    int k = up ? PORT_KEY_WHEEL_UP : PORT_KEY_WHEEL_DOWN;
    if (port_keys_translate(k) != 0 && wheel_queue[!up] < 8)
        wheel_queue[!up]++;
}

static void wheel_tick(void)
{
    uint64_t now = port_now_us();
    int i;
    if (wheel_down_key) {
        if (now < wheel_up_at)
            return;
        phys_event(wheel_down_key, 0);
        wheel_down_key = 0;
        wheel_up_at = now + 30000;      /* a gap, so the next notch is a new press */
        return;
    }
    if (now < wheel_up_at)
        return;
    for (i = 0; i < 2; i++)
        if (wheel_queue[i]) {
            wheel_queue[i]--;
            wheel_down_key = i ? PORT_KEY_WHEEL_DOWN : PORT_KEY_WHEEL_UP;
            phys_event(wheel_down_key, 1);
            wheel_up_at = now + 60000;  /* held over at least one game frame */
            return;
        }
}

static void key_event(SDL_Scancode s, int down)
{
    if (s == SDL_SCANCODE_PAUSE) {
        if (down) {
            static const uint8_t seq[6] = { 0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5 };
            int i;
            for (i = 0; i < 6; i++)
                port_kb_scancode(seq[i]);
        }
        return;
    }
    phys_event(s, down);
}

/* the game's own key pressed and let go (options.c: its GAME OPTIONS box presses O), past the
 * key bindings: with O bound to an action the box pressed that action (P131) */
void port_key_tap(SDL_Scancode s)
{
    unsigned c = (unsigned)s < SDL_SCANCODE_COUNT ? set1[s] : 0;
    int up;
    /* Alt let go first: Alt+O taps O with Alt still held, and the game took Alt+O (not its O) and
     * opened nothing (owner's test) */
    port_kb_scancode(0xB8);
    port_kb_scancode(0xE0);
    port_kb_scancode(0xB8);
    for (up = 0; c && up < 2; up++) {
        if (c & 0x100)
            port_kb_scancode(0xE0);
        port_kb_scancode((uint8_t)((c & 0x7F) | (up ? 0x80 : 0)));
    }
}

/* ------------------------------------------------------------------ display */
static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static int tex_w, tex_h;
static SDL_Texture *menu_tex;           /* "Widescreen": the main menu 854 wide (menuwide.c) */
static SDL_Texture *s3d_tex[PORT_S3D_SLOTS], *s3d_target;   /* the sharp 3D views, the screen with them */
static int s3d_tw[PORT_S3D_SLOTS], s3d_th[PORT_S3D_SLOTS], s3d_gw, s3d_gh;
static struct { unsigned serial, pal; int cx0, cy0, cx1, cy1; } s3d_made[PORT_S3D_SLOTS];  /* in s3d_tex */
/* "Anti-aliasing: FXAA" (screen.c port_screen_fxaa): the pictures have alpha, 254 where the 3D's
 * pixels are (disp3d) and 255 or 0 elsewhere (the HUD, the maps, the art), so that the pass changes
 * only the 3D; the cockpit's view goes over the stretched screen with its alpha (aa_blend) */
static int s3d_aa;
static SDL_BlendMode aa_blend(void)
{
    return SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_SRC_ALPHA, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                                      SDL_BLENDOPERATION_ADD, SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ZERO,
                                      SDL_BLENDOPERATION_ADD);
}
/* widescreen: the last wide picture's extra columns and the main view's rows (the mouse beside
 * the screen), the pointer drawn there and its bitmap */
static int wide_nudge;
static int ml_active;                   /* mouselook has the mouse (below) */
static SDL_Texture *wide_cur_tex;
#define BMT_MONO_ 1                     /* the game's bitmap types (2d.h) */
#define BMT_FLAT8_ 2
static const uint8_t *wide_cur_bits;
static int wide_cur_w, wide_cur_h, wide_cur_type, wide_cur_color;
/* widescreen, the mission's screen (sharp3d.h port_s3d_stretch): the screen stretched to cw of the
 * game's columns; the main view's wide picture is placed at xh0 of them, the game's own picture
 * in its middle (fe of its columns from its left).  Also for the mouse. */
static struct {
    int on, cw, kx, ky, view;           /* view: the main view's picture is in */
    int fx, fy, fw, fh, fe, xh0;        /* the main view on the game's screen, its picture */
    const uint8_t *win;                 /* fw x fh: 1 where the view's window is */
    int pack, pe;                       /* a cockpit pack's picture: the screen 1:1, pe columns in */
} ck;
static SDL_FRect ck_rect;               /* the stretched screen in the window */
static SDL_Texture *ck_target, *ck_tex;
static int ck_gw, ck_gh, ck_tw, ck_th;
static struct { unsigned serial, pal, side[PORT_S3D_SLOTS], pack; int xh0, cw; } ck_made;   /* in ck_tex */
static uint64_t ck_good_us;             /* when ck_target last had a cockpit pack's screen with the view */
static unsigned long ck_good_frame;     /* ... the game's frame then */
extern unsigned long game_frame;
/* ... "Cockpit pack" (cockpitpack.c): its picture for the screen up (ckp_on), laid over the screen
 * at its own pixels (ckp_tex, made for the screen ckp_scr); ck_dead: the pointer over the
 * picture's art beside the screen (ck_dead_u, _v: where), no clicks there */
static port_ckpic ckp;
static int ckp_on, ck_dead;
static float ck_dead_u, ck_dead_v;
static SDL_Texture *ckp_tex;
static int ckp_tw, ckp_th;
static struct {
    unsigned serial, pal, f;
    int xh0, ok;
    double hit;
} ckp_made;
/* ... "Helmet motion" with the layered helmet (ckp.parts): the frame (the screen, the picture,
 * what the HUD drew over the art, the goggles' screens) is made in ckf_target and drawn part by
 * part over the 3D (hlp_draw); the view's picture in three: the world (ck_tex: the 3D and the
 * reticle over it), the frame's part (ckv_tex: see-through where the world shows) and the
 * targets' layer (ckb_tex, over the world, behind the parts: P126) */
static SDL_Texture *ckf_target, *ckv_tex, *ckb_tex;
static void hlp_forget(void);
static unsigned renderer_gen;           /* counts make_renderer: a new renderer can get the old one's
                                         * address (P130) */
static int ckf_tw, ckf_th;
static uint8_t ckp_scr[320 * 400];
static uint64_t ck_cur_seen;            /* when the game last showed its pointer */
/* ... the cockpit's side screens' maps (sharp3d.c port_s3d_map): each at the right proportions
 * over its stretched screen, e more of its columns on each side, placed at xh0 of the stretched
 * screen's columns; the frame's screen copy and map picture (what the HUD drew over the map) */
static struct {
    int on, x, y, w, h, e, xh0;
    const uint8_t *scr, *low;
} ckm[PORT_S3D_SLOTS];
static SDL_Texture *ckm_tex[PORT_S3D_SLOTS];
static int ckm_tw[PORT_S3D_SLOTS], ckm_th[PORT_S3D_SLOTS];
static struct { unsigned serial, pal; int xh0, cw; } ckm_made[PORT_S3D_SLOTS];
static uint32_t pal32[256];             /* the game's palette (its screenshots) */
static uint32_t disp32[256];            /* ... with the screen filters' colour settings */
static uint32_t disp3d[256];            /* ... alpha 254: the 3D's pixels (Anti-aliasing) */

/* "Anti-aliasing": the target label's text drawn into the sharp picture (sharp3d.c) is not the
 * 3D's: alpha 255 there, so the pass leaves it sharp; d is row Y of the picture */
static void label_alpha(const port_s3d_frame *f, int Y, uint32_t *d)
{
    int i, X;
    for (i = 0; i < f->lab_n; i++) {
        const port_s3d_lab *l = &f->lab[i];
        const uint8_t *m;
        if (Y < l->y || Y >= l->y + l->h)
            continue;
        m = f->lab_m + l->m + (size_t)(Y - l->y) * l->w;
        for (X = 0; X < l->w; X++)
            if (m[X] && d[l->x + X] >> 24 == 0xFE)
                d[l->x + X] |= 0xFF000000u;
    }
}
static unsigned disp32_serial;          /* changes with disp32 */
static uint8_t tc_lut[256];             /* True colour: the colour settings per channel ... */
static int tc_sat = 100;                /* ... and the colour (%) */

/* a full colour of the 3D (True colour, port_s3d_frame tc) with the colour settings, alpha a */
static uint32_t tc_px(uint32_t v, uint32_t a)
{
    int r = tc_lut[v >> 16 & 0xFF], g = tc_lut[v >> 8 & 0xFF], b = tc_lut[v & 0xFF];
    if (tc_sat != 100) {
        int l = (77 * r + 150 * g + 29 * b) >> 8;
        r = l + (r - l) * tc_sat / 100;
        g = l + (g - l) * tc_sat / 100;
        b = l + (b - l) * tc_sat / 100;
        r = r < 0 ? 0 : r > 255 ? 255 : r;
        g = g < 0 ? 0 : g > 255 ? 255 : g;
        b = b < 0 ? 0 : b > 255 ? 255 : b;
    }
    return a | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}
/* P101: a picture's full colours follow the palette shown when it is not the one they were
 * worked out with (the game fades the palette to black at a mission's end without drawing the
 * view again: the clouds and the fog stayed bright while the rest went dark).  Per colour of the
 * game and channel: what the palette's change did to that colour (x 256), or -1 - the new value
 * where it was 0; each full-colour pixel takes its game's colour's change (tc_prep, tc_faded) */
static int tc_fading;
static int tc_fk[256][3];

static void tc_prep(const port_s3d_frame *f)
{
    int i, k, same = 1;
    tc_fading = 0;
    if (!f || !f->tc)
        return;
    for (i = 0; i < 256 && same; i++)
        same = (pal32[i] & 0xFFFFFFu) == (f->tc_pal[i] & 0xFFFFFFu);
    if (same)
        return;
    for (i = 0; i < 256; i++)
        for (k = 0; k < 3; k++) {
            int was = f->tc_pal[i] >> (16 - 8 * k) & 0xFF, now = pal32[i] >> (16 - 8 * k) & 0xFF;
            tc_fk[i][k] = was ? now * 256 / was : -1 - now;
        }
    tc_fading = 1;
}

static uint32_t tc_faded(uint32_t v, int c)
{
    int k, o = 0;
    if (!tc_fading)
        return v;
    for (k = 0; k < 3; k++) {
        int m = tc_fk[c][k], x = m < 0 ? -1 - m : (int)(v >> (16 - 8 * k) & 0xFF) * m >> 8;
        o |= (x > 255 ? 255 : x) << (16 - 8 * k);
    }
    return (v & 0xFF000000u) | (uint32_t)o;
}

static unsigned pal_seen = ~0u, grade_seen;
static int ir_seen;                     /* port_ir_on() at the last regrade */
static int renderer_gpu;                /* the renderer was made for the screen filters' shaders */
static SDL_FRect dst_rect;
int port_window_w, port_window_h;       /* the window's pixels at the last present (bench.c) */
int port_fullscreen;                    /* Fullscreen: options.c, tnfree.ini fullscreen=on, Alt+Enter */
static int fullscreen;                  /* the window's state; follows port_fullscreen (port_run) */
static int shot_window;                 /* FREEFALL_SHOT_WINDOW=1: every shot is the whole window */
static const char *shot_prefix;
static uint64_t shot_every, shot_next;
static int shot_count;
static SDL_AtomicInt sync_shot;         /* port_shot_window_sync: pictures to draw before the shot */
static char sync_shot_name[1100];

static void update_palette(void)
{
    int i, sh = port_video.dac_bits == 8 ? 0 : 2;
    for (i = 0; i < 256; i++) {
        uint32_t r = port_video.dac[i * 3] << sh, g = port_video.dac[i * 3 + 1] << sh,
                 b = port_video.dac[i * 3 + 2] << sh;
        if (sh) {                       /* 6-bit DAC: 63 -> 255 */
            r |= r >> 6;
            g |= g >> 6;
            b |= b >> 6;
        }
        pal32[i] = 0xFF000000u | r << 16 | g << 8 | b;
    }
}

static void save_shot(int w, int h)
{
    char name[1100];
    SDL_Surface *s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_XRGB8888);
    const uint8_t *src = port_video.vram + port_video.start;
    int x, y;

    if (!s)
        return;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            ((uint32_t *)((uint8_t *)s->pixels + y * s->pitch))[x] = pal32[src[y * port_video.pitch + x]];
    if (SDL_getenv("FREEFALL_SHOT_STATS")) {
        int used[256] = { 0 }, nu = 0, nd = 0, i;
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                used[src[y * port_video.pitch + x]]++;
        for (i = 0; i < 256; i++) {
            nu += used[i] != 0;
            nd += port_video.dac[i * 3] || port_video.dac[i * 3 + 1] || port_video.dac[i * 3 + 2];
        }
        port_log("shot %d: %dx%d start %u, %d colours used, %d non-black DAC entries\n", shot_count, w, h,
                 (unsigned)port_video.start, nu, nd);
    }
    snprintf(name, sizeof name, "%s%04d.bmp", shot_prefix, shot_count++);
    SDL_SaveBMP(s, name);
    SDL_DestroySurface(s);
}

/* SDL's "gpu" renderer (Direct3D 12 / Vulkan) while a screen filter needs shaders; otherwise,
 * or if it can't start, the system's default renderer as before.  Main thread. */
static void make_renderer(void)
{
    int gpu = port_screen_gpu_wanted(), x;

    if (texture)
        SDL_DestroyTexture(texture);
    texture = NULL;
    for (x = 0; x < PORT_S3D_SLOTS; x++) {
        if (s3d_tex[x])
            SDL_DestroyTexture(s3d_tex[x]);
        s3d_tex[x] = NULL;
    }
    if (s3d_target)
        SDL_DestroyTexture(s3d_target);
    s3d_target = NULL;
    if (wide_cur_tex)
        SDL_DestroyTexture(wide_cur_tex);
    wide_cur_tex = NULL;
    if (menu_tex)
        SDL_DestroyTexture(menu_tex);
    menu_tex = NULL;
    if (ck_target)
        SDL_DestroyTexture(ck_target);
    if (ck_tex)
        SDL_DestroyTexture(ck_tex);
    if (ckp_tex)
        SDL_DestroyTexture(ckp_tex);
    ck_target = ck_tex = ckp_tex = NULL;
    ckp_made.ok = 0;
    /* P130: the layered helmet's textures too (the renderer takes its textures with it, and a
     * texture of the new one could get the old one's address: the helmet's hidden art was written
     * into a smaller texture, a crash on a switch of preset) */
    if (ckf_target)
        SDL_DestroyTexture(ckf_target);
    if (ckv_tex)
        SDL_DestroyTexture(ckv_tex);
    if (ckb_tex)
        SDL_DestroyTexture(ckb_tex);
    ckf_target = ckv_tex = ckb_tex = NULL;
    hlp_forget();
    renderer_gen++;
    for (x = 0; x < PORT_S3D_SLOTS; x++) {
        if (ckm_tex[x])
            SDL_DestroyTexture(ckm_tex[x]);
        ckm_tex[x] = NULL;
    }
    port_screen_renderer_gone();
    port_options_renderer_gone();       /* the first-start question's logo */
    port_missions_renderer_gone();
    port_help_renderer_gone();
    if (renderer)
        SDL_DestroyRenderer(renderer);
    renderer = NULL;
    renderer_gpu = 0;
    if (gpu) {
        SDL_PropertiesID p = SDL_CreateProperties();
        SDL_SetPointerProperty(p, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, window);
        SDL_SetStringProperty(p, SDL_PROP_RENDERER_CREATE_NAME_STRING, "gpu");
        SDL_SetBooleanProperty(p, SDL_PROP_RENDERER_CREATE_GPU_SHADERS_SPIRV_BOOLEAN, true);
        SDL_SetBooleanProperty(p, SDL_PROP_RENDERER_CREATE_GPU_SHADERS_DXIL_BOOLEAN, true);
        renderer = SDL_CreateRendererWithProperties(p);
        SDL_DestroyProperties(p);
        if (!renderer) {
            port_log("The GPU renderer is not available (%s): screen filters that need shaders are off.\n",
                     SDL_GetError());
            port_screen_gpu_unavailable();
        }
    }
    if (!renderer)
        renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer)
        port_fatal("Cannot create the renderer: %s\n", SDL_GetError());
    port_log("Renderer: %s\n", SDL_GetRendererName(renderer));
    SDL_SetRenderVSync(renderer, 1);
    renderer_gpu = port_screen_gpu_wanted();    /* 0 after a failure: no new try */
}

/* the game's pointer bitmap as a texture (wide_cur_tex), 0 if none */
static int cursor_tex(int *pcw, int *pch, int *phx, int *phy)
{
    const uint8_t *bits;
    int cw, ch, row, type, hx, hy, color;
    if (!port_cursor_get(&bits, &cw, &ch, &row, &type, &hx, &hy, &color) ||
        (type != BMT_FLAT8_ && type != BMT_MONO_))
        return 0;
    if (!wide_cur_tex || bits != wide_cur_bits || cw != wide_cur_w || ch != wide_cur_h ||
        type != wide_cur_type || color != wide_cur_color) {
        uint32_t *px;
        int x, y;
        if (wide_cur_tex)
            SDL_DestroyTexture(wide_cur_tex);
        wide_cur_tex = NULL;
        px = SDL_malloc((size_t)cw * ch * 4);
        if (!px)
            return 0;
        for (y = 0; y < ch; y++)
            for (x = 0; x < cw; x++) {
                int v = type == BMT_FLAT8_ ? bits[y * row + x]
                        : (bits[y * row + x / 8] >> (7 - x % 8)) & 1 ? color : 0;
                px[y * cw + x] = v ? 0xFF000000u | (disp32[v] & 0xFFFFFF) : 0;
            }
        wide_cur_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, cw, ch);
        if (wide_cur_tex) {
            SDL_UpdateTexture(wide_cur_tex, NULL, px, cw * 4);
            SDL_SetTextureBlendMode(wide_cur_tex, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(wide_cur_tex, SDL_SCALEMODE_NEAREST);
        }
        SDL_free(px);
        wide_cur_bits = bits;
        wide_cur_w = cw;
        wide_cur_h = ch;
        wide_cur_type = type;
        wide_cur_color = color;
    }
    *pcw = cw;
    *pch = ch;
    *phx = hx;
    *phy = hy;
    return wide_cur_tex != NULL;
}

/* a side screen's map shows the game's pixel (gx, gy): its slot (0: none) */
static int ckm_at(int gx, int gy)
{
    int s;
    for (s = 1; s < PORT_S3D_SLOTS; s++) {
        int a = gx - ckm[s].x, b = gy - ckm[s].y;
        if (!ckm[s].on || a < 0 || a >= ckm[s].w || b < 0 || b >= ckm[s].h)
            continue;
        if (ckm[s].scr && ckm[s].scr[(size_t)b * ckm[s].w + a] != ckm[s].low[(size_t)b * ckm[s].w + a])
            return 0;                   /* the HUD's (stretched with the screen) */
        return s;
    }
    return 0;
}

/* the stretched screen: the column (of cw) where the game's pixel (gx, gy) is shown */
static float ck_col(int gx, int gy, int w)
{
    int s;
    if (ck.pack)                        /* a cockpit pack's picture: the screen 1:1 */
        return gx + ck.pe + 0.5f;
    if (ck.view && gy >= ck.fy && gy < ck.fy + ck.fh) {
        int a = gx - ck.fx;
        if (a >= 0 && a < ck.fw && ck.win[(size_t)(gy - ck.fy) * ck.fw + a])
            return ck.xh0 + ck.fe + a + 0.5f;
    }
    if ((s = ckm_at(gx, gy)) != 0)
        return ckm[s].xh0 + ckm[s].e + (gx - ckm[s].x) + 0.5f;
    return (gx + 0.5f) * ck.cw / w;
}

/* a new picture of the widescreen view laid into the window's colours (ck_compose), 16 rows a
 * part on the window's threads (port_parallel_display, P59) */
typedef struct {
    const port_s3d_frame *f, *sv[PORT_S3D_SLOTS];
    const uint8_t *src;
    void *pixels;
    const int *lsv, *l1;
    int pitch, tpitch, hw, hh, kx, ky, paused, ir;
    int pack, E, cw, xh0, sw;           /* a cockpit pack's picture (ckp): the screen 1:1, E in */
    void *pixf, *pixb;                  /* the layered helmet (ck_rows_one): the frame's part, the
                                         * targets' layer (pixels: the world) */
    int fpitch, bpitch;
    int eyk;                            /* the picture's rows above the view's (f->ey * ky) */
    int sh;                             /* the screen's rows */
    int lay;                            /* the layered helmet (its parts: ckp.parts) */
} CkRows;

static int covered(int b, const int *x0, const int *x1, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (b >= x0[i] && b < x1[i])
            return 1;
    return 0;
}

/* the layered helmet: 1 and the colour if the HUD drew over the game's 3D in the pack's window at
 * (X, Y) of the view's picture (the reticle, a HUD display in the helmet's art) */
static int hud_over_3d(const CkRows *rw, int X, int Y, uint8_t *out)
{
    const port_s3d_frame *f = rw->f;
    int Xs = X, Ys = Y, ly, sy, b, C, gx;
    const uint8_t *scr;
    if (Xs < 0 || Xs >= rw->hw || Ys < 0 || Ys >= rw->hh)
        return 0;
    ly = Ys / rw->ky;
    sy = f->y + ly;
    b = rw->l1[Xs];
    C = rw->xh0 + Xs / rw->kx;
    gx = C - rw->E;
    if (b < 0 || b >= f->w || C < 0 || C >= rw->cw || gx < 0 || gx >= rw->sw || !f->win[(size_t)ly * f->w + b] ||
        !ckp.win[(size_t)sy * rw->cw + C] ||
        (f->tip_x1 > f->tip_x0 && sy >= f->tip_y0 && sy <= f->tip_y1 && gx >= f->tip_x0 && gx <= f->tip_x1))
        return 0;
    scr = f->scr && !rw->paused ? f->scr + (size_t)ly * f->w : rw->src + (size_t)sy * rw->pitch + f->x;
    if (scr[b] == f->low[(size_t)ly * f->w + b] || scr[b] == ckp.art[(size_t)sy * rw->sw + f->x + b] ||
        (ckp.marks && ckp.marks[(size_t)sy * rw->sw + f->x + b]))
        return 0;                       /* (nor a damaged piece of the 4:3 frame: its cracks, P95) */
    *out = scr[b];
    return 1;
}

static void ck_rows(void *arg, int part, int thread)
{
    const CkRows *rw = arg;
    const port_s3d_frame *f = rw->f, *const *sv = rw->sv;
    const uint8_t *src = rw->src;
    const int *lsv = rw->lsv, *l1 = rw->l1;
    void *pixels = rw->pixels;
    int pitch = rw->pitch, tpitch = rw->tpitch, hw = rw->hw, kx = rw->kx, ky = rw->ky, paused = rw->paused;
    int y0 = part * 16, y1 = y0 + 16 < rw->hh ? y0 + 16 : rw->hh, X, Y, i;
    (void)thread;
    for (Y = y0; Y < y1; Y++) {
        int Yv = Y - rw->eyk, ly = Yv / ky, sy = f->y + ly;    /* (Yv: the view's row) */
        int tip = f->tip_x1 > f->tip_x0 && sy >= f->tip_y0 && sy <= f->tip_y1;
        const uint8_t *win = f->win + (size_t)ly * f->w;
        const uint8_t *scr = f->scr && !paused ? f->scr + (size_t)ly * f->w : src + (size_t)sy * pitch + f->x;
        const uint8_t *low = f->low + (size_t)ly * f->w;
        const uint8_t *hi = f->high + (ptrdiff_t)Yv * hw;
        const uint8_t *hud = f->hud ? f->hud + (size_t)ly * (hw / kx) : NULL;
        const uint8_t *hm = f->hudm ? f->hudm + (size_t)ly * f->w : NULL;
        const uint32_t *p3 = s3d_aa && !f->map ? disp3d : disp32;     /* the 3D's colours */
        const uint32_t *tc = f->tc && f->tc_rows[Yv] && !rw->ir ? f->tc + (ptrdiff_t)Yv * hw : NULL;
        uint32_t a3 = p3[0] & 0xFF000000u;
        uint32_t *d = (uint32_t *)((uint8_t *)pixels + (size_t)Y * tpitch);
        const port_s3d_frame *gr[PORT_S3D_SLOTS];
        int ng = 0, j, cx0[3], cx1[3], nc = 0;
        uint8_t hc;
        if (Yv < 0 || Yv >= f->h * ky) {     /* (rows the layered helmet's view has more) */
            SDL_memset(d, 0, (size_t)hw * 4);
            continue;
        }
        for (i = 0; i < f->cov_n; i++)  /* the goggles' screens on this row: the HUD's there */
            if (sy >= f->cov[i][1] && sy < f->cov[i][3]) {
                cx0[nc] = f->cov[i][0] - f->x;
                cx1[nc++] = f->cov[i][2] - f->x;
            }
        for (i = 1; i < PORT_S3D_SLOTS; i++)    /* the side screens on this row */
            if (sv[i] && sy >= sv[i]->y && sy < sv[i]->y + sv[i]->h)
                gr[ng++] = sv[i];
        for (X = 0; X < hw; X++) {
            int a = lsv[X], b = l1[X];
            const port_s3d_frame *g = NULL;
            for (j = 0; j < ng && !g; j++)
                if (b + f->x >= gr[j]->x && b + f->x < gr[j]->x + gr[j]->w)
                    g = gr[j];
            if (g) {
                /* a side screen: its sharp 3D, or what the HUD drew over it */
                int gl = b + f->x - g->x, gy = sy - g->y;
                uint8_t gs = g->scr && !paused ? g->scr[(size_t)gy * g->w + gl] : scr[b];
                if (hud && hud[X / kx])
                    d[X] = 0xFF000000u | disp32[hud[X / kx]];
                else if (gs != g->low[(size_t)gy * g->w + gl])
                    d[X] = 0xFF000000u | disp32[gs];
                else
                    d[X] = (s3d_aa && !g->map ? disp3d : disp32)[g->high[(size_t)(gy * ky + Yv % ky) *
                                                                         g->w * kx + gl * kx + X % kx]];
                continue;
            }
            if (hm && b >= 0 && b < f->w && hm[b])     /* a HUD picture (the weapon's box): in front of
                                                         * the targets' brackets too (P125) */
                d[X] = 0xFF000000u | disp32[scr[b]];
            else if (hud && hud[X / kx])     /* the targets' brackets and labels */
                d[X] = 0xFF000000u | disp32[hud[X / kx]];
            else if (nc && b >= 0 && b < f->w && covered(b, cx0, cx1, nc))
                d[X] = 0xFF000000u | disp32[scr[b]];    /* a goggles' screen */
            else if (rw->pack) {
                /* a cockpit pack: the 3D wherever its picture is see-through (the picture goes
                 * over the rest), what the HUD drew over the game's 3D (not the game's own art),
                 * the screen where the HUD drew beside the game's view or a help box is, and
                 * where the game's art has a hole outside the view's window (the pirate's weapon
                 * box: the screen there, as without a pack, not the wide 3D; P84) */
                int C = rw->xh0 + X / kx, gx = C - rw->E;
                int on = gx >= 0 && gx < rw->sw, inv = b >= 0 && b < f->w;
                /* a damaged piece of the game's 4:3 frame where the pack has its window: the
                 * view, as where the frame is whole (P95) */
                int dm = on && ckp.marks && ckp.marks[(size_t)sy * rw->sw + gx] && C >= 0 && C < rw->cw &&
                         ckp.win[(size_t)sy * rw->cw + C] && !(tip && gx >= f->tip_x0 && gx <= f->tip_x1);
                if (!dm && (C < 0 || C >= rw->cw || !ckp.win[(size_t)sy * rw->cw + C] ||
                    (tip && gx >= f->tip_x0 && gx <= f->tip_x1) ||
                    (on && !inv && src[(size_t)sy * pitch + gx] != ckp.art[(size_t)sy * rw->sw + gx]) ||
                    (inv && !ckp.art[(size_t)sy * rw->sw + f->x + b] &&
                     (!win[b] || (win[b] == 2 && scr[b] != low[b])))))
                    d[X] = 0;
                else if (!dm && inv && scr[b] != low[b] &&
                         scr[b] != ckp.art[(size_t)sy * rw->sw + f->x + b])
                    d[X] = 0xFF000000u | disp32[scr[b]];
                else if (tc && tc[X])
                    d[X] = tc_px(tc_faded(tc[X], hi[X]), a3);
                else
                    d[X] = p3[hi[X]];
            }
            else if (a < 0 || a >= f->w || !win[a] || (win[a] == 2 && scr[a] != low[a]) ||
                     (tip && a + f->x >= f->tip_x0 && a + f->x <= f->tip_x1))
                d[X] = 0;               /* the art, a popup help box: the stretched screen */
            else if (b >= 0 && b < f->w && win[b] && scr[b] != low[b] &&
                     !(tip && b + f->x >= f->tip_x0 && b + f->x <= f->tip_x1))
                d[X] = 0xFF000000u | disp32[scr[b]];    /* the HUD drew here */
            else if (tc && tc[X])   /* True colour */
                d[X] = tc_px(tc_faded(tc[X], hi[X]), a3);
            else
                d[X] = p3[hi[X]];
        }
        if (s3d_aa && f->lab_n)
            label_alpha(f, Y, d);
    }
}

/* ... with the layered helmet ("Helmet motion"): the same choices, each pixel into one of three
 * pictures.  The world (pixels): the 3D everywhere (under the art too, for the parts to move
 * off), and the HUD's pixels over the game's 3D in the window (the reticle; a HUD display in the
 * helmet's art goes with its part instead).  The frame's part (pixf): the screen's
 * pixel wherever the frame is (the art's place, a help box, the HUD beside the view, the goggles'
 * screens, the side screens), see-through where the world shows.  The targets' layer (pixb). */
/* the layered helmet's part at a pixel of the view's texture (sharp X of game column C, sharp row Yv
 * of game row gy): the picture's own pixel under it, not its game pixel's first.  A game pixel at
 * the art's edge is half art, half window; its window half went with the window and stayed still
 * while the part moved: a dot of the game's own art beside the console (owner) */
static int ckp_part_at(const CkRows *rw, int gy, int C, int X, int Yv)
{
    int s = ckp.s, sx = (X % rw->kx) * s / rw->kx, sy = ((Yv % rw->ky) + rw->ky) % rw->ky * s / rw->ky;
    return ckp.parts[(size_t)(gy * s + sy) * ckp.pw * s + (size_t)C * s + sx];
}

static void ck_rows_one(void *arg, int part, int thread)
{
    const CkRows *rw = arg;
    const port_s3d_frame *f = rw->f, *const *sv = rw->sv;
    const uint8_t *src = rw->src;
    const int *l1 = rw->l1;
    int pitch = rw->pitch, hw = rw->hw, hh = rw->hh, kx = rw->kx, ky = rw->ky, paused = rw->paused;
    int cw = rw->cw, sw = rw->sw, E = rw->E;
    int y0 = part * 16, y1 = y0 + 16 < hh ? y0 + 16 : hh, X, Y, i;
    (void)thread;
    for (Y = y0; Y < y1; Y++) {
        int Yv = Y - rw->eyk, ly = Yv / ky, sy = f->y + ly;    /* (Yv: the view's row) */
        int tip = f->tip_x1 > f->tip_x0 && sy >= f->tip_y0 && sy <= f->tip_y1;
        const uint8_t *win = f->win + (size_t)ly * f->w;
        const uint8_t *scr = f->scr && !paused ? f->scr + (size_t)ly * f->w : src + (size_t)sy * pitch + f->x;
        const uint8_t *low = f->low + (size_t)ly * f->w;
        const uint8_t *hi = f->high + (ptrdiff_t)Yv * hw;
        const uint8_t *hud = f->hud ? f->hud + (size_t)ly * (hw / kx) : NULL;
        const uint8_t *hm = f->hudm ? f->hudm + (size_t)ly * f->w : NULL;
        const uint32_t *p3 = s3d_aa && !f->map ? disp3d : disp32;     /* the 3D's colours */
        const uint32_t *tc = f->tc && f->tc_rows[Yv] && !rw->ir ? f->tc + (ptrdiff_t)Yv * hw : NULL;
        uint32_t a3 = p3[0] & 0xFF000000u;
        uint32_t *dw = (uint32_t *)((uint8_t *)rw->pixels + (size_t)Y * rw->tpitch);
        uint32_t *df = (uint32_t *)((uint8_t *)rw->pixf + (size_t)Y * rw->fpitch);
        uint32_t *db = (uint32_t *)((uint8_t *)rw->pixb + (size_t)Y * rw->bpitch);
        const port_s3d_frame *gr[PORT_S3D_SLOTS];
        int ng = 0, j, cx0[3], cx1[3], nc = 0;
        uint8_t hc;
        if (Yv < 0 || Yv >= f->h * ky) {     /* the layered helmet's rows more: the 3D, and in the
                                         * frame the screen's own pixels (what the HUD drew there,
                                         * e.g. the weapon's picture below the view: it moves with
                                         * its part; the pack's art goes over it, ckp_layer) */
            int gy = f->y + (Yv < 0 ? -((-Yv + ky - 1) / ky) : Yv / ky);
            for (X = 0; X < hw; X++) {
                int gx = rw->xh0 + X / kx - E, C = rw->xh0 + X / kx;
                dw[X] = tc && tc[X] ? tc_px(tc_faded(tc[X], hi[X]), a3) : p3[hi[X]];
                /* (only in the helmet's parts: in its window, the 3D) */
                /* (and in the pack's holes where the screen is not the game's art: what the HUD
                 * drew there, the pirate's weapon box in the 320 x 200 cockpit above the view) */
                df[X] = gx >= 0 && gx < sw && gy >= 0 && gy < rw->sh && C >= 0 && C < cw &&
                        (!rw->lay || ckp_part_at(rw, gy, C, X, Yv) ||
                         (ckp.win[(size_t)gy * cw + C] && src[(size_t)gy * pitch + gx] != ckp.art[(size_t)gy * sw + gx]))
                            ? 0xFF000000u | disp32[src[(size_t)gy * pitch + gx]] : 0;
                db[X] = 0;
            }
            continue;
        }
        for (i = 0; i < f->cov_n; i++)  /* the goggles' screens on this row: the HUD's there */
            if (sy >= f->cov[i][1] && sy < f->cov[i][3]) {
                cx0[nc] = f->cov[i][0] - f->x;
                cx1[nc++] = f->cov[i][2] - f->x;
            }
        for (i = 1; i < PORT_S3D_SLOTS; i++)    /* the side screens on this row */
            if (sv[i] && sy >= sv[i]->y && sy < sv[i]->y + sv[i]->h)
                gr[ng++] = sv[i];
        for (X = 0; X < hw; X++) {
            int b = l1[X], C = rw->xh0 + X / kx, gx = C - E;
            int on = gx >= 0 && gx < sw, inv = b >= 0 && b < f->w, inc = C >= 0 && C < cw;
            const port_s3d_frame *g = NULL;
            uint32_t w3 = tc && tc[X] ? tc_px(tc_faded(tc[X], hi[X]), a3) : p3[hi[X]], fr = 0;
            db[X] = hud && hud[X / kx] && !(hm && inv && hm[b]) ? 0xFF000000u | disp32[hud[X / kx]] : 0;
            if (hud_over_3d(rw, X, Yv, &hc)) {
                if (rw->lay && inc && sy >= 0 && sy < rw->sh &&
                    ckp_part_at(rw, sy, C, X, Yv)) {
                    df[X] = 0xFF000000u | disp32[hc];   /* a HUD display in the helmet's art (the
                                                         * number beside the health bar): it moves
                                                         * with its part */
                    dw[X] = w3;
                    db[X] = 0;
                    continue;
                }
                w3 = 0xFF000000u | disp32[hc];          /* the reticle, with the 3D */
            }
            for (j = 0; j < ng && !g; j++)
                if (b + f->x >= gr[j]->x && b + f->x < gr[j]->x + gr[j]->w)
                    g = gr[j];
            if (g) {                    /* a side screen: its sharp 3D, or what the HUD drew over it */
                int gl = b + f->x - g->x, gy = sy - g->y;
                uint8_t gs = g->scr && !paused ? g->scr[(size_t)gy * g->w + gl] : scr[b];
                if (gs != g->low[(size_t)gy * g->w + gl])
                    fr = 0xFF000000u | disp32[gs];
                else
                    fr = 0xFF000000u | (s3d_aa && !g->map ? disp3d : disp32)[g->high[(size_t)(gy * ky + Yv % ky) *
                                                                             g->w * kx + gl * kx + X % kx]];
            } else if ((nc && inv && covered(b, cx0, cx1, nc)) || (hm && inv && hm[b]))
                fr = 0xFF000000u | disp32[scr[b]];      /* a goggles' screen, a HUD picture */
            else {
                int dm = on && ckp.marks && ckp.marks[(size_t)sy * sw + gx] && inc &&
                         ckp.win[(size_t)sy * cw + C] && !(tip && gx >= f->tip_x0 && gx <= f->tip_x1);
                if (!dm && (!inc || !ckp.win[(size_t)sy * cw + C] || (tip && gx >= f->tip_x0 && gx <= f->tip_x1) ||
                    (on && !inv && src[(size_t)sy * pitch + gx] != ckp.art[(size_t)sy * sw + gx]) ||
                    (inv && !ckp.art[(size_t)sy * sw + f->x + b] && (!win[b] || (win[b] == 2 && scr[b] != low[b])))))
                    fr = on ? 0xFF000000u | disp32[src[(size_t)sy * pitch + gx]] : 0xFF000000u;
                else if (!dm && inv && !win[b] && scr[b] != low[b] && scr[b] != ckp.art[(size_t)sy * sw + f->x + b]) {
                    /* what the HUD drew beside the window, over the game's 3D in the view's
                     * rectangle (as without the layered helmet): the pirate's weapon box in the
                     * 320 x 200 cockpit, whose rows are the view's there; with the frame's part */
                    df[X] = 0xFF000000u | disp32[scr[b]];
                    dw[X] = w3;
                    continue;
                }
                if (fr && rw->lay && inc && sy >= 0 && sy < rw->sh && !ckp_part_at(rw, sy, C, X, Yv))
                    fr = 0;             /* (the window's half of an edge pixel: see ckp_part_at) */
            }
            dw[X] = w3;
            df[X] = fr;
        }
        if (s3d_aa && f->lab_n)
            label_alpha(f, Yv, dw);
    }
}

/* the hit's static on the pack's picture: a hit breaks rows of the console's screens (hl_scr) and
 * of the goggles' screens into static */
static uint8_t hl_static[2];            /* the static's light and dark colours */
static uint32_t hl_static_pal[256];     /* the palette they are for */
static int hl_static_ok;
static uint8_t *hl_scr;                 /* the screens (for the static), ckp.pix's size: pixels of the console */
static const uint8_t *hl_scr_of;        /* ... of this picture */
static uint32_t hl_rnd = 0x2545F491u;

static uint32_t hl_rand(void)
{
    hl_rnd ^= hl_rnd << 13;
    hl_rnd ^= hl_rnd >> 17;
    hl_rnd ^= hl_rnd << 5;
    return hl_rnd;
}

/* the static's colours in the palette shown: the nearest to a pale blue and a dark navy (the
 * game's palette, so the infrared and the palette's flashes work on them too) */
static void hl_make_static(void)
{
    static const double want[2][3] = { { 150, 160, 255 }, { 10, 10, 60 } };
    int k, j;
    if (hl_static_ok && !memcmp(hl_static_pal, pal32, sizeof pal32))
        return;
    for (k = 0; k < 2; k++) {
        double best = 1e30;
        hl_static[k] = 0;
        for (j = 1; j < 256; j++) {
            uint32_t v = pal32[j];
            double dr = (v >> 16 & 0xFF) - want[k][0], dg = (v >> 8 & 0xFF) - want[k][1], db = (v & 0xFF) - want[k][2];
            double d = dr * dr + dg * dg + db * db;
            if (d < best) {
                best = d;
                hl_static[k] = (uint8_t)j;
            }
        }
    }
    memcpy(hl_static_pal, pal32, sizeof pal32);
    hl_static_ok = 1;
}

/* the screens of the console (for the hit's static): the picture's big dark-blue areas on its
 * chin, bezels and brow (the test clips' rule), the bright green box of the weapon's screen (the
 * game's art has it there, the HUD draws the weapon over it: the goggles' hump, the cockpit's
 * screen above the map), and its big see-through holes beside the window (the pirate's screens:
 * the game's own show there), less their edges; W x H: ckp.pix's */
static void hl_make_screens(int W, int H, int s)
{
    size_t n = (size_t)W * H, i;
    int *stack;
    uint8_t *m;
    int min_area = 3000 * s * s / 4 * H / (400 * s), er = 3 * s, x, y;
    if (hl_scr_of == ckp.pix && hl_scr)
        return;
    free(hl_scr);
    hl_scr = calloc(n, 1);
    m = malloc(n);
    stack = malloc(n * sizeof *stack);
    hl_scr_of = ckp.pix;
    if (!hl_scr || !m || !stack) {
        free(m);
        free(stack);
        return;
    }
    for (i = 0; i < n; i++) {
        uint32_t v = pal32[ckp.pix[i]];
        int r = v >> 16 & 0xFF, g = v >> 8 & 0xFF, b = v & 0xFF;
        m[i] = !ckp.pix[i] ? 4 : g > r + 80 && g > b + 80 ? 6 :
               (!ckp.lay || ckp.lay[i] <= 2) && b > r + 30 && 0.299 * r + 0.587 * g + 0.114 * b < 90;
    }
    for (i = 0; i < n; i++) {           /* the big areas (a hole: not the window, the biggest) */
        int top = 0, area = 0, kind = m[i], keep;
        size_t k;
        if (kind != 1 && kind != 4 && kind != 6)
            continue;
        stack[top++] = (int)i;
        m[i] = 2;
        while (top) {
            int p = stack[--top], px = p % W, py = p / W;
            area++;
            if (px > 0 && m[p - 1] == kind) { m[p - 1] = 2; stack[top++] = p - 1; }
            if (px < W - 1 && m[p + 1] == kind) { m[p + 1] = 2; stack[top++] = p + 1; }
            if (py > 0 && m[p - W] == kind) { m[p - W] = 2; stack[top++] = p - W; }
            if (py < H - 1 && m[p + W] == kind) { m[p + W] = 2; stack[top++] = p + W; }
        }
        keep = area >= (kind == 6 ? min_area / 8 : min_area) && !(kind == 4 && (size_t)area * 8 > n);
        stack[top++] = (int)i;          /* again: kept (3) or not (5) */
        m[i] = 3;
        while (top) {
            int p = stack[--top], px = p % W, py = p / W;
            hl_scr[p] = (uint8_t)keep;
            if (px > 0 && m[p - 1] == 2) { m[p - 1] = 3; stack[top++] = p - 1; }
            if (px < W - 1 && m[p + 1] == 2) { m[p + 1] = 3; stack[top++] = p + 1; }
            if (py > 0 && m[p - W] == 2) { m[p - W] = 3; stack[top++] = p - W; }
            if (py < H - 1 && m[p + W] == 2) { m[p + W] = 3; stack[top++] = p + W; }
        }
        (void)k;
    }
    for (y = 0; y < H; y++)             /* less their edges: er pixels in from any other pixel */
        for (x = 0; x < W; x++) {
            size_t k = (size_t)y * W + x;
            m[k] = hl_scr[k];
        }
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            size_t k = (size_t)y * W + x;
            int a, ok = m[k];
            for (a = -er; a <= er && ok; a++)
                ok = x + a >= 0 && x + a < W && y + a >= 0 && y + a < H && m[k + a] && m[(size_t)(y + a) * W + x];
            hl_scr[k] = (uint8_t)ok;
        }
    free(m);
    free(stack);
}

/* a cockpit pack's picture over the screen laid 1:1 (ck_compose), at its own pixels, 16 rows of
 * the game's a part: beside the screen the picture's pixels; on it the picture's wherever the
 * screen still shows the game's art at that place, or the art is the game's window there (the
 * picture covers it), or the screen shows the game's own 3D in the view's window there (the
 * window takes the art's edge pixels: the picture covers them too), but not where the HUD drew
 * over the art, the targets' layer has a pixel or a popup help box is; 0 (see-through) elsewhere
 * and where the picture's pixel is 0 */
typedef struct {
    const uint8_t *src;
    const port_s3d_frame *f;
    void *pixels;
    int pitch, tpitch, w, h, E, cw, s, xh0, paused;
    double hit;                         /* the hit's static: 1 full */
    uint32_t seed;
} CkpRows;

static uint32_t hl_hash(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t r = a * 0x85EBCA6Bu ^ b * 0xC2B2AE35u ^ c;
    r ^= r >> 15;
    r *= 0x2C1B3C6Du;
    r ^= r >> 12;
    r *= 0x297A2D39u;
    r ^= r >> 15;
    return r;
}

static void ckp_rows(void *arg, int part, int thread)
{
    const CkpRows *rw = arg;
    const port_s3d_frame *f = rw->f;
    int s = rw->s, cw = rw->cw, w = rw->w, E = rw->E, FW = cw * s;
    int y0 = part * 16, y1 = y0 + 16 < rw->h ? y0 + 16 : rw->h, Y, C, i, j;
    (void)thread;
    for (Y = y0; Y < y1; Y++) {
        const uint8_t *sr = rw->src + (size_t)Y * rw->pitch, *o = ckp.art + (size_t)Y * w;
        const uint8_t *mk = ckp.marks ? ckp.marks + (size_t)Y * w : NULL, *pw = ckp.win + (size_t)Y * cw;
        const uint8_t *hud = NULL, *vw = NULL, *vs = NULL, *vl = NULL, *hm = NULL;
        int hud_w = 0, tip = 0, vx = f ? f->x : 0, stat[2] = { 0, 0 }, ng = 0, gx0[3], gx1[3];
        if (f && f->hud && Y >= f->y && Y < f->y + f->h) {
            hud_w = f->w + 2 * f->ex / f->kx;
            hud = f->hud + (size_t)(Y - f->y) * hud_w;
        }
        if (f && f->win && Y >= f->y && Y < f->y + f->h) {     /* the view's window, its 3D */
            size_t r = (size_t)(Y - f->y) * f->w;
            vw = f->win + r;
            vs = f->scr && !rw->paused ? f->scr + r : sr + f->x;
            vl = f->low + r;
            hm = f->hudm ? f->hudm + r : NULL;
        }
        if (f && f->tip_x1 > f->tip_x0 && Y >= f->tip_y0 && Y <= f->tip_y1)
            tip = 1;
        if (rw->hit > 0 && hl_scr)      /* these rows of the screens in static? */
            for (j = 0; j < s; j++)
                stat[j] = (hl_hash((uint32_t)(Y * s + j), 0x51A7u, rw->seed) & 0xFFFF) < (uint32_t)(65536 * 0.35 * rw->hit);
        if (rw->hit > 0 && f && f->goggles)     /* the goggles' screens on this row (less a pixel) */
            for (i = 0; i < f->cov_n; i++)
                if (Y > f->cov[i][1] && Y < f->cov[i][3] - 1) {
                    gx0[ng] = f->cov[i][0] + 1;
                    gx1[ng++] = f->cov[i][2] - 1;
                }
        for (C = 0; C < cw; C++) {
            int gx = C - E, v = gx - vx, beside = C < 0 || C >= cw;
            /* (in the view's rectangle the screen can be the game's 3D over its own art too:
             * the pirate's weapon box, whose bottom row the HUD fills black only in some frames,
             * so the 3D showed there as a line; the pack's art goes over it as over the rest) */
            /* (a hole in the game's art beside the view's window, inside its rectangle, where
             * the HUD drew: the screen, not the pack's art; the pirate's weapon box in the
             * 320 x 200 goggles, whose art is a hole there and the pack's black: no weapon) */
            int hud_hole = vw && v >= 0 && v < f->w && !vw[v] && gx >= 0 && gx < w && !o[gx] && sr[gx];
            int show = beside || gx < 0 || gx >= w || sr[gx] == o[gx] || (!o[gx] && !hud_hole) ||
                       (vw && v >= 0 && v < f->w && (vw[v] || o[gx]) && vs[v] == vl[v]);
            int tipc = !beside && tip && gx >= f->tip_x0 && gx <= f->tip_x1, gs = 0;
            for (j = 0; j < ng && !gs; j++)
                gs = gx >= gx0[j] && gx < gx1[j];
            /* (the targets' brackets and labels stay behind the pack's art: its art goes over
             * them, as over the 3D; owner, P126) */
            /* (a pixel of a HUD picture in the view, the weapon's in the Heavy's and Scout's box:
             * the screen, even where it matches the 3D's colour; green specks of the box's art
             * at night, owner) */
            if (show && hm && v >= 0 && v < f->w && hm[v])
                show = 0;
            if (!show && mk && gx >= 0 && gx < w && mk[gx] && pw[C] && !tipc &&
                !(hud && C - rw->xh0 >= 0 && C - rw->xh0 < hud_w && hud[C - rw->xh0]))
                show = 1;       /* a damaged piece of the 4:3 frame on the pack's window (P95) */
            if (show && tipc)
                show = 0;
            for (j = 0; j < s; j++) {
                int R = Y * s + j;
                const uint8_t *pk = ckp.pix + (size_t)R * FW + (size_t)C * s;
                size_t kf = (size_t)R * FW + (size_t)C * s;
                uint32_t *d = (uint32_t *)((uint8_t *)rw->pixels + (size_t)R * rw->tpitch) + (size_t)C * s;
                for (i = 0; i < s; i++) {
                    int val = show ? pk[i] : 0, X = C * s + i;
                    if (stat[j] && !tipc && ((hl_scr && hl_scr[kf + i]) || gs))   /* static on a screen */
                        val = hl_static[(hl_hash((uint32_t)(X / (5 * s)), (uint32_t)R, rw->seed) & 0xFF) < 115 ? 0 : 1];
                    d[i] = val ? 0xFF000000u | disp32[val] : 0;
                }
            }
        }
    }
}

/* ... made again when the picture, the colours, the screen or the main view's picture (its
 * targets' layer, its help box) changed, and each frame of a hit's static; drawn over the whole target (tw x th) */
static void ckp_layer(int w, int h, int pitch, const uint8_t *src, const port_s3d_frame *f, int E,
                      int tw, int th)
{
    int cw = ckp.pw, s = ckp.s, y, same, xh0 = f ? ck.xh0 : 0, TW = cw * s;
    double hit = 0;
    unsigned fs = f ? f->serial : 0;
    SDL_FRect r = { 0, 0, (float)tw, (float)th };

    if (w > 320 || h > 400)
        return;
    {                                   /* the hit's static */
        port_helmet_state hs;
        port_helmet_get(&hs);
        hit = hs.hit;
        if (hit > 0) {
            hl_make_static();
            hl_make_screens(cw * s, h * s, s);
        }
    }
    if (ckp_tex && (ckp_tw != TW || ckp_th != h * s)) {
        SDL_DestroyTexture(ckp_tex);
        ckp_tex = NULL;
    }
    if (!ckp_tex) {
        ckp_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, TW, h * s);
        if (!ckp_tex)
            return;
        SDL_SetTextureBlendMode(ckp_tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(ckp_tex, SDL_SCALEMODE_NEAREST);
        ckp_tw = TW;
        ckp_th = h * s;
        ckp_made.ok = 0;
    }
    same = ckp_made.ok && ckp_made.serial == ckp.serial && ckp_made.pal == disp32_serial &&
           ckp_made.f == fs && ckp_made.xh0 == xh0 &&
           hit <= 0 && ckp_made.hit <= 0;
    for (y = 0; y < h; y++)             /* the screen as last time? (and keep it) */
        if (memcmp(ckp_scr + (size_t)y * w, src + (size_t)y * pitch, w)) {
            memcpy(ckp_scr + (size_t)y * w, src + (size_t)y * pitch, w);
            same = 0;
        }
    if (!same) {
        void *pixels;
        int tpitch;
        if (!SDL_LockTexture(ckp_tex, NULL, &pixels, &tpitch))
            return;
        {
            CkpRows c;
            c.src = src;
            c.f = f;
            c.pixels = pixels;
            c.pitch = pitch;
            c.tpitch = tpitch;
            c.w = w;
            c.h = h;
            c.E = E;
            c.cw = cw;
            c.s = s;
            c.xh0 = xh0;
            c.paused = port_game_paused();
            c.hit = hit;
            c.seed = hl_rand();
            port_parallel_display(ckp_rows, &c, (h + 15) / 16);
        }
        SDL_UnlockTexture(ckp_tex);
        ckp_made.ok = 1;
        ckp_made.serial = ckp.serial;
        ckp_made.pal = disp32_serial;
        ckp_made.f = fs;
        ckp_made.xh0 = xh0;
        ckp_made.hit = hit;
    }
    SDL_RenderTexture(renderer, ckp_tex, NULL, &r);
}

/* "Helmet motion" with the layered helmet (the pack picture's -parts.png, cockpitpack.c): the
 * frame (ckf_target: the screen, the frame's part of the view and the picture, at rest) is drawn
 * part by part back to front, each moved by its own share of the helmet's move, with its hidden
 * art (what is under the parts in front of it) moved with it; the 3D and what the HUD drew over it
 * stay still under them all.  The parts' places are rectangles of the picture's pixels (runs of a
 * row, the same runs of the rows below joined), drawn as geometry from the frame's target and the
 * hidden art's texture: whole pixels of the picture */
typedef struct { short x0, y0, x1, y1; } HlRect;
static struct {
    const uint8_t *parts, *hidden;      /* the picture's maps they are made of */
    unsigned id;                        /* ... of this picture (ckp.id) */
    int W, H, np;
    const uint8_t *border;              /* ... and the art beyond its edges (ckp.border) */
    int bb;
    HlRect *r[HELM_PARTS + 1][4];       /* each part's (0: the window) places: its own, its hidden
                                         * art, its border (the border's pixels beside its own) and
                                         * its hidden border (beside its hidden art at the edge) */
    int n[HELM_PARTS + 1][4];
    SDL_Texture *htex;                  /* the hidden art (W x np H) */
    SDL_Texture *btex;                  /* the border ((W + 2 bb) x (H + 2 bb)) */
    unsigned hpal;                      /* ... in these colours */
} hlp;

/* the renderer is made again (make_renderer): its textures go with the old one */
static void hlp_forget(void)
{
    if (hlp.htex)
        SDL_DestroyTexture(hlp.htex);
    if (hlp.btex)
        SDL_DestroyTexture(hlp.btex);
    hlp.htex = hlp.btex = NULL;
    hlp.W = 0;                          /* (made again for the next frame) */
}

static void hlp_rects(const uint8_t *m, int W, int H, int val, HlRect **out, int *nout)
{
    HlRect *r = NULL, *open = NULL;
    int n = 0, cap = 0, y, x, i, no = 0, nn;
    int *xs = malloc(sizeof(int) * (W + 1) * 2);    /* this row's runs */
    HlRect *nop = malloc(sizeof(HlRect) * (W + 1));
    open = malloc(sizeof(HlRect) * (W + 1));
    if (!xs || !nop || !open)
        goto done;
    for (y = 0; y <= H; y++) {
        int k = 0;
        if (y < H)
            for (x = 0; x < W; x++)
                if ((val < 0 ? m[(size_t)y * W + x] != 0 : m[(size_t)y * W + x] == val) &&
                    (x == 0 || (val < 0 ? !m[(size_t)y * W + x - 1] : m[(size_t)y * W + x - 1] != val))) {
                    int e = x;
                    while (e < W && (val < 0 ? m[(size_t)y * W + e] != 0 : m[(size_t)y * W + e] == val))
                        e++;
                    xs[k++] = x;
                    xs[k++] = e;
                    x = e - 1;
                }
        nn = 0;
        for (i = 0; i < no; i++) {      /* the open rectangles: on, or closed */
            int j, on = 0;
            for (j = 0; j < k && !on; j += 2)
                if (xs[j] == open[i].x0 && xs[j + 1] == open[i].x1) {
                    on = 1;
                    xs[j] = -1;
                }
            if (on) {
                open[i].y1 = (short)(y + 1);
                nop[nn++] = open[i];
            } else {
                if (n == cap) {
                    HlRect *t = realloc(r, sizeof *r * (cap = cap ? cap * 2 : 256));
                    if (!t)
                        goto done;
                    r = t;
                }
                r[n++] = open[i];
            }
        }
        for (i = 0; i < k; i += 2)
            if (xs[i] >= 0) {
                nop[nn].x0 = (short)xs[i];
                nop[nn].x1 = (short)xs[i + 1];
                nop[nn].y0 = (short)y;
                nop[nn].y1 = (short)(y + 1);
                nn++;
            }
        memcpy(open, nop, sizeof *open * nn);
        no = nn;
    }
done:
    free(xs);
    free(nop);
    free(open);
    *out = r;
    *nout = n;
}

/* the parts' places for the picture (W x H) as they are; the hidden art's texture in the colours */
static int hlp_make(int W, int H)
{
    int p, k, np = ckp.np;
    int bb = ckp.border ? ckp.bb : 0, BW = W + 2 * bb, BH = H + 2 * bb;
    /* (by the picture's id and its number of parts too: a pack loaded again can get its maps at the
     * same addresses with other parts, P130) */
    if (hlp.parts != ckp.parts || hlp.hidden != ckp.hidden || hlp.border != ckp.border || hlp.W != W || hlp.H != H ||
        hlp.np != np || hlp.id != ckp.id) {
        uint8_t *bl = NULL;
        for (p = 0; p <= HELM_PARTS; p++)
            for (k = 0; k < 4; k++) {
                free(hlp.r[p][k]);
                hlp.r[p][k] = NULL;
                hlp.n[p][k] = 0;
            }
        if (hlp.htex)
            SDL_DestroyTexture(hlp.htex);
        if (hlp.btex)
            SDL_DestroyTexture(hlp.btex);
        hlp.htex = hlp.btex = NULL;
        hlp.parts = ckp.parts;
        hlp.hidden = ckp.hidden;
        hlp.id = ckp.id;
        hlp.border = ckp.border;
        hlp.bb = bb;
        hlp.W = W;
        hlp.H = H;
        hlp.np = np;
        if (bb)
            bl = malloc((size_t)BW * BH);
        for (p = 0; p <= np; p++) {
            hlp_rects(ckp.parts, W, H, p, &hlp.r[p][0], &hlp.n[p][0]);
            if (p)
                hlp_rects(ckp.hidden + (size_t)(p - 1) * W * H, W, H, -1, &hlp.r[p][1], &hlp.n[p][1]);
            if (p && bl) {
                int x, y, j;            /* each border pixel: 1 the part at the picture's edge beside
                                         * it (where that is the window, the nearest art along the
                                         * edge: a frame's curve that goes on past the edge; else
                                         * part 1), 2 the part's hidden art */
                const uint8_t *hd = ckp.hidden + (size_t)(p - 1) * W * H;
                for (y = 0; y < BH; y++)
                    for (x = 0; x < BW; x++) {
                        int px = x - bb, py = y - bb, q;
                        uint8_t v = 0;
                        if ((px < 0 || px >= W || py < 0 || py >= H) && ckp.border[(size_t)y * BW + x]) {
                            int side = px < 0 || px >= W, t;
                            px = px < 0 ? 0 : px >= W ? W - 1 : px;
                            py = py < 0 ? 0 : py >= H ? H - 1 : py;
                            q = ckp.parts[(size_t)py * W + px];
                            for (t = 1; !q && t <= 4 * bb; t++) {   /* (along the edge, nearest first) */
                                int b2;
                                for (b2 = 0; b2 < 2 && !q; b2++) {
                                    int ax = side ? px : px + (b2 ? t : -t), ay = side ? py + (b2 ? t : -t) : py;
                                    if (ax < 0 || ax >= W || ay < 0 || ay >= H || !ckp.parts[(size_t)ay * W + ax])
                                        continue;
                                    q = ckp.parts[(size_t)ay * W + ax];
                                    px = ax;
                                    py = ay;
                                }
                            }
                            v = (q ? q : 1) == p ? 1 : hd[(size_t)py * W + px] ? 2 : 0;
                        }
                        bl[(size_t)y * BW + x] = v;
                    }
                for (j = 0; j < 2; j++)
                    hlp_rects(bl, BW, BH, j + 1, &hlp.r[p][2 + j], &hlp.n[p][2 + j]);
            }
        }
        free(bl);
        hlp.htex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H * np);
        if (hlp.htex) {
            SDL_SetTextureBlendMode(hlp.htex, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(hlp.htex, SDL_SCALEMODE_NEAREST);
        }
        if (bb && (hlp.btex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, BW, BH))) {
            SDL_SetTextureBlendMode(hlp.btex, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(hlp.btex, SDL_SCALEMODE_NEAREST);
        }
        hlp.hpal = disp32_serial - 1;
    }
    if (!hlp.htex)
        return 0;
    if (hlp.hpal != disp32_serial) {    /* the hidden art in the colours shown */
        void *pixels;
        int tpitch, y, x;
        if (!SDL_LockTexture(hlp.htex, NULL, &pixels, &tpitch))
            return 0;
        for (y = 0; y < H * np; y++) {
            const uint8_t *h = ckp.hidden + (size_t)y * W;
            uint32_t *d = (uint32_t *)((uint8_t *)pixels + (size_t)y * tpitch);
            for (x = 0; x < W; x++)
                d[x] = h[x] ? 0xFF000000u | disp32[h[x]] : 0;
        }
        SDL_UnlockTexture(hlp.htex);
        if (hlp.btex && SDL_LockTexture(hlp.btex, NULL, &pixels, &tpitch)) {
            for (y = 0; y < BH; y++) {
                const uint8_t *h = ckp.border + (size_t)y * BW;
                uint32_t *d = (uint32_t *)((uint8_t *)pixels + (size_t)y * tpitch);
                for (x = 0; x < BW; x++)
                    d[x] = h[x] ? 0xFF000000u | disp32[h[x]] : 0;
            }
            SDL_UnlockTexture(hlp.btex);
        }
        hlp.hpal = disp32_serial;
    }
    return 1;
}

#define HELM_ZREF 0.125                 /* m: the part that moves as the helmet */
#define HELM_DEPTH_K 1.0                /* (1: the parallax as it is; more makes the depth easier to
                                         * see, but opens wider gaps between the parts) */
/* each part's move now, the picture's pixels (H: its rows; rows 200: half the move down) */
static void hlp_offsets(int W, int H, int rows, int *dx, int *dy)
{
    port_helmet_state hs;
    const double TX = 1.19175359;       /* the test clips' view: 100 degrees across 1920 pixels */
    double limx = 7.0 * ckp.s, limy = (rows == 200 ? 3.0 : 6.0) * ckp.s;
    int p;
    port_helmet_get(&hs);
    for (p = 0; p < HELM_PARTS; p++) {
        double z = ckp.pz[p], a, b, k;
        dx[p] = dy[p] = 0;
        if (ckp.pmove[p] == HELM_STILL)
            continue;
        /* the nearer a part, the more it moves (the helmet turns about the neck, behind the eye:
         * DEPTH_K above 1 makes the layers' depth easier to see) */
        k = pow(HELM_ZREF / z, HELM_DEPTH_K);
        a = (hs.yaw * 19.2 - hs.ex / z / TX * 960) / 1920 * W;
        b = (hs.pitch * 19.2 + hs.ey / z / TX * 960) / 1080 * H;
        dx[p] = (int)floor(k * limx * tanh(a / limx) + 0.5);
        if ((ckp.pmove[p] == HELM_SIDE_DOWN && b > 0) || ckp.pmove[p] == HELM_MOVE)
            dy[p] = (int)floor(k * limy * tanh(b / limy) + 0.5);
    }
}

/* the rectangles rs (n) of the picture's pixels from tex (k: its pixels a picture's pixel, v0:
 * the picture's rows down) to the target moved dx, dy (fx, fy: the target's pixels a picture's
 * pixel, whole ones: each a copy of whole pixels; kx, ky: tex's pixels a picture's pixel) */
static void hlp_copy(SDL_Texture *tex, const HlRect *rs, int n, int kx, int ky, int v0, int fx, int fy, int dx, int dy)
{
    int i;
    for (i = 0; i < n; i++) {
        const HlRect *r = &rs[i];
        SDL_FRect s = { (float)(r->x0 * kx), (float)((r->y0 + v0) * ky), (float)((r->x1 - r->x0) * kx),
                        (float)((r->y1 - r->y0) * ky) };
        SDL_FRect d = { (float)((r->x0 + dx) * fx), (float)((r->y0 + dy) * fy), (float)((r->x1 - r->x0) * fx),
                        (float)((r->y1 - r->y0) * fy) };
        SDL_RenderTexture(renderer, tex, &s, &d);
    }
}

/* the frame (frame: tw x th, the picture's W x H pixels at rest) part by part over the target */
static void hlp_draw(SDL_Texture *frame, int tw, int th, int rows)
{
    int W = ckp.pw * ckp.s, H = rows * ckp.s, dx[HELM_PARTS], dy[HELM_PARTS], p, fx = tw / W, fy = th / H;
    if (!hlp_make(W, H) || fx < 1 || fy < 1 || fx * W != tw || fy * H != th) {
        SDL_RenderTexture(renderer, frame, NULL, NULL);
        return;
    }
    hlp_offsets(W, H, rows, dx, dy);
    for (p = 0; p < HELM_PARTS && hlp.bb; p++) {   /* (never further than the border reaches) */
        dx[p] = dx[p] > hlp.bb ? hlp.bb : dx[p] < -hlp.bb ? -hlp.bb : dx[p];
        dy[p] = dy[p] > hlp.bb ? hlp.bb : dy[p] < -hlp.bb ? -hlp.bb : dy[p];
    }
    hlp_copy(frame, hlp.r[0][0], hlp.n[0][0], fx, fy, 0, fx, fy, 0, 0);        /* the window's: still */
    /* first every part's hidden art (under its neighbours, in front and behind: a gap that opens
     * between two parts shows either one's art, never the 3D), then the parts themselves */
    for (p = hlp.np; p >= 1; p--) {     /* (front to back: where two cover a gap, the one behind) */
        hlp_copy(hlp.htex, hlp.r[p][1], hlp.n[p][1], 1, 1, (p - 1) * H, fx, fy, dx[p - 1], dy[p - 1]);
        if (hlp.btex)                   /* (its border's pixels from bb beyond the picture's) */
            hlp_copy(hlp.btex, hlp.r[p][3], hlp.n[p][3], 1, 1, 0, fx, fy, dx[p - 1] - hlp.bb, dy[p - 1] - hlp.bb);
    }
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    for (p = 1; p <= hlp.np; p++) {
        int i;
        if (hlp.btex)
            hlp_copy(hlp.btex, hlp.r[p][2], hlp.n[p][2], 1, 1, 0, fx, fy, dx[p - 1] - hlp.bb, dy[p - 1] - hlp.bb);
        /* (black under the part first, as the still helmet has: where the frame is see-through
         * over the art, e.g. the black the HUD draws in the timer's box, the 3D drawn taller than
         * the window must not show through) */
        for (i = 0; i < hlp.n[p][0]; i++) {
            const HlRect *r = &hlp.r[p][0][i];
            SDL_FRect d = { (float)((r->x0 + dx[p - 1]) * fx), (float)((r->y0 + dy[p - 1]) * fy),
                            (float)((r->x1 - r->x0) * fx), (float)((r->y1 - r->y0) * fy) };
            SDL_RenderFillRect(renderer, &d);
        }
        hlp_copy(frame, hlp.r[p][0], hlp.n[p][0], fx, fy, 0, fx, fy, dx[p - 1], dy[p - 1]);
    }
}

/* Widescreen, the mission's screen (cockpit or goggles): the screen (base: kx x ky its size, the
 * side screens' views in it) stretched evenly to the window's shape, cw = w + 2 E of the game's
 * columns, then the main view's wide picture f (NULL: none) behind the stretched window: where
 * the window is (its stencil less the art over it, at the stretched place), the picture's pixel,
 * or what the HUD drew over the game's 3D at the same place of the game's own picture (the
 * reticle: it sits in the middle of the wide one, as the game drew it, so it stays on its
 * units); over all of it the targets' layer (f->hud).  The side screens the HUD draws over the
 * view (the goggles') with their sharp pictures (side[1..], NULL: none) where they show their 3D.
 * With a cockpit pack's picture (ckp_on) the screen is not stretched but laid 1:1 in its middle,
 * the main view's picture behind the picture's see-through pixels and the picture over it all
 * (ckp_layer); the picture has Kx x Ky of the target's pixels a game pixel, whole ones for its
 * own pixels too.  Then the pointer (the display draws it). */
static SDL_Texture *ck_compose(int w, int h, int pitch, const uint8_t *src, const port_s3d_frame *f,
                               const port_s3d_frame **side, const port_s3d_frame **smap,
                               SDL_Texture *base, int kx, int ky, int *ex)
{
    static int lsv[4096 + 16], l1[4096 + 16];
    int E = f ? f->wide_e : port_s3d_wide_e, cw, tw, th, Kx = kx, Ky = ky;
    int cwid, chei, hx, hy, one = 0, lay = 0, fin;
    SDL_FRect vr = { 0, 0, 0, 0 };

    if (ckp_on) {
        E = (ckp.pw - w) / 2;
        Kx = kx % ckp.s ? kx * ckp.s : kx;
        Ky = ky % ckp.s ? ky * ckp.s : ky;
        /* "Helmet motion" with the picture's parts (-parts.png): the layered helmet, its parts
         * move apart (hlp_draw) */
        lay = ckp.parts && ckp.np && port_helmet_motion && w <= 320 && h <= 400;
    }
    port_helmet_sway = lay;             /* (sharp3d.c: the view's rows more above and below) */
    one = lay;                          /* (the world, the frame and the targets' layer apart) */
    cw = w + 2 * E;
    tw = cw * Kx;
    th = h * Ky;

    /* P75: with a cockpit pack's picture and no main view's picture to lay in (the first frames
     * after a switch of cockpit and goggles, sharp3d.c settling) nothing would show behind the
     * picture's see-through pixels beside the screen: black at both sides.  The last screen once
     * more instead, for up to a quarter of a second or 6 of the game's frames (a slow display: in
     * a quarter of a second it may not see the 2 frames settling takes, P107); the switch shows a
     * few frames later */
    if (ckp_on && !(f && f->ex / kx > 0 && f->w * kx + 2 * f->ex <= 4096) && ck_target && ck.pack &&
        ck_good_us && (port_now_us() - ck_good_us < 250000 || game_frame - ck_good_frame <= 6)) {
        ck.on = 1;
        *ex = E;
        return ck_target;
    }
    /* the layered helmet on a switch of cockpit and goggles: the game has the other one up and the
     * view is still the last one's (its screen in the new frame's place behind the picture): the
     * last screen once more, as the one-piece frame gets it (P107) */
    if (lay && f && f->goggles != port_hud_goggles() && ck_target && ck_gw == tw && ck_gh == th && ck_good_us) {
        ck.on = 1;
        *ex = E;
        return ck_target;
    }
    if (!ck_target || ck_gw != tw || ck_gh != th) {
        if (ck_target)
            SDL_DestroyTexture(ck_target);
        ck_target = SDL_CreateTexture(renderer, s3d_aa ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_XRGB8888,
                                      SDL_TEXTUREACCESS_TARGET, tw, th);
        ck_gw = tw;
        ck_gh = th;
        if (!ck_target)
            return NULL;
        SDL_SetTextureBlendMode(ck_target, SDL_BLENDMODE_NONE);
    }
    if (one && (!ckf_target || ckf_tw != tw || ckf_th != th)) {
        if (ckf_target)
            SDL_DestroyTexture(ckf_target);
        ckf_target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, tw, th);
        ckf_tw = tw;
        ckf_th = th;
        if (ckf_target) {
            SDL_SetTextureBlendMode(ckf_target, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(ckf_target, SDL_SCALEMODE_NEAREST);
        }
    }
    if (one && !ckf_target)
        one = lay = 0;
    SDL_SetRenderTarget(renderer, one ? ckf_target : ck_target);
    fin = one;                          /* (the frame into ckf_target) */
    SDL_SetTextureScaleMode(base, SDL_SCALEMODE_NEAREST);  /* the columns as the window's below */
    if (ckp_on) {                       /* a cockpit pack's picture: the screen 1:1 in its middle */
        SDL_FRect r = { (float)(E * Kx), 0, (float)(w * Kx), (float)th };
        SDL_BlendMode bm = SDL_BLENDMODE_NONE;
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        if (one) {                      /* (opaque into the frame's target) */
            SDL_GetTextureBlendMode(base, &bm);
            SDL_SetTextureBlendMode(base, SDL_BLENDMODE_NONE);
        }
        SDL_RenderTexture(renderer, base, NULL, &r);
        if (one)
            SDL_SetTextureBlendMode(base, bm);
    } else
        SDL_RenderTexture(renderer, base, NULL, NULL);
    ck.on = 1;
    ck.cw = cw;
    ck.kx = Kx;
    ck.ky = Ky;
    ck.view = 0;
    ck.pack = ckp_on;
    ck.pe = E;
    if (f && f->ex / kx > 0 && f->w * kx + 2 * f->ex <= 4096) {
        int hw = f->w * kx + 2 * f->ex, hh = (f->h + 2 * f->ey) * ky, fe = f->ex / kx, X, Y, tpitch, i, same = 1;
        int paused = port_game_paused();      /* the screen as it is (sharp3d_compose) */
        const port_s3d_frame *sv[PORT_S3D_SLOTS] = { NULL };
        double cu = ckp_on ? f->x + E + f->w / 2.0 : (f->x + f->w / 2.0) * cw / w;
        unsigned pk = ckp_on ? ckp.serial ^ (one ? 0x80000000u : 0) : 0;
        void *pixels, *pixf = NULL, *pixb = NULL;
        int fpitch = 0, bpitch = 0;
        ck.fx = f->x;
        ck.fy = f->y;
        ck.fw = f->w;
        ck.fh = f->h;
        ck.fe = fe;
        ck.xh0 = (int)SDL_floor(cu - f->w / 2.0 - fe + 0.5);
        ck.win = f->win;
        vr.x = (float)(ck.xh0 * Kx);    /* where the picture goes in the target */
        vr.y = (float)((f->y - f->ey) * Ky);
        vr.w = (float)(hw / kx * Kx);
        vr.h = (float)((f->h + 2 * f->ey) * Ky);
        if (!ck_tex || ck_tw != hw || ck_th != hh) {
            if (ck_tex)
                SDL_DestroyTexture(ck_tex);
            ck_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, hw, hh);
            ck_tw = hw;
            ck_th = hh;
            ck_made.serial = 0;
            if (ck_tex) {
                SDL_SetTextureBlendMode(ck_tex, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(ck_tex, SDL_SCALEMODE_NEAREST);
            }
            if (ckv_tex)
                SDL_DestroyTexture(ckv_tex);
            if (ckb_tex)
                SDL_DestroyTexture(ckb_tex);
            ckv_tex = ckb_tex = NULL;
        }
        if (one && !ckv_tex && ck_tex) {        /* the layered helmet's frame part, targets' layer */
            ckv_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, hw, hh);
            ckb_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, hw, hh);
            if (ckv_tex) {
                SDL_SetTextureBlendMode(ckv_tex, SDL_BLENDMODE_NONE);
                SDL_SetTextureScaleMode(ckv_tex, SDL_SCALEMODE_NEAREST);
            }
            if (ckb_tex) {
                SDL_SetTextureBlendMode(ckb_tex, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(ckb_tex, SDL_SCALEMODE_NEAREST);
            }
            ck_made.serial = 0;
        }
        if (one && (!ckv_tex || !ckb_tex))
            one = lay = 0;            /* (no room: still; the frame was begun in ckf_target, so the view
                                 * goes there, and that over the target below) */
        for (i = 1; i < PORT_S3D_SLOTS; i++) {  /* the side screens inside the view */
            const port_s3d_frame *g = side ? side[i] : NULL;
            if (g && g->ex == 0 && g->x >= f->x && g->y >= f->y && g->x + g->w <= f->x + f->w &&
                g->y + g->h <= f->y + f->h)
                sv[i] = g;
            same &= (sv[i] ? sv[i]->serial : 0) == ck_made.side[i];
        }
        if (ck_tex && f->scr && !paused && f->serial == ck_made.serial && disp32_serial == ck_made.pal &&
            ck.xh0 == ck_made.xh0 && cw == ck_made.cw && pk == ck_made.pack && same) {
            /* the same picture as last time */
            if (one)
                SDL_RenderTexture(renderer, ckv_tex, NULL, &vr);
            else {
                SDL_SetTextureBlendMode(ck_tex, s3d_aa ? aa_blend() : SDL_BLENDMODE_BLEND);
                SDL_RenderTexture(renderer, ck_tex, NULL, &vr);
            }
            ck.view = 1;
            goto pointer;
        }
        ck_made.serial = 0;
        for (X = 0; X < hw; X++) {      /* each column: the stretched screen's, the game picture's */
            double U = ck.xh0 + (X + 0.5) / kx;
            lsv[X] = ckp_on ? X / kx - fe : (int)SDL_floor(U * w / cw) - f->x;
            l1[X] = X / kx - fe;
        }
        if (one && (!SDL_LockTexture(ckv_tex, NULL, &pixf, &fpitch) ||
                    !SDL_LockTexture(ckb_tex, NULL, &pixb, &bpitch))) {
            if (pixf)
                SDL_UnlockTexture(ckv_tex);
            pixf = NULL;
        }
        if (ck_tex && (!one || pixf) && SDL_LockTexture(ck_tex, NULL, &pixels, &tpitch)) {
            {
                CkRows c;
                c.f = f;
                SDL_memcpy(c.sv, sv, sizeof c.sv);
                c.src = src;
                c.pixels = pixels;
                c.lsv = lsv;
                c.l1 = l1;
                c.pitch = pitch;
                c.tpitch = tpitch;
                c.hw = hw;
                c.hh = hh;
                c.kx = kx;
                c.ky = ky;
                c.paused = paused;
                c.ir = port_ir_on();
                c.pack = ckp_on;
                c.E = E;
                c.cw = cw;
                c.xh0 = ck.xh0;
                c.sw = w;
                c.pixf = pixf;
                c.pixb = pixb;
                c.fpitch = fpitch;
                c.bpitch = bpitch;
                c.eyk = f->ey * ky;
                c.sh = h;
                c.lay = lay && ckp.parts;
                tc_prep(f);
                port_parallel_display(one ? ck_rows_one : ck_rows, &c, (hh + 15) / 16);
            }
            SDL_UnlockTexture(ck_tex);
            if (one) {
                SDL_UnlockTexture(ckv_tex);
                SDL_UnlockTexture(ckb_tex);
            }
            ck_made.serial = f->serial;
            ck_made.pal = disp32_serial;
            for (i = 0; i < PORT_S3D_SLOTS; i++)
                ck_made.side[i] = sv[i] ? sv[i]->serial : 0;
            ck_made.xh0 = ck.xh0;
            ck_made.cw = cw;
            ck_made.pack = pk;
            if (one)
                SDL_RenderTexture(renderer, ckv_tex, NULL, &vr);
            else {
                SDL_SetTextureBlendMode(ck_tex, s3d_aa ? aa_blend() : SDL_BLENDMODE_BLEND);
                SDL_RenderTexture(renderer, ck_tex, NULL, &vr);
            }
            ck.view = 1;
        }
    }
pointer:
    if (ckp_on)                         /* the cockpit pack's picture over it all */
        ckp_layer(w, h, pitch, src, ck.view ? f : NULL, E, tw, th);
    {   /* the cockpit's side screens' maps: over their stretched screens at the right proportions
         * (the picture's columns 1:1), the stretched screen where the HUD drew over the map */
        static int col[4096 + 16];
        int s, paused = port_game_paused();
        for (s = 1; s < PORT_S3D_SLOTS; s++) {
            const port_s3d_frame *g = smap ? smap[s] : NULL;
            int hw, hh, ge, X, Y, tpitch;
            void *pixels;
            ckm[s].on = 0;
            if (!g || g->kx != kx || g->ky != ky || g->ex / kx <= 0 || g->w * kx + 2 * g->ex > 4096)
                continue;
            ge = g->ex / kx;
            hw = g->w * kx + 2 * g->ex;
            hh = g->h * ky;
            ckm[s].x = g->x;
            ckm[s].y = g->y;
            ckm[s].w = g->w;
            ckm[s].h = g->h;
            ckm[s].e = ge;
            ckm[s].xh0 = (int)SDL_floor((g->x + g->w / 2.0) * cw / w - g->w / 2.0 - ge + 0.5);
            ckm[s].scr = g->scr && !paused ? g->scr : NULL;
            ckm[s].low = g->low;
            if (!ckm_tex[s] || ckm_tw[s] != hw || ckm_th[s] != hh) {
                if (ckm_tex[s])
                    SDL_DestroyTexture(ckm_tex[s]);
                ckm_tex[s] = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, hw, hh);
                ckm_tw[s] = hw;
                ckm_th[s] = hh;
                ckm_made[s].serial = 0;
                if (!ckm_tex[s])
                    continue;
                SDL_SetTextureBlendMode(ckm_tex[s], SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(ckm_tex[s], SDL_SCALEMODE_NEAREST);
            }
            if (!(g->scr && !paused && g->serial == ckm_made[s].serial && disp32_serial == ckm_made[s].pal &&
                  ckm[s].xh0 == ckm_made[s].xh0 && cw == ckm_made[s].cw)) {
                if (!SDL_LockTexture(ckm_tex[s], NULL, &pixels, &tpitch))
                    continue;
                for (X = 0; X < hw; X++)
                    col[X] = (int)SDL_floor((ckm[s].xh0 + (X + 0.5) / kx) * w / cw) - g->x;
                for (Y = 0; Y < hh; Y++) {
                    int ly = Y / ky;
                    const uint8_t *scr = g->scr && !paused ? g->scr + (size_t)ly * g->w
                                                           : src + (size_t)(g->y + ly) * pitch + g->x;
                    const uint8_t *low = g->low + (size_t)ly * g->w, *hi = g->high + (size_t)Y * hw;
                    uint32_t *d = (uint32_t *)((uint8_t *)pixels + (size_t)Y * tpitch);
                    for (X = 0; X < hw; X++) {
                        int a = col[X];
                        d[X] = a < 0 || a >= g->w || scr[a] != low[a] ? 0 : 0xFF000000u | disp32[hi[X]];
                    }
                }
                SDL_UnlockTexture(ckm_tex[s]);
                ckm_made[s].serial = g->serial;
                ckm_made[s].pal = disp32_serial;
                ckm_made[s].xh0 = ckm[s].xh0;
                ckm_made[s].cw = cw;
            }
            {
                SDL_FRect r = { (float)(ckm[s].xh0 * kx), (float)(g->y * ky), (float)hw, (float)hh };
                SDL_RenderTexture(renderer, ckm_tex[s], NULL, &r);
            }
            ckm[s].on = 1;
        }
    }
    if (one || fin) {                   /* the layered helmet: the world, the targets' layer over it,
                                         * the frame's parts over all */
        SDL_FRect fr = { 0, 0, (float)tw, (float)th };
        SDL_SetRenderTarget(renderer, ck_target);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        if (one && ck.view) {
            SDL_SetTextureBlendMode(ck_tex, s3d_aa ? aa_blend() : SDL_BLENDMODE_BLEND);
            SDL_RenderTexture(renderer, ck_tex, NULL, &vr);
        }
        /* (the targets' brackets and labels behind the helmet: its parts and the art under them
         * go over them, as over the 3D; owner, P126) */
        if (one && ck.view)
            SDL_RenderTexture(renderer, ckb_tex, NULL, &vr);
        if (lay)
            hlp_draw(ckf_target, tw, th, h);
        else
            SDL_RenderTexture(renderer, ckf_target, NULL, &fr);
    }
    if (port_now_us() - ck_cur_seen < 150000 && cursor_tex(&cwid, &chei, &hx, &hy)) {
        int px, py, inview = 0;         /* the game's pointer, or the real one beyond its picture */
        float U;
        if (ck_dead && ck.pack) {       /* over a cockpit pack's art beside the screen */
            U = ck_dead_u;
            py = (int)ck_dead_v;
        } else if (port_wide_ptr && ck.view) {
            px = port_wide_ptr_x;
            py = port_wide_ptr_y;
            U = ck.xh0 + ck.fe + (px - ck.fx);
            inview = 1;
        } else if (port_wide_map && ckm[port_wide_map].on) {
            px = port_wide_map_x;
            py = port_wide_map_y;
            U = ckm[port_wide_map].xh0 + ckm[port_wide_map].e + (px - ckm[port_wide_map].x);
        } else {
            port_cursor_pos(&px, &py);
            U = ck_col(px, py, w) - 0.5f;
            inview = ck.view && py >= ck.fy && py < ck.fy + ck.fh && px >= ck.fx && px < ck.fx + ck.fw;
        }
        {
            SDL_FRect r = { (U - hx) * Kx, (float)((py - hy) * Ky), (float)(cwid * Kx), (float)(chei * Ky) };
            SDL_RenderTexture(renderer, wide_cur_tex, NULL, &r);
        }
    }
    SDL_SetRenderTarget(renderer, NULL);
    ck_good_us = ckp_on && ck.view ? port_now_us() : 0;
    ck_good_frame = game_frame;
    *ex = E;
    return ck_target;
}

/* a new picture of a sharp view laid into the window's colours (sharp3d_compose), 16 rows a
 * part on the window's threads (port_parallel_display, P59) */
typedef struct {
    const port_s3d_frame *f;
    const uint8_t *src;
    void *pixels;
    int pitch, tpitch, hw, hh, kx, ky, paused, ir, nomask, cx0, cy0, cx1, cy1;
} S3dRows;

static void s3d_rows(void *arg, int part, int thread)
{
    const S3dRows *rw = arg;
    const port_s3d_frame *f = rw->f;
    const uint8_t *src = rw->src;
    void *pixels = rw->pixels;
    int pitch = rw->pitch, tpitch = rw->tpitch, hw = rw->hw, kx = rw->kx, ky = rw->ky, paused = rw->paused;
    int s3d_nomask = rw->nomask, cx0 = rw->cx0, cy0 = rw->cy0, cx1 = rw->cx1, cy1 = rw->cy1;
    int y0 = part * 16, y1 = y0 + 16 < rw->hh ? y0 + 16 : rw->hh, Y;
    (void)thread;
    for (Y = y0; Y < y1; Y++) {
        int ly = Y / ky;
        const uint8_t *scr = src + (size_t)(f->y + ly) * pitch + f->x;
        const uint8_t *fscr = f->scr && !paused ? f->scr + (size_t)ly * f->w : scr;
        int c0 = cx0 - f->x, c1 = cx1 - f->x;      /* the pointer's columns on this row */
        int p0 = f->cur_x0 - f->x, p1 = f->cur_x1 - f->x;  /* ... where it was in scr */
        if (f->y + ly < cy0 || f->y + ly >= cy1)
            c0 = c1 = 0;
        if (f->y + ly < f->cur_y0 || f->y + ly >= f->cur_y1)
            p0 = p1 = 0;
        const uint8_t *low = f->low + (size_t)ly * f->w;
        const uint8_t *hm = f->hudm ? f->hudm + (size_t)ly * f->w : NULL;     /* HUD pictures */
        const uint8_t *hi = f->high + (size_t)Y * hw;
        const uint32_t *p3 = s3d_aa && !f->map ? disp3d : disp32;     /* the 3D's colours */
        const uint32_t *tc = f->tc && f->tc_rows[Y] && !rw->ir ? f->tc + (size_t)Y * hw : NULL;
        uint32_t a3 = p3[0] & 0xFF000000u;
        uint32_t *d = (uint32_t *)((uint8_t *)pixels + (size_t)Y * tpitch), *d0 = d;
        int lx, k;
        for (k = 0; k < f->ex; k++, hi++, tc = tc ? tc + 1 : NULL)  /* widescreen: the sides as drawn */
            *d++ = tc && *tc ? tc_px(tc_faded(*tc, *hi), a3) : p3[*hi];
        for (lx = 0; lx < f->w; lx++) {
            /* the screen as copied with the picture (the game may have copied the next
             * frame's since); the screen itself under the pointer, which the game draws
             * onto it */
            uint8_t sv = (lx >= c0 && lx < c1) || (lx >= p0 && lx < p1) ? scr[lx] : fscr[lx];
            if ((sv != low[lx] || (hm && hm[lx])) && !s3d_nomask) {     /* the HUD drew here */
                uint32_t c = disp32[sv];
                for (k = 0; k < kx; k++)
                    *d++ = c;
                hi += kx;
                if (tc)
                    tc += kx;
            } else if (tc)          /* True colour: its full colours where it has them */
                for (k = 0; k < kx; k++, hi++, tc++)
                    *d++ = *tc ? tc_px(tc_faded(*tc, *hi), a3) : p3[*hi];
            else
                for (k = 0; k < kx; k++)
                    *d++ = p3[*hi++];
        }
        for (k = 0; k < f->ex; k++, hi++, tc = tc ? tc + 1 : NULL)
            *d++ = tc && *tc ? tc_px(tc_faded(*tc, *hi), a3) : p3[*hi];
        if (s3d_aa && f->lab_n)
            label_alpha(f, Y, d0);
    }
}

/* the screen src still shows what was copied with the picture f (9 in 10 of every 4th pixel of its
 * view, every 2nd row): the frame it was made for, though the game may have switched the cockpit
 * and the goggles since */
static int frame_fits_screen(const port_s3d_frame *f, const uint8_t *src, int pitch)
{
    int x, y, n = 0, same = 0;
    if (!f->scr)
        return 0;
    for (y = 1; y < f->h; y += 2)
        for (x = 1; x < f->w; x += 4) {
            n++;
            same += f->scr[(size_t)y * f->w + x] == src[(size_t)(f->y + y) * pitch + f->x + x];
        }
    return n && same * 10 >= n * 9;
}

/* The sharp 3D views (sharp3d.c) laid into the screen: a picture of the whole screen at kx x ky
 * its size, each sharp view where the screen still shows the game's own 3D pixels and the
 * screen's pixels wherever the HUD or the mouse pointer drew over them.  Widescreen: the main
 * view's picture is wider than the screen, by *ex of the screen's columns on each side, and the
 * screen sits in the middle of it.  NULL if there is none for this screen. */
static SDL_Texture *sharp3d_compose_now(int w, int h, int pitch, const uint8_t *src, int *ex)
{
    const port_s3d_frame *fs[PORT_S3D_SLOTS], *ckf, *sm[PORT_S3D_SLOTS] = { NULL };
    int kx = 0, ky = 0, i, n = 0, ox, tw, cx0, cx1, cy0, cy1;
    int stretch = port_widescreen && port_s3d_wide_e > 0 && port_s3d_stretch_up();
    /* paused: no new frames come, and the game draws its PAUSED box on the screen itself, so the
     * display goes by the screen as it is, not as copied with the picture */
    int paused = port_game_paused();

    static int s3d_nomask = -1;
    if (s3d_nomask < 0)
        s3d_nomask = SDL_getenv("FREEFALL_S3D_NOMASK") != NULL;
    if ((port_screen_fxaa_wanted() && renderer_gpu) != s3d_aa) {
        /* "Anti-aliasing" on or off: the pictures again, with or without the 3D's mark */
        s3d_aa = !s3d_aa;
        for (i = 0; i < PORT_S3D_SLOTS; i++) {
            if (s3d_tex[i])
                SDL_DestroyTexture(s3d_tex[i]);
            s3d_tex[i] = NULL;
        }
        if (s3d_target)
            SDL_DestroyTexture(s3d_target);
        if (ck_target)
            SDL_DestroyTexture(ck_target);
        s3d_target = ck_target = NULL;
        ck_made.serial = 0;
    }
    for (i = 0; i < PORT_S3D_SLOTS; i++) {
        const port_s3d_frame *f = port_s3d_take(i);
        if (f && (f->scr_w != w || f->scr_h != h || f->x + f->w > w || f->y + f->h > h))
            f = NULL;
        /* made before a switch of cockpit and goggles: with the screen that went with it still up
         * (the game switched as it drew the next frame) it is the one to show (P107) */
        if (f && f->goggles != port_hud_goggles() && !frame_fits_screen(f, src, pitch))
            f = NULL;
        if (f && f->ckpack != ckp_on)   /* ... or of a cockpit pack's picture and the stretched screen */
            f = NULL;
        if (f && !kx) {                 /* all at the first one's size */
            kx = f->kx;
            ky = f->ky;
        }
        if (f && (f->kx != kx || f->ky != ky))
            f = NULL;
        fs[i] = f;
        n += f != NULL;
    }
    ck.on = 0;
    if (!n)
        return stretch ? ck_compose(w, h, pitch, src, NULL, NULL, NULL, texture, 1, 1, ex) : NULL;
    ckf = stretch && fs[0] && fs[0]->win ? fs[0] : NULL;    /* the main view's: laid in after */
    if (ckf || (fs[0] && fs[0]->win))
        fs[0] = NULL;
    ox = fs[0] ? fs[0]->ex : 0;         /* widescreen: the picture's columns left of the screen */
    {                                   /* the game's pointer on its screen */
        const uint8_t *cb;
        int cw, ch, crow, ctype, chx, chy, ccol, px, py;
        cx0 = cx1 = cy0 = cy1 = 0;
        if (port_cursor_shown() && port_cursor_get(&cb, &cw, &ch, &crow, &ctype, &chx, &chy, &ccol)) {
            port_cursor_pos(&px, &py);
            cx0 = px - chx - 2;
            cy0 = py - chy - 2;
            cx1 = cx0 + cw + 4;
            cy1 = cy0 + ch + 4;
        }
    }
    for (i = 1; i < PORT_S3D_SLOTS; i++) {     /* a side screen's map, widescreen: laid in after */
        sm[i] = fs[i] && fs[i]->ex ? fs[i] : NULL;
        if (sm[i])
            fs[i] = NULL;
    }
    tw = w * kx + 2 * ox;
    *ex = ox / kx;
    if (!s3d_target || s3d_gw != tw || s3d_gh != h * ky) {
        if (s3d_target)
            SDL_DestroyTexture(s3d_target);
        s3d_target = SDL_CreateTexture(renderer, s3d_aa ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_XRGB8888,
                                       SDL_TEXTUREACCESS_TARGET, tw, h * ky);
        s3d_gw = tw;
        s3d_gh = h * ky;
        if (!s3d_target)
            return NULL;
        SDL_SetTextureBlendMode(s3d_target, SDL_BLENDMODE_NONE);
    }
    SDL_SetRenderTarget(renderer, s3d_target);
    if (ox) {                           /* black beside the screen where the view is not */
        SDL_FRect r = { (float)ox, 0, (float)(w * kx), (float)(h * ky) };
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_RenderTexture(renderer, texture, NULL, &r);
    } else
        SDL_RenderTexture(renderer, texture, NULL, NULL);
    for (i = 0; i < PORT_S3D_SLOTS; i++) {
        const port_s3d_frame *f = fs[i];
        int hw, hh, Y, tpitch;
        void *pixels;
        SDL_FRect r;
        if (!f)
            continue;
        hw = f->w * kx + 2 * f->ex;
        hh = f->h * ky;
        if (!s3d_tex[i] || s3d_tw[i] != hw || s3d_th[i] != hh) {
            if (s3d_tex[i])
                SDL_DestroyTexture(s3d_tex[i]);
            s3d_tex[i] = SDL_CreateTexture(renderer, s3d_aa ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_XRGB8888,
                                           SDL_TEXTUREACCESS_STREAMING, hw, hh);
            s3d_tw[i] = hw;
            s3d_th[i] = hh;
            if (!s3d_tex[i])
                continue;
            SDL_SetTextureBlendMode(s3d_tex[i], SDL_BLENDMODE_NONE);
            SDL_SetTextureScaleMode(s3d_tex[i], SDL_SCALEMODE_NEAREST);
            s3d_made[i].serial = 0;
        }
        /* the same picture as at the last present (the game makes 25 or 60 a second, the display
         * shows more), with the pointer where it was: the texture holds it already */
        if (f->scr && !paused && f->serial == s3d_made[i].serial && cx0 == s3d_made[i].cx0 && cy0 == s3d_made[i].cy0 &&
            cx1 == s3d_made[i].cx1 && cy1 == s3d_made[i].cy1 && disp32_serial == s3d_made[i].pal)
            goto draw;
        if (!SDL_LockTexture(s3d_tex[i], NULL, &pixels, &tpitch))
            continue;
        s3d_made[i].serial = f->serial;
        s3d_made[i].cx0 = cx0;
        s3d_made[i].cy0 = cy0;
        s3d_made[i].cx1 = cx1;
        s3d_made[i].cy1 = cy1;
        s3d_made[i].pal = disp32_serial;
        {
            S3dRows c;
            c.f = f;
            c.src = src;
            c.pixels = pixels;
            c.pitch = pitch;
            c.tpitch = tpitch;
            c.hw = hw;
            c.hh = hh;
            c.kx = kx;
            c.ky = ky;
            c.paused = paused;
            c.ir = port_ir_on();
            c.nomask = s3d_nomask;
            c.cx0 = cx0;
            c.cy0 = cy0;
            c.cx1 = cx1;
            c.cy1 = cy1;
            tc_prep(f);
            port_parallel_display(s3d_rows, &c, (hh + 15) / 16);
        }
        SDL_UnlockTexture(s3d_tex[i]);
    draw:
        r.x = (float)(f->x * kx + ox - f->ex);
        r.y = (float)(f->y * ky);
        r.w = (float)hw;
        r.h = (float)hh;
        SDL_RenderTexture(renderer, s3d_tex[i], NULL, &r);
    }
    SDL_SetRenderTarget(renderer, NULL);
    if (stretch)
        return ck_compose(w, h, pitch, src, ckf, fs, sm, s3d_target, kx, ky, ex);
    return s3d_target;
}

/* sharp3d_compose_now, but with a cockpit pack's picture, on a switch of the cockpit and the
 * goggles when the screen already shows the new one and the main view's picture for it is not
 * made yet: the last picture once more (at most 3 times), not the new screen without its 3D
 * beyond the screen's sides (P107) */
static SDL_Texture *sharp3d_compose(int w, int h, int pitch, const uint8_t *src, int *ex)
{
    static SDL_Texture *last;
    static unsigned last_gen;
    static int last_ex, held;
    const port_s3d_frame *f = port_s3d_take(0);
    if (f && last && last_gen == renderer_gen && held < 3 && ckp_on && f->goggles != port_hud_goggles() &&
        !frame_fits_screen(f, src, pitch)) {
        held++;
        *ex = last_ex;
        return last;
    }
    held = 0;
    last = sharp3d_compose_now(w, h, pitch, src, ex);
    last_gen = renderer_gen;
    last_ex = *ex;
    return last;
}

/* "Show FPS": the game's frames a second (port_frame_limit), the window's, and the main view's
 * drawing time for the 3D resolution and terrain detail (sharp3d.c), averaged over half a second,
 * centred under the cockpit's health and energy bars, under the goggles' compass, or at the top
 * of the screen outside missions */
static void fps_meter(int ww, int wh)
{
    static uint64_t t0;
    static unsigned long frames0, presents, us_sum, us_n;
    static char line[96];
    uint64_t now = port_now_us();
    float s = wh >= 1400 ? 3.0f : wh >= 700 ? 2.0f : 1.0f;
    SDL_FRect bg;

    presents++;
    if (port_s3d_wanted() && port_s3d_us) {
        us_sum += port_s3d_us;
        us_n++;
    }
    if (!t0 || now - t0 >= 500000) {
        if (t0) {
            double sec = (now - t0) / 1e6;
            unsigned long gf = game_frames;
            int n = SDL_snprintf(line, sizeof line, "%s%sGame %.0f fps  Display %.0f fps",
                                 port_bench_label(), port_bench_label()[0] ? "  " : "",
                                 (gf - frames0) / sec, presents / sec);
            if (us_n)
                SDL_snprintf(line + n, sizeof line - n, "  3D %.1f ms", us_sum / 1000.0 / us_n);
            frames0 = gf;
        } else
            frames0 = game_frames;
        t0 = now;
        presents = us_sum = us_n = 0;
    }
    if (!line[0] || ww <= 0 || dst_rect.h <= 0)
        return;
    while (s > 1 && (SDL_strlen(line) * 8 + 8) * s > dst_rect.w * 0.6f)
        s--;
    bg.w = (SDL_strlen(line) * 8 + 8) * s;
    bg.h = 12 * s;
    /* the window's top right corner */
    bg.x = SDL_floorf(ww - bg.w - 4 * s);
    bg.y = 4 * s;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 170);
    SDL_RenderFillRect(renderer, &bg);
    SDL_SetRenderScale(renderer, s, s);
    SDL_SetRenderDrawColor(renderer, 255, 255, 96, 255);
    SDL_RenderDebugText(renderer, (bg.x + 4 * s) / s, (bg.y + 2 * s) / s, line);
    SDL_SetRenderScale(renderer, 1, 1);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

/* a screen filter or Anti-aliasing changed whether the renderer must be SDL's "gpu" one: the next
 * picture makes it again (the options screen's "(starting...)") */
int port_renderer_switching(void)
{
    return port_screen_gpu_wanted() != renderer_gpu;
}

static void present(void)
{
    int w = port_video.w, h = port_video.h, pitch = port_video.pitch, ww, wh, x, y, menu_wide;
    uint32_t start = port_video.start;

    if (port_screen_gpu_wanted() != renderer_gpu)   /* a screen filter that needs shaders, on or off */
        make_renderer();
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    if (w > 0 && h > 0 && pitch > 0 && start + (uint64_t)pitch * (h - 1) + w <= PORT_VRAM_KB * 1024u) {
        void *pixels;
        int tpitch;
        int ir = port_ir_on();
        if (pal_seen != port_video.pal_serial || grade_seen != port_screen_serial() || ir != ir_seen) {
            pal_seen = port_video.pal_serial;
            grade_seen = port_screen_serial();
            ir_seen = ir;
            update_palette();
            if (ir) {                   /* Infrared: Sci-fi, the thermal colours (infrared.c) */
                uint32_t ir[256];
                memcpy(ir, pal32, sizeof ir);
                port_ir_palette(ir);
                port_screen_grade(ir, disp32);
            } else
                port_screen_grade(pal32, disp32);
            for (x = 0; x < 256; x++)
                disp3d[x] = disp32[x] & 0xFEFFFFFFu;
            port_screen_grade_lut(tc_lut, &tc_sat);
            disp32_serial++;
        }
        if (!texture || tex_w != w || tex_h != h) {
            if (texture)
                SDL_DestroyTexture(texture);
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
            SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
            tex_w = w;
            tex_h = h;
        }
        if (SDL_LockTexture(texture, NULL, &pixels, &tpitch)) {
            const uint8_t *src = port_video.vram + start;
            for (y = 0; y < h; y++) {
                uint32_t *d = (uint32_t *)((uint8_t *)pixels + y * tpitch);
                const uint8_t *s = src + y * pitch;
                for (x = 0; x < w; x++)
                    d[x] = disp32[s[x]];
            }
            vsmooth_overlay(pixels, tpitch, start, pitch, w, h, port_screen_grade_rgb);  /* Smooth videos */
            SDL_UnlockTexture(texture);
        }
        /* "Widescreen": the main menu at 16:9, the owner's sides beside the game's picture */
        menu_wide = port_widescreen && w == 640 && h == 480 && port_movies_open <= 0 &&
                    port_menu_wide_up(port_video.vram + start, pitch);
        if (menu_wide) {
            if (!menu_tex) {
                menu_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING,
                                             640 + 2 * PORT_MENU_WIDE_E, 480);
                if (menu_tex)
                    SDL_SetTextureScaleMode(menu_tex, SDL_SCALEMODE_NEAREST);
            }
            if (menu_tex && SDL_LockTexture(menu_tex, NULL, &pixels, &tpitch)) {
                port_menu_wide_fill(pixels, tpitch, port_video.vram + start, pitch, disp32);
                SDL_UnlockTexture(menu_tex);
            } else
                menu_wide = 0;
        }
        /* the game's modes are all shown at 4:3, like on a CRT (screen.c: the screen filters) */
        SDL_GetCurrentRenderOutputSize(renderer, &ww, &wh);
        port_window_w = ww;
        port_window_h = wh;
        port_screen_rect(w, h, ww, wh, &dst_rect);
        ckp_on = 0;
        if (port_ck_pack[0] && port_widescreen && port_s3d_stretch_up()) {
            /* "Cockpit pack": all of its picture in the window (bars above and below it in a
             * window narrower than 16:9), the 2x one when the screen has twice its rows or more */
            SDL_FRect r;
            port_screen_rect(w, h, ww * w / port_ck_cols(w), wh, &r);
            r.x = SDL_floorf((ww - r.w) / 2);
            if (port_ck_get(w, h, (int)r.h, port_video.vram + start, pitch, &ckp)) {
                ckp_on = 1;
                dst_rect = r;
            }
        }
        port_ck_on = ckp_on;            /* the main view as wide as the picture's window (sharp3d.c) */
        port_s3d_out_h = (int)dst_rect.h;
        if (ckp_on)
            port_s3d_wide_e = (ckp.pw - w) / 2;
        else if (port_widescreen && dst_rect.w > 0) {
            /* the game's columns the window has room for beside the screen (up to 16:9) */
            float room = (float)ww < dst_rect.h * 16 / 9 ? (float)ww : dst_rect.h * 16 / 9;
            int e = (int)((room / dst_rect.w * w - w) / 2);
            port_s3d_wide_e = e > 0 ? e : 0;
        } else
            port_s3d_wide_e = 0;
        {
            int ex = 0;
            SDL_FRect shown = dst_rect;     /* where the screen went (help.c's picture) */
            int shown_ex = ckp_on ? (ckp.pw - w) / 2 : 0, shown_stretched = 0;
            SDL_Texture *sharp;
            if (port_cursor_shown())
                ck_cur_seen = port_now_us();
            sharp = sharp3d_compose(w, h, pitch, port_video.vram + start, &ex);
            if (sharp && s3d_aa) {      /* Anti-aliasing: on the 3D's pixels, at 3D resolution 2x+ */
                float sw, sh;
                if (SDL_GetTextureSize(sharp, &sw, &sh) && sh >= 2 * h)
                    sharp = port_screen_fxaa(renderer, sharp, (int)sw, (int)sh);
            }
            port_wide_cursor = ck.on;
            if (!ck.view && port_wide_ptr)  /* the wide view is gone: the game's pointer */
                port_wide_ptr = 0;
            if (sharp && ex) {          /* widescreen: the picture beyond the screen's sides */
                SDL_FRect r = dst_rect;
                r.x -= dst_rect.w * ex / w;
                r.w += 2 * dst_rect.w * ex / w;
                if (ck.on)
                    ck_rect = r;
                port_screen_draw(renderer, sharp, w + 2 * ex, h, &r);
                shown = r;
                shown_ex = ex;
                shown_stretched = !ckp_on;  /* the HUD stretched evenly, not 1:1 in a pack's middle */
            } else if (!sharp && menu_wide) {
                /* the main menu's sides beyond the screen's, as far as the window goes (the game's
                 * picture where it always is: the pointer stays in it) */
                SDL_FRect r = dst_rect;
                r.x -= dst_rect.w * PORT_MENU_WIDE_E / w;
                r.w += 2 * dst_rect.w * PORT_MENU_WIDE_E / w;
                port_screen_draw(renderer, menu_tex, w + 2 * PORT_MENU_WIDE_E, h, &r);
                shown = r;
                shown_ex = PORT_MENU_WIDE_E;
            } else {
                SDL_FRect r = dst_rect;
                /* "Movie size: Fill screen": the movie as big as the window allows (the black
                 * around it goes off the window's edges; movie.c draws movies on a black screen) */
                if (!sharp && port_movies_open > 0 && port_movie_w > 0 && !port_lockstep &&
                    port_movie_pitch == (uint32_t)pitch && port_movie_off >= start) {
                    uint32_t o = port_movie_off - start;
                    if (port_screen_movie_rect(w, h, ww, wh, (int)(o % (uint32_t)pitch), (int)(o / (uint32_t)pitch),
                                               port_movie_w, port_movie_h, &r))
                        shown = r;
                }
                port_screen_draw(renderer, sharp ? sharp : texture, w, h, &r);
            }
            port_help_present(renderer, ww, wh, &shown, w, shown_ex, shown_stretched);  /* help.c: H's screen */
        }
        port_missions_render(renderer, &dst_rect);      /* the Mission Selector, over the main menu */
        if (port_show_fps || port_bench)
            fps_meter(ww, wh);
        port_options_render(renderer, &dst_rect);
        if (shot_prefix && port_now_us() >= shot_next) {
            shot_next = port_now_us() + shot_every;
            if (port_options_open || port_missions_open || port_ms_fading || port_game_options_up || shot_window) {
                /* the whole window as shown */
                char name[1100];
                SDL_Surface *ws = SDL_RenderReadPixels(renderer, NULL);
                snprintf(name, sizeof name, "%s%04d.bmp", shot_prefix, shot_count++);
                if (ws) {
                    SDL_SaveBMP(ws, name);
                    SDL_DestroySurface(ws);
                }
            } else
                save_shot(w, h);
        }
        /* a lockstep shot (port_shot_window_sync): the second picture drawn since the request,
         * so the window shows the frame the game thread stopped after */
        if (SDL_GetAtomicInt(&sync_shot) > 1)
            SDL_AddAtomicInt(&sync_shot, -1);
        else if (SDL_GetAtomicInt(&sync_shot) == 1) {
            SDL_Surface *ws = SDL_RenderReadPixels(renderer, NULL);
            if (!ws || !SDL_SaveBMP(ws, sync_shot_name))
                port_log("shot: cannot write %s (%s)\n", sync_shot_name, SDL_GetError());
            SDL_DestroySurface(ws);
            SDL_SetAtomicInt(&sync_shot, 0);
        }
    }
    SDL_RenderPresent(renderer);
}

/* the game's thread (lockstep.c): save the whole window as shown, with the frame drawn so far,
 * to name (BMP), and wait until it is saved */
void port_shot_window_sync(const char *name)
{
    uint64_t until = port_now_us() + 10000000;     /* (a window that draws nothing: go on) */
    snprintf(sync_shot_name, sizeof sync_shot_name, "%s", name);
    SDL_SetAtomicInt(&sync_shot, 2);
    while (SDL_GetAtomicInt(&sync_shot) && port_now_us() < until)
        SDL_Delay(1);
    if (SDL_GetAtomicInt(&sync_shot)) {
        port_log("shot: the window drew nothing, %s not saved\n", name);
        SDL_SetAtomicInt(&sync_shot, 0);
    }
}

/* the pointer over the main view's wide picture beyond the game's own (sharp3d.h), at column b
 * of the view: its place there is port_wide_ptr_x / _y; the game's pointer inside its view, away
 * from the HUD at the view's corners (the goggles' rim buttons, the cockpit's window frame): the
 * middle half of its rows, a window column a few columns in from the edge of the row */
static void wide_beyond(int b, int *gx, int *gy)
{
    int m = ck.fw / 2, y = *gy - ck.fy;
    const uint8_t *row;
    port_wide_ptr_x = ck.fx + b;
    port_wide_ptr_y = *gy;
    y = y < ck.fh / 4 ? ck.fh / 4 : y >= ck.fh * 3 / 4 ? ck.fh * 3 / 4 - 1 : y;
    row = ck.win + (size_t)y * ck.fw;
    b = b < m ? 0 : ck.fw - 1;
    while (b != m && !(row[b] == 1 && row[b + (b < m ? 4 : -4)] == 1))
        b += b < m ? 1 : -1;
    b += b < m ? 4 : b > m ? -4 : 0;
    *gx = ck.fx + b;
    *gy = ck.fy + y;
}

/* window coordinates -> game pixels */
static void to_game(float wx, float wy, int *gx, int *gy)
{
    float rx, ry;
    int w = port_video.w, h = port_video.h;

    SDL_RenderCoordinatesFromWindow(renderer, wx, wy, &rx, &ry);
    ck_dead = 0;
    if (w <= 0 || h <= 0 || dst_rect.w <= 0) {
        *gx = *gy = 0;
        return;
    }
    if (ck.on && ck_rect.w > 0) {
        /* the stretched screen: the art's columns stretched; in the view's window the game's own
         * picture at its own scale in the middle of the wide one, beyond it the wide one's */
        float U = (rx - ck_rect.x) * ck.cw / ck_rect.w, V = (ry - ck_rect.y) * h / ck_rect.h;
        int side = 0, map = 0, s;
        /* the window may be a pixel wider than the picture: its last column is the picture's */
        U = U < 0 ? 0 : U >= ck.cw ? ck.cw - 0.001f : U;
        V = V < 0 ? 0 : V >= h ? h - 0.001f : V;
        *gx = (int)SDL_floorf(U * w / ck.cw);
        *gy = (int)SDL_floorf(V);
        if (ck.pack) {
            /* a cockpit pack's picture: the screen 1:1 in its middle; in the view's rows the wide
             * picture wherever the pack's is see-through and the game's window is not; beside
             * the screen the pack's art: no clicks there (the game's pointer at the edge) */
            int C = (int)SDL_floorf(U), a;
            *gx = C - ck.pe;
            a = *gx - ck.fx;
            if (ck.view && !ml_active && *gy >= ck.fy && *gy < ck.fy + ck.fh && a >= -ck.fe && a < ck.fw + ck.fe) {
                const uint8_t *row = ck.win + (size_t)(*gy - ck.fy) * ck.fw;
                int m = ck.fw / 2;
                if (a >= 0 && a < ck.fw && row[a]) {
                    while (row[a] != 1 && a != m)       /* as below */
                        a += a < m ? 1 : -1;
                    *gx = ck.fx + a;
                } else if (ckp.win[(size_t)*gy * ck.cw + C]) {
                    side = 1;
                    wide_beyond(a, gx, gy);
                }
            }
            if (!side && (*gx < 0 || *gx >= w)) {
                ck_dead = 1;
                ck_dead_u = U;
                ck_dead_v = V;
            }
        } else if (!ml_active && (s = ckm_at(*gx, *gy)) != 0) {
            /* a side screen's map: its picture's column; beyond the game's map, the game has the
             * pointer at its edge and the real column from port_wide_map_x */
            int b = (int)SDL_floorf(U - ckm[s].xh0) - ckm[s].e;
            if (b < 0 || b >= ckm[s].w) {
                map = s;
                port_wide_map_x = ckm[s].x + b;
                port_wide_map_y = *gy;
                b = b < 0 ? 0 : ckm[s].w - 1;
            }
            *gx = ckm[s].x + b;
        }
        if (ck.view && !ck.pack && !ml_active && *gy >= ck.fy && *gy < ck.fy + ck.fh) {
            const uint8_t *row = ck.win + (size_t)(*gy - ck.fy) * ck.fw;
            int a = *gx - ck.fx;
            if (a >= 0 && a < ck.fw && row[a]) {
                int b = (int)SDL_floorf(U - ck.xh0 - ck.fe), m = ck.fw / 2;
                if (b >= 0 && b < ck.fw && row[b]) {
                    /* at the window's edge (2: the art's shape decides, P18) the game's own
                     * stencil may have none: the nearest window column towards the middle */
                    while (row[b] != 1 && b != m)
                        b += b < m ? 1 : -1;
                    *gx = ck.fx + b;
                } else {                /* the wide picture beyond the game's (sharp3d.h) */
                    side = 1;
                    wide_beyond(b, gx, gy);
                }
            }
        }
        if (side != port_wide_ptr) {
            port_wide_ptr = side;
            wide_nudge = 1;
        }
        if (map != port_wide_map) {
            port_wide_map = map;
            wide_nudge = 1;
        }
    } else {
        *gx = (int)SDL_floorf((rx - dst_rect.x) * w / dst_rect.w);
        *gy = (int)SDL_floorf((ry - dst_rect.y) * h / dst_rect.h);
        port_wide_map = 0;
    }
    if (*gx < 0) *gx = 0;
    if (*gy < 0) *gy = 0;
    if (*gx >= w) *gx = w - 1;
    if (*gy >= h) *gy = h - 1;
}

/* the game moved the pointer (int 33h function 4): move the system cursor there too */
static volatile int warp_pending, warp_x, warp_y;

void port_mouse_warp(int gx, int gy)
{
    warp_x = gx;
    warp_y = gy;
    warp_pending = 1;
}

static void do_warp(void)
{
    float wx, wy;
    int w = port_video.w, h = port_video.h;

    warp_pending = 0;
    if (w <= 0 || h <= 0 || dst_rect.w <= 0 || !(SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS))
        return;
    if (ck.on && ck_rect.w > 0) {       /* the stretched screen (to_game) */
        float U = ck_col(warp_x, warp_y, w);
        if (!SDL_RenderCoordinatesToWindow(renderer, ck_rect.x + U * ck_rect.w / ck.cw,
                                           ck_rect.y + (warp_y + 0.5f) * ck_rect.h / h, &wx, &wy))
            return;
    } else if (!SDL_RenderCoordinatesToWindow(renderer, dst_rect.x + (warp_x + 0.5f) * dst_rect.w / w,
                                              dst_rect.y + (warp_y + 0.5f) * dst_rect.h / h, &wx, &wy))
        return;
    SDL_WarpMouseInWindow(window, wx, wy);
}

static int mouse_buttons;


static int sdl_buttons(SDL_MouseButtonFlags f)
{
    return ((f & SDL_BUTTON_LMASK) ? 1 : 0) | ((f & SDL_BUTTON_RMASK) ? 2 : 0) | ((f & SDL_BUTTON_MMASK) ? 4 : 0);
}

/* ------------------------------------------------------------------ mouselook
 * Quality of life, off by default (tnfree.ini mouselook=on, --mouselook).  While the player's
 * controls run in a mission (CtrlApplySlots calls port_mouselook_take each frame) the pointer
 * is captured: moving the mouse turns and looks up / down, and the game's own pointer, which
 * is the aiming reticle, stays at the centre of the main view, so the left and right buttons
 * fire and target there as usual.  The middle button frees the pointer for the HUD (it stays
 * where the reticle was) and captures it again.  Paused, in menus or between missions the
 * pointer is the game's.  Modes (tnfree.ini mouselook_mode): turn = the mouse turns the player;
 * head = it turns the view as Q / E do, up to 90 degrees either way, without changing where the
 * player walks, and F (look straight ahead) turns the player to face the view instead of
 * turning the head back (control.c).  (Hybrid and follow modes were tried and dropped.) */
int port_mouselook;                     /* the controls style: 0 Vanilla, 1 Advanced (mouselook) */
int port_mouselook_sens = 100;          /* percent */
int port_mouselook_invert;              /* 1: mouse forward looks down */
static SDL_AtomicInt ml_turn, ml_look;  /* angle units (0x10000 = full turn) not yet taken */
static SDL_AtomicInt ml_cx, ml_cy;      /* the centre of the main view (game pointer coordinates) */
static float ml_frac_x, ml_frac_y;
static volatile uint64_t ml_seen_us;    /* the last frame the player's controls ran */
static int ml_freed;
static volatile int ml_map;             /* the map fills the main view (M): no mouselook */

#define ML_UNITS 18.0f                  /* angle units per mouse count at 100% (about 0.1 degree) */

/* returns 1 in head mode (F then turns the player to face the view); map: the map fills the
 * main view, so the pointer is free until it closes */
int port_mouselook_take(int *turn, int *look, int cx, int cy, int map)
{
    *turn = SDL_SetAtomicInt(&ml_turn, 0);
    *look = SDL_SetAtomicInt(&ml_look, 0);
    ml_map = map;
    if (map)
        *turn = *look = 0;
    port_ml_frame_turn += *turn;        /* "Arms pack": the arm sways behind the turn (arms.c) */
    port_ml_frame_look += *look;
    SDL_SetAtomicInt(&ml_cx, cx);
    SDL_SetAtomicInt(&ml_cy, cy);
    if (!port_lockstep)
        port_keys_mission_frame();
    if (port_mouselook && !port_lockstep)
        ml_seen_us = port_now_us();
    return port_mouselook && !port_lockstep && !map;
}

/* Advanced controls chosen (the HUD marks the head's straight-ahead in white) */
int port_ml_improved(void)
{
    return port_mouselook && !port_lockstep;
}

/* the game's pointer to the centre of the main view */
static void ml_centre(int force)
{
    static int last_x = -1, last_y = -1;
    int x = SDL_GetAtomicInt(&ml_cx), y = SDL_GetAtomicInt(&ml_cy);

    if (force || x != last_x || y != last_y)
        port_mouse_event(x, y, mouse_buttons, 1, 0);
    last_x = x;
    last_y = y;
}

/* helmet.c helm_2617F4 (game thread): a mission's HUD starts.  The mission starts with the
 * pointer captured and gimbal mode off.  (That was done whenever the mission's controls had not
 * run for a quarter of a second, so the options screen, the help screen and the game's own
 * options screen switched gimbal mode off too: owner.)  Lockstep: its g action's, lockstep.c. */
void port_hint_mission(void);           /* help.c */
void port_mission_begin(void)
{
    if (!port_lockstep)
        ml_freed = ml_shift = port_gimbal = 0;
    port_hint_mission();
}

/* main loop: capture the pointer while a mission's controls run, release it otherwise */
static void ml_update(void)
{
    uint64_t now = port_now_us();
    int in_mission = ml_seen_us && now - ml_seen_us < 250000;
    int want = port_mouselook && !ml_freed && !ml_shift && in_mission &&
               (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS);

    if (port_options_open || ml_map || port_game_paused())     /* the pause shows the pointer */
        want = 0;
    if (!port_mouselook)
        port_gimbal = 0;                /* gimbal mode belongs to the Advanced controls */
    if (want != ml_active) {
        ml_active = want;
        SDL_SetWindowRelativeMouseMode(window, want);
        if (!want) {                    /* the system pointer comes back where the reticle is */
            port_mouse_warp(SDL_GetAtomicInt(&ml_cx), SDL_GetAtomicInt(&ml_cy));
            do_warp();
        }
        SDL_SetAtomicInt(&ml_turn, 0);
        SDL_SetAtomicInt(&ml_look, 0);
        ml_frac_x = ml_frac_y = 0;
        if (want) {
            if (port_wide_ptr) {        /* widescreen: the pointer leaves the side for the reticle */
                port_wide_ptr = 0;
                wide_nudge = 1;
            }
            ml_centre(1);
        }
    }
    if (ml_active)
        ml_centre(0);                   /* the view may have moved (helmet on / off) */
}

/* mouselook in a mission, map not open (Mouselook (hold) then frees the pointer while held) */
static int ml_shift_mission(void)
{
    return port_mouselook && !port_lockstep && !ml_map && ml_seen_us &&
           port_now_us() - ml_seen_us < 250000;
}

/* the mouselook switch (middle button unless rebound): frees / captures the pointer */
static void ml_toggle(void)
{
    if (port_mouselook && ml_seen_us && port_now_us() - ml_seen_us < 250000) {
        ml_freed = !ml_freed;
        ml_update();
    }
}

static void ml_motion(float dx, float dy)
{
    float k = ML_UNITS * port_mouselook_sens / 100.0f;
    int ix, iy;

    ml_frac_x += dx * k;
    ml_frac_y += dy * k * (port_mouselook_invert ? -1 : 1);
    ix = (int)ml_frac_x;
    iy = (int)ml_frac_y;
    ml_frac_x -= ix;
    ml_frac_y -= iy;
    SDL_AddAtomicInt(&ml_turn, ix);
    SDL_AddAtomicInt(&ml_look, iy);
}

/* lockstep runs (lockstep.c L action): a mouselook move as the mouse would give it */
void port_mouselook_add(int turn, int look)
{
    SDL_AddAtomicInt(&ml_turn, turn);
    SDL_AddAtomicInt(&ml_look, look);
}

/* ------------------------------------------------------------------ scripted input (testing)
 * FREEFALL_SCRIPT="ms:action;..." with action k<SDL key name> (press and release), d<key> / u<key>
 * (hold down / release), m<x>,<y> (move the pointer, game pixels), c (left click), q (quit),
 * M<x>,<y> (move to window pixel x,y), P / p<x>,<y> (left button down / up there), Q<x>,<y> (right
 * click there), v<x>,<y> (the real mouse to window pixel x,y).  A time written +ms counts from the
 * moment a mission starts (the script waits there until one does). */
static char *script;
static uint64_t script_due, script_mission;

static void run_script(void)
{
    char *semi, *colon;
    while (script && *script) {
        colon = strchr(script, ':');
        if (!colon)
            break;
        if (!script_due) {
            if (script[0] == '+') {     /* "+ms": counted from the first frame of a mission */
                extern unsigned char game_in_mission;
                if (!script_mission) {
                    if (!game_in_mission)
                        return;
                    script_mission = port_now_us();
                }
                script_due = script_mission + (uint64_t)atol(script + 1) * 1000;
            } else
                script_due = (uint64_t)atol(script) * 1000;
        }
        if (port_now_us() < script_due)
            return;
        script_due = 0;
        semi = strchr(colon, ';');
        if (semi)
            *semi = 0;
        colon++;
        port_log("script: %s\n", colon);
        if (colon[0] == 'k') {
            SDL_Scancode s = SDL_GetScancodeFromName(colon + 1);
            key_event(s, 1);
            key_event(s, 0);
        } else if (colon[0] == 'a' || colon[0] == 'e') {   /* Alt+key / key, as real key events */
            SDL_Event ev;
            int up;
            for (up = 0; up < 2; up++) {
                SDL_zero(ev);
                ev.type = up ? SDL_EVENT_KEY_UP : SDL_EVENT_KEY_DOWN;
                ev.key.scancode = SDL_GetScancodeFromName(colon + 1);
                ev.key.mod = colon[0] == 'a' ? SDL_KMOD_LALT : 0;
                ev.key.down = !up;
                SDL_PushEvent(&ev);
            }
        } else if (colon[0] == 'W') {   /* left click at window pixel x,y, as real mouse events */
            SDL_Event ev;
            int up;
            for (up = 0; up < 2; up++) {
                SDL_zero(ev);
                ev.type = up ? SDL_EVENT_MOUSE_BUTTON_UP : SDL_EVENT_MOUSE_BUTTON_DOWN;
                ev.button.button = SDL_BUTTON_LEFT;
                ev.button.down = !up;
                ev.button.x = (float)atoi(colon + 1);
                ev.button.y = strchr(colon, ',') ? (float)atoi(strchr(colon, ',') + 1) : 0;
                SDL_PushEvent(&ev);
            }
        } else if (colon[0] == 'M' || colon[0] == 'P' || colon[0] == 'p' || colon[0] == 'Q') {
            int x = atoi(colon + 1), y = strchr(colon, ',') ? atoi(strchr(colon, ',') + 1) : 0, gx, gy;
            to_game((float)x, (float)y, &gx, &gy);
            port_mouse_event(gx, gy, mouse_buttons, wide_nudge ? 2 : 1, 0);
            wide_nudge = 0;
            if (colon[0] == 'P' || colon[0] == 'p') {
                mouse_buttons = colon[0] == 'P' ? mouse_buttons | 1 : mouse_buttons & ~1;
                port_mouse_event(gx, gy, mouse_buttons, 0, 1);
            } else if (colon[0] == 'Q') {
                port_mouse_event(gx, gy, mouse_buttons | 2, 0, 2);
                port_mouse_event(gx, gy, mouse_buttons, 0, 2);
            }
        } else if (colon[0] == 'h') {   /* mouse wheel notch: h1 up, h-1 down, as a real event */
            SDL_Event ev;
            SDL_zero(ev);
            ev.type = SDL_EVENT_MOUSE_WHEEL;
            ev.wheel.y = (float)atoi(colon + 1);
            SDL_PushEvent(&ev);
        } else if (colon[0] == 'r') {   /* relative mouse motion dx,dy, as a real event */
            SDL_Event ev;
            SDL_zero(ev);
            ev.type = SDL_EVENT_MOUSE_MOTION;
            ev.motion.xrel = (float)atoi(colon + 1);
            ev.motion.yrel = strchr(colon, ',') ? (float)atoi(strchr(colon, ',') + 1) : 0;
            SDL_PushEvent(&ev);
        } else if (colon[0] == 'v') {   /* the real mouse to window pixel x,y (SDL's own motion) */
            SDL_WarpMouseInWindow(window, (float)atoi(colon + 1),
                                  strchr(colon, ',') ? (float)atoi(strchr(colon, ',') + 1) : 0);
        } else if (colon[0] == 'd' || colon[0] == 'u') {
            key_event(SDL_GetScancodeFromName(colon + 1), colon[0] == 'd');
        } else if (colon[0] == 'm') {
            int x = atoi(colon + 1), y = strchr(colon, ',') ? atoi(strchr(colon, ',') + 1) : 0;
            port_mouse_event(x, y, mouse_buttons, 1, 0);
        } else if (colon[0] == 'c') {
            extern void port_mouse_click(void);
            port_mouse_click();
        } else if (colon[0] == 'b') {   /* hold the game's mouse buttons (1 left, 2 right; 0 up) */
            mouse_buttons = atoi(colon + 1);
            port_mouse_event(0, 0, mouse_buttons, 0, 1);
        } else if (colon[0] == 'o') {   /* one of the game's own options (8 = the difficulty, 1 to 5) */
            port_game_option_set(atoi(colon + 1), strchr(colon, ',') ? atoi(strchr(colon, ',') + 1) : 0);
        } else if (colon[0] == 'q')
            port_request_quit();
        script = semi ? semi + 1 : NULL;
    }
}

/* ------------------------------------------------------------------ the game thread */
typedef int (*game_entry)(int, char **);
static game_entry entry;
static int g_argc;
static char **g_argv;
static volatile int game_done = 0;
static int game_status;

static int SDLCALL game_thread(void *unused)
{
    extern void port_exit(int code);
    (void)unused;
    note_game_thread();
    port_exit(entry(g_argc, g_argv));
    return 0;
}

/* exit() of the game (any thread): run the game's exit functions on the calling thread, then
 * let the main thread close the window and end the process. */
static SDL_ThreadID main_thread_id;

void port_finish(int code)
{
    game_status = code;
    game_done = 1;
    if (SDL_GetCurrentThreadID() != main_thread_id)
        for (;;)
            SDL_Delay(1000);            /* the main thread exits the process */
}

/* the game is ending (P113): its exit functions (AtExitRun) free its resources (ResTerm) on the
 * game's thread while the window's thread may be drawing from them: the game's pointer over the
 * options button (tnm_cursor's picture is a resource), the help screen, the sharp view.  From here
 * on the window keeps its last picture; a drawing under way is waited for (a second at most, so
 * that nothing the drawing waits on can hold the game's end) */
static SDL_AtomicInt game_ending, in_present;

void port_game_ending(void)
{
    uint64_t until;
    SDL_SetAtomicInt(&game_ending, 1);
    if (SDL_GetCurrentThreadID() == main_thread_id)
        return;                         /* not drawing: it called this */
    until = port_now_us() + 1000000;
    while (SDL_GetAtomicInt(&in_present) && port_now_us() < until)
        SDL_Delay(1);
}

void port_request_quit(void)
{
    extern unsigned short lg_ctrlbreak_count;
    int was;
    port_quit_requested = 1;
    /* the main loop of the game ends on a ctrl-break.  It comes as the DOS interrupt did: with
     * the interrupts on, never in the middle of a sharp or wide pass (sharp3d.c), whose putting
     * back of the game's data undid it, so a mission went on and the window shut only when its
     * 3 s were up (P103) */
    was = port_irq_save();
    lg_ctrlbreak_count = 1;
    port_irq_restore(was);
}

/* the window's display's refresh rate (its mode; looked at again every 2 s, the window may have
 * moved to another display), 0 if not known */
static double display_hz(void)
{
    static uint64_t at;
    static double hz;
    uint64_t now = port_now_us();
    if (!at || now - at > 2000000) {
        const SDL_DisplayMode *m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
        hz = !m ? 0 : m->refresh_rate_numerator > 0 && m->refresh_rate_denominator > 0 ?
             (double)m->refresh_rate_numerator / m->refresh_rate_denominator : m->refresh_rate;
        at = now;
    }
    return hz;
}

/* the options screen or the Mission Selector opened or closed: the game sees every key and
 * button go up while one is open (they draw the game's pointer themselves, options.c) */
static void options_toggled(void)
{
    int i;

    if (port_options_open || port_missions_open) {
        for (i = 0; i < PORT_KEY_COUNT; i++)
            if (keys_down[i])
                phys_event(i, 0);
        if (mouse_buttons) {
            int changed = mouse_buttons;
            mouse_buttons = 0;
            port_mouse_event(0, 0, 0, 0, changed | 0x100);
        }
        ml_update();
    } else
        SDL_HideCursor();
}

static void handle_event(SDL_Event *e)
{
    int gx, gy, b;
    if (port_bench && e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_ESCAPE)
        port_request_quit();            /* Esc stops the benchmark (bench.c) */
    if (port_lockstep && e->type != SDL_EVENT_QUIT)
        return;                         /* lockstep.c feeds the input */
    if (e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_RETURN && (e->key.mod & SDL_KMOD_ALT)) {
        /* window / full screen, first: over the options screen and the Mission Selector too (P138:
         * they took it as Enter, the chosen setting changed or the chosen mission started) */
        if (!e->key.repeat) {
            port_fullscreen = !port_fullscreen;     /* the main loop changes the window */
            port_save_settings();               /* and the options screen's Fullscreen line */
        }
        return;
    }
    if (e->type != SDL_EVENT_QUIT) {
        int was = port_options_open, was_ms = port_missions_open;
        if (port_options_event(e) || port_missions_event(e)) {
            /* (the Mission Selector keeps the game's pointer where it was: the menu's buttons
             * under it would light up) */
            if (port_options_open != was || port_missions_open != was_ms)
                options_toggled();
            else if (port_options_open && e->type == SDL_EVENT_MOUSE_MOTION && !ml_active &&
                     !port_options_held && !port_missions_open) {
                /* in the menus the game's pointer follows the mouse under the options screen, no
                 * buttons (options.c draws the same pointer over it), so it is not left behind */
                to_game(e->motion.x, e->motion.y, &gx, &gy);
                port_mouse_event(gx, gy, 0, 1, 0);
            }
            return;
        }
    }
    switch (e->type) {
    case SDL_EVENT_QUIT:
        port_request_quit();
        break;
    case SDL_EVENT_RENDER_TARGETS_RESET:
    case SDL_EVENT_RENDER_DEVICE_RESET:     /* the textures' contents may be gone: fill them again */
        SDL_memset(s3d_made, 0, sizeof s3d_made);
        ck_made.serial = 0;
        break;
    case SDL_EVENT_KEY_DOWN:
        key_event(e->key.scancode, 1);
        break;
    case SDL_EVENT_KEY_UP:
        key_event(e->key.scancode, 0);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (ml_active) {
            ml_motion(e->motion.xrel, e->motion.yrel);
            break;
        }
        to_game(e->motion.x, e->motion.y, &gx, &gy);
        port_mouse_event(gx, gy, mouse_buttons, wide_nudge ? 2 : 1, 0);
        wide_nudge = 0;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        {                               /* middle, 4 and 5 can be bound (keys.c) */
            int k = e->button.button == SDL_BUTTON_MIDDLE ? PORT_KEY_MOUSE_MIDDLE :
                    e->button.button == SDL_BUTTON_X1 ? PORT_KEY_MOUSE_4 :
                    e->button.button == SDL_BUTTON_X2 ? PORT_KEY_MOUSE_5 : 0;
            int isdown = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            if (k && (isdown ? port_keys_translate(k) != 0 : keys_down[k])) {
                phys_event(k, isdown);
                break;
            }
        }
        if (ml_active) {                /* the buttons act at the reticle, the view's centre */
            gx = SDL_GetAtomicInt(&ml_cx);
            gy = SDL_GetAtomicInt(&ml_cy);
        } else
            to_game(e->button.x, e->button.y, &gx, &gy);
        b = sdl_buttons(SDL_GetMouseState(NULL, NULL));
        if (ck_dead && !ml_active)      /* over a cockpit pack's art beside the screen: no presses */
            b &= mouse_buttons;
        if (b != mouse_buttons) {
            int changed = b ^ mouse_buttons;
            mouse_buttons = b;
            port_mouse_event(gx, gy, b, 0, changed);
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (e->wheel.y != 0)
            wheel_notch((e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e->wheel.y : e->wheel.y) > 0);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        /* release everything held, as a DOS machine would see the keys go up */
        if (mouse_buttons) {
            int changed = mouse_buttons;
            mouse_buttons = 0;
            port_mouse_event(0, 0, 0, 0, changed | 0x100);
        }
        break;
    }
}

int port_run(game_entry fn, int argc, char **argv)
{
    SDL_Thread *th;
    uint64_t quit_at = 0, debug_next = 0;
    int debug_ticks = SDL_getenv("FREEFALL_DEBUG_TICKS") != NULL;
    int scale = 3;
    const char *s;

    entry = fn;
    g_argc = argc;
    g_argv = argv;
    main_thread_id = SDL_GetCurrentThreadID();
    if ((s = SDL_getenv("FREEFALL_SHOT")) != NULL && *s) {
        shot_prefix = s;
        s = SDL_getenv("FREEFALL_SHOT_EVERY");
        shot_every = (uint64_t)(s ? atol(s) : 1000) * 1000;
    }
    if ((s = SDL_getenv("FREEFALL_SCRIPT")) != NULL && *s)
        script = SDL_strdup(s);
    shot_window = (s = SDL_getenv("FREEFALL_SHOT_WINDOW")) != NULL && *s == '1';

    {   /* FREEFALL_WINDOW=WxH: the window's first size (test screenshots at a given size) */
        int ww = 320 * scale, wh = 240 * scale;
        if ((s = SDL_getenv("FREEFALL_WINDOW")) != NULL && strchr(s, 'x') && atoi(s) > 0 &&
            atoi(strchr(s, 'x') + 1) > 0) {
            ww = atoi(s);
            wh = atoi(strchr(s, 'x') + 1);
        }
        /* Fullscreen on (tnfree.ini, read before this): full screen from the start; leaving it
         * gives the window this size */
        fullscreen = port_fullscreen;
        window = SDL_CreateWindow("Terra Nova: Freefall " FREEFALL_VERSION_FULL, ww, wh,
                                  SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
        if (!window && fullscreen) {    /* full screen failed: a window */
            port_log("Fullscreen: cannot start full screen (%s), a window instead\n", SDL_GetError());
            fullscreen = port_fullscreen = 0;
            window = SDL_CreateWindow("Terra Nova: Freefall " FREEFALL_VERSION_FULL, ww, wh,
                                      SDL_WINDOW_RESIZABLE);
        }
    }
    if (!window)
        port_fatal("Cannot open the window: %s\n", SDL_GetError());
    make_renderer();
    SDL_HideCursor();

    port_input_init();
    /* RIFE loads while the game starts, but only once the window and its renderer are made: its
     * thread loading the Vulkan DLLs while this one made the window could deadlock in Windows'
     * loader (seen under a debugger, which slows every DLL load) */
    if (port_smooth_videos && !port_bench)
        vsmooth_start(0, -1);
    {                                   /* the game thread, with a 16 MB stack */
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER, (void *)game_thread);
        SDL_SetStringProperty(props, SDL_PROP_THREAD_CREATE_NAME_STRING, "game");
        SDL_SetNumberProperty(props, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER, 16 << 20);
        th = SDL_CreateThreadWithProperties(props);
        SDL_DestroyProperties(props);
    }
    if (!th)
        port_fatal("Cannot start the game thread: %s\n", SDL_GetError());
    SDL_DetachThread(th);
    /* the window's thread ahead of the drawing threads, which have every core but one: it waited
     * for a turn and missed refreshes (P59) */
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH);

    while (!game_done) {
        SDL_Event e;
        uint64_t next = port_now_us() + 16000;
        while (SDL_PollEvent(&e))
            handle_event(&e);
        if (script)
            run_script();
        if (warp_pending)
            do_warp();
        wheel_tick();
        ml_update();
        if (!fullscreen != !port_fullscreen) {  /* the options screen's Fullscreen or Alt+Enter */
            fullscreen = port_fullscreen;
            if (!SDL_SetWindowFullscreen(window, fullscreen) && fullscreen) {
                port_log("Fullscreen: cannot go full screen (%s), the window stays\n", SDL_GetError());
                fullscreen = port_fullscreen = 0;
            }
        }
        SDL_SetAtomicInt(&in_present, 1);
        if (!SDL_GetAtomicInt(&game_ending)) {
            present();
            SDL_SetAtomicInt(&in_present, 0);
            display_refreshed(display_hz());
        } else {                        /* the game's exit functions run: no drawing (P113) */
            SDL_SetAtomicInt(&in_present, 0);
            SDL_Delay(5);
        }
        if (debug_ticks && port_now_us() > debug_next) {
            extern unsigned long tn_ticks, game_frame, game_frame_dt;
            extern volatile unsigned long port_div_traps;   /* divtrap.c: each one a trip through a debugger */
            debug_next = port_now_us() + 1000000;
            port_log("tick %lu frame %lu dt %lu div traps %lu\n", tn_ticks, game_frame, game_frame_dt,
                     port_div_traps);
        }
        if (port_quit_requested && !quit_at)
            quit_at = port_now_us() + 3000000;
        if (quit_at && port_now_us() > quit_at) {
            port_log("The game did not stop after the window was closed; closing it.\n");
            break;
        }
        /* vsync paces this loop; without it (or minimised), don't spin */
        while (port_now_us() + 1000 < next && !(SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) == 0)
            SDL_Delay(5);
        if (port_now_us() + 2000 < next && shot_prefix)
            SDL_Delay(1);
    }
    port_audio_shutdown();
    vsmooth_stop();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    if (port_bench_report[0])           /* the benchmark's results (bench.c) */
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Terra Nova: Freefall benchmark",
                                 port_bench_report, NULL);
    SDL_Quit();
    return game_status;
}

int port_platform_init(void)
{
    t0 = 0;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK)) {
        port_log("SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }
    t0 = SDL_GetTicksNS() / 1000;
    irq_mutex = SDL_CreateMutex();
    disp_mutex = SDL_CreateMutex();
    disp_cond = SDL_CreateCondition();
    port_video.vram = (uint8_t *)SDL_calloc(PORT_VRAM_KB, 1024);
    port_video.dac_bits = 6;
    return 0;
}

