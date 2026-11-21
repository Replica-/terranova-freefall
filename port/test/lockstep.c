/* lockstep.c -- deterministic runs for the frame-by-frame comparison with the original (step 4).
 *
 * verify/port_compare.py runs the original __FF.EXE in the emulator (verify/frame.py) and this
 * program side by side and compares them frame by frame.  For that both must see exactly the
 * same world, so with FREEFALL_LOCKSTEP set the port does what frame.py's Game does:
 *   - no timer interrupt: the Miles timers never fire (miles_timer.c), the game's 250 Hz clock
 *     (tn_ticks) advances by 0x32 at the start of every frame, and GameTimeFrame's frame limiter
 *     ends at once with the clock at its end (gametime.c);
 *   - no real input: keys go straight into the keyboard handler's buffer and the mouse into the
 *     driver, at the start of the frame the input script names, the way frame.py's key() and
 *     mouse() do;
 *   - the mission is loaded the way Game.start_mission does it: at the first frame of the main
 *     loop the shell thread is killed, the mission number set, the shell's natives 31 and 44
 *     run and the shell script's mission load function started; at the next frame its
 *     mission-start function.
 * From frame FREEFALL_LOCKSTEP_FROM on, the state at the start of every frame (the original's
 * whole data object, the screen and the palette) is appended to the file FREEFALL_LOCKSTEP;
 * after FREEFALL_LOCKSTEP_FRAMES frames the program quits.
 *
 *   FREEFALL_LOCKSTEP=file           turns lockstep on; the frame dumps go there ("-": no dumps)
 *   FREEFALL_LOCKSTEP_SHOTS=n,n,...  the whole window as shown saved at the start of these frames
 *                                    (FREEFALL_LOCKSTEP_SHOT=prefix, + "fNNNNN.bmp"), then the
 *                                    program quits
 *   FREEFALL_LOCKSTEP_MISSION=n      mission to start (default 1; 0 = stay in the shell)
 *   FREEFALL_LOCKSTEP_FROM=n         first frame to dump (default 2, where frame.py's mission
 *                                    snapshot is taken)
 *   FREEFALL_LOCKSTEP_FRAMES=n       frames to dump (default 50)
 *   FREEFALL_LOCKSTEP_INPUT=script   "frame:action;..." with action d<scan> / u<scan> (key down /
 *                                    up, set-1 scan code in hex, | 80 for E0 keys), t<scan> (both),
 *                                    m<x>,<y> (mouse driver position), b<buttons> (bit 0 left,
 *                                    bit 1 right), w<addr>,<value> (write a dword of the
 *                                    original's data, both hex: e.g. the mission number),
 *                                    s<x>,<y> (widescreen: the pointer beside the screen at game
 *                                    pixel x,y, decimal), S (back on the screen), g (gimbal mode
 *                                    on / off), L<turn>,<look> (the mouse's head turn and look
 *                                    in angle units, decimal, as mouselook gives them)
 *   FREEFALL_LOCKSTEP_HEAP=k         every k frames, the heap blocks too (Linux build)
 *   FREEFALL_LOCKSTEP_ALIGN=list     "size:low;..." blocks placed like the original's, modulo
 *                                    64 KB (Linux build; see aligned_malloc)
 * Fresh heap blocks are filled with CD bytes and freed ones with DD, as the emulator's heap
 * does (Linux build; CMakeLists.txt wraps malloc), so code that reads memory it never wrote sees
 * the same bytes on both sides.
 * Written next to the dump: <file>.map, one "port address  original address" line per routine,
 * and the address ranges the comparison needs to tell pointers apart.
 *
 * The benchmark (tnfree --bench, bench.c) runs the same way, without the dumps and the heap's
 * fill bytes, its clock 10 ticks on a frame (the original's 25 frames a second) and its input
 * from bench.c. */
#ifndef _WIN32
#define _GNU_SOURCE                     /* pthread_getattr_np */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include "port.h"
#include "sharp3d.h"
#ifndef _WIN32
#include <pthread.h>
#endif

void port_exit(int code);

int port_lockstep;

static FILE *dump;
static long mission = 1, dump_from = 2, dump_frames = 50;
static char *input;
static unsigned long nframe;
static long heap_every;
static const char *shots, *shot_prefix;    /* FREEFALL_LOCKSTEP_SHOTS, FREEFALL_LOCKSTEP_SHOT */
static uint64_t shots_t0;
static int cmp_heap;                    /* the comparison's heap: fill bytes, own allocator (not the benchmark) */
static void heap_init(void);
static void write_heap(void);
#ifndef _WIN32
static void align_init(const char *s);
#endif

#define TICKS_PER_FRAME 0x32
static long tpf = TICKS_PER_FRAME;      /* the clock's ticks a frame */
#define TN_TICKS        0x35F8DC        /* the 250 Hz clock */
#define SHELL_THREAD    0x36F4B4
#define MISSION_NO      0x359708        /* script engine variable 7 */
#define IN_MISSION      0x35972E        /* game_in_mission (byte): cleared at the mission's end */
#define WEATHER         0x35CADA        /* game_35CADA (byte): 0 rain, 1 snow, 0xFF none */
#define RES_DESC        0x446BE8        /* gResDesc */
#define KBD_LOWMEM      0x45D62C        /* kbd_lowmem_start */
#define F_SCRIPT_START  0x2E7FF4
#define F_SCRIPT_KILL   0x2E8154
#define F_RES_LOCK      0x2EB380
#define F_RES_UNLOCK    0x2EB3F4
#define F_NATIVE_31     0x2DD78C        /* shell resources closed, restnobj.res opened */
#define F_NATIVE_44     0x279C00        /* default equipment */

#define U32(a) (*(volatile unsigned long *)port_ptr(a))

typedef long (*fn1)(long);

static long call1(unsigned long addr, long a)
{
    fn1 f = (fn1)port_func_at(addr);
    if (!f)
        port_fatal("lockstep: no routine at %06lX\n", addr);
    return f(a);
}

void port_mouse_raw(int x, int y, int buttons);

/* ------------------------------------------------------------------ input (as frame.py) */
void port_lockstep_key(int code, int down)
{
    unsigned char *b = *(unsigned char **)port_ptr(KBD_LOWMEM);
    unsigned st, head;

    if (!b)
        return;
    code &= 0xFF;
    st = b[0x410 + code];
    if (down) {
        if ((st & 1) && !(st & 2))
            return;                     /* held: the handler drops repeats */
        b[0x410 + code] = st | 1;
    } else
        b[0x410 + code] = st & ~1u;
    head = *(unsigned short *)b;
    *(unsigned short *)(b + head) = (down ? 0x100 : 0) | code;
    head += 2;
    if (head >= 0x410)
        head = 0x10;
    *(unsigned short *)b = head;
}

extern int port_gimbal;                 /* platform.c */
void port_mouselook_add(int turn, int look);    /* platform.c */

static void run_input(void)
{
    char *s = input;
    while (s && *s) {
        char *colon = strchr(s, ':'), *semi = strchr(s, ';');
        if (!colon)
            break;
        if ((unsigned long)atol(s) == nframe) {
            char *a = colon + 1;
            int v = (int)strtol(a + 1, NULL, 16);
            switch (a[0]) {
            case 'd': port_lockstep_key(v, 1); break;
            case 'u': port_lockstep_key(v, 0); break;
            case 't': port_lockstep_key(v, 1); port_lockstep_key(v, 0); break;
            case 'g': port_gimbal = !port_gimbal; break;    /* (a Freefall action: no key) */
            case 'L': {                 /* a mouselook move (angle units, decimal) */
                char *c = strchr(a, ',');
                port_mouselook_add(atoi(a + 1), c ? atoi(c + 1) : 0);
                break;
            }
            case 'm': {
                char *c = strchr(a, ',');
                port_mouse_raw(atoi(a + 1), c ? atoi(c + 1) : 0, -1);
                break;
            }
            case 'b': port_mouse_raw(-1, -1, atoi(a + 1)); break;
            case 's': {                 /* widescreen: the pointer beside the screen at x,y */
                char *c = strchr(a, ',');
                port_wide_ptr_x = atoi(a + 1);
                port_wide_ptr_y = c ? atoi(c + 1) : 0;
                port_wide_ptr = 1;
                port_mouse_raw(port_wide_ptr_x < 0 ? 0 : 319, port_wide_ptr_y, -1);
                break;
            }
            case 'S': port_wide_ptr = 0; break;
            case 'w': {
                char *c = strchr(a, ',');
                U32(strtoul(a + 1, NULL, 16)) = c ? strtoul(c + 1, NULL, 16) : 0;
                break;
            }
            }
        }
        s = semi ? semi + 1 : NULL;
    }
}

/* ------------------------------------------------------------------ mission start (as frame.py) */
/* script functions (res << 16 | index) whose code contains pat, in resources 0x3E8..0x3FF */
static long find_script_function(const unsigned char *pat, int n)
{
    unsigned char *desc = *(unsigned char **)port_ptr(RES_DESC);
    long found = -1;
    int res, count = 0;

    for (res = 0x3E8; res < 0x400; res++) {
        unsigned long size, i;
        unsigned nfun, k;
        unsigned char *code;
        if (!(*(unsigned long *)(desc + 16 * res + 8) >> 4))
            continue;
        size = *(unsigned long *)(desc + 16 * res + 4) >> 8;
        code = (unsigned char *)call1(F_RES_LOCK, res);
        nfun = size >= 2 ? *(unsigned short *)code : 0;
        if (nfun && 2 + 4 * nfun <= size)
            for (i = 0; i + n <= size; i++)
                if (!memcmp(code + i, pat, n)) {
                    unsigned best = 0, bestoff = 0;
                    for (k = 0; k < nfun; k++) {
                        unsigned off = *(unsigned *)(code + 2 + 4 * k);
                        if (off <= i && off >= bestoff) {
                            best = k;
                            bestoff = off;
                        }
                    }
                    found = (long)res << 16 | best;
                    count++;
                }
        call1(F_RES_UNLOCK, res);
    }
    if (count != 1)
        port_fatal("lockstep: %d script functions match, expected 1\n", count);
    return found;
}

static void start_mission_part1(void)
{
    static const unsigned char load_pat[] = { 0x2A, 0x25, 0x00, 0x02 };    /* native 37 = FFLoadMission */
    unsigned long sh = U32(SHELL_THREAD);
    if (sh)
        call1(F_SCRIPT_KILL, (long)sh);
    U32(MISSION_NO) = mission;
    call1(F_NATIVE_31, 0);
    call1(F_NATIVE_44, 0);
    call1(F_SCRIPT_START, find_script_function(load_pat, 4));
}

static void start_mission_part2(void)
{
    static const unsigned char setup_pat[] = { 0x2A, 0x57, 0x00, 0x00 };   /* native 87 = MissionSetup */
    call1(F_SCRIPT_START, find_script_function(setup_pat, 4));
}

/* ------------------------------------------------------------------ dumps */
static void put32(unsigned long v)
{
    fwrite(&v, 4, 1, dump);
}

static void write_frame(void)
{
    int w = port_video.w, h = port_video.h, y;
    put32(0x534C4E54);                  /* "TNLS" */
    put32(nframe);
    put32((unsigned long)w);
    put32((unsigned long)h);
    fwrite(tn_data_obj, 1, TN_DATA_SIZE, dump);
    fwrite(port_video.dac, 1, 768, dump);
    for (y = 0; y < h; y++)
        fwrite(port_video.vram + port_video.start + (size_t)y * port_video.pitch, 1, (size_t)w, dump);
    fflush(dump);
}

static char map_path[1100];

/* the game thread's stack, for pointers to local variables the game leaves in its data */
static void write_stack_range(void)
{
#ifndef _WIN32
    pthread_attr_t a;
    void *lo;
    size_t n;
    FILE *f;
    if (pthread_getattr_np(pthread_self(), &a) != 0)
        return;
    pthread_attr_getstack(&a, &lo, &n);
    pthread_attr_destroy(&a);
    f = fopen(map_path, "a");
    if (f) {
        fprintf(f, "stack %08lx %08lx\n", (unsigned long)lo, (unsigned long)n);
        fclose(f);
    }
#endif
}

static void write_map(const char *path)
{
    char name[1100];
    FILE *f;
    int i;
#ifndef _WIN32
    extern char __executable_start[], etext[], end[];
#endif

    snprintf(name, sizeof name, "%s.map", path);
    snprintf(map_path, sizeof map_path, "%s", name);
    f = fopen(name, "w");
    if (!f)
        port_fatal("lockstep: cannot write %s\n", name);
    fprintf(f, "data %08lx %08lx\n", (unsigned long)tn_data_obj, TN_DATA_SIZE);
    fprintf(f, "code %08lx %08lx\n", (unsigned long)tn_code_obj, TN_CODE_SIZE);
    fprintf(f, "vram %08lx %08lx\n", (unsigned long)port_video.vram, (unsigned long)PORT_VRAM_KB * 1024);
#ifndef _WIN32
    fprintf(f, "text %08lx %08lx\n", (unsigned long)__executable_start, (unsigned long)(etext - __executable_start));
    fprintf(f, "static %08lx %08lx\n", (unsigned long)etext, (unsigned long)(end - etext));
#endif
    for (i = 0; i < port_nfuncs; i++)
        fprintf(f, "fn %08lx %06lx %s\n", (unsigned long)port_funcs[i].fn, port_funcs[i].addr, port_funcs[i].name);
    fclose(f);
}

/* ------------------------------------------------------------------ hooks */
void port_lockstep_init(void)
{
    const char *s = getenv("FREEFALL_LOCKSTEP");
    if (port_bench) {
        port_lockstep = 1;
        port_bench_init(&mission, &tpf);
        return;
    }
    if (!s || !*s)
        return;
    port_lockstep = 1;
    if (strcmp(s, "-")) {               /* "-": no dumps (the shots below only) */
        cmp_heap = 1;
        dump = fopen(s, "wb");
        if (!dump)
            port_fatal("lockstep: cannot write %s\n", s);
        write_map(s);
    }
    shots = getenv("FREEFALL_LOCKSTEP_SHOTS");
    shot_prefix = getenv("FREEFALL_LOCKSTEP_SHOT");
    if ((s = getenv("FREEFALL_LOCKSTEP_MISSION")) != NULL) mission = atol(s);
    if ((s = getenv("FREEFALL_LOCKSTEP_FROM")) != NULL) dump_from = atol(s);
    if ((s = getenv("FREEFALL_LOCKSTEP_FRAMES")) != NULL) dump_frames = atol(s);
    if ((s = getenv("FREEFALL_LOCKSTEP_INPUT")) != NULL) input = strdup(s);
    if ((s = getenv("FREEFALL_LOCKSTEP_HEAP")) != NULL) heap_every = atol(s);
    if (heap_every)
        heap_init();
#ifndef _WIN32
    align_init(getenv("FREEFALL_LOCKSTEP_ALIGN"));
#endif
    port_log("lockstep: mission %ld, frames %ld..%ld\n", mission, dump_from, dump_from + dump_frames - 1);
}

void port_shot_window_sync(const char *name);  /* platform.c */

/* FREEFALL_LOCKSTEP_SHOTS: the window as shown at the start of the listed frames (the picture
 * of the frame before), then the end after the last */
static void take_shots(void)
{
    const char *s = shots;
    long f, last = -1;
    char *end;
    while (*s) {
        f = strtol(s, &end, 10);
        if (end == s)
            break;
        if ((unsigned long)f == nframe) {
            char name[1100];
            snprintf(name, sizeof name, "%sf%05ld.bmp", shot_prefix ? shot_prefix : "", f);
            port_shot_window_sync(name);
        }
        if (f > last)
            last = f;
        s = *end == ',' ? end + 1 : end;
    }
    if (last >= 0 && (long)nframe >= last) {
        /* the frames run as fast as they are drawn: their time is the mission's drawing cost
         * (the shots' waits are a few ms each); the weather 0 rain, 1 snow, 255 none (weather.c) */
        double sec = (port_now_us() - shots_t0) / 1e6;
        port_log("lockstep: shots done, %lu frames in %.1f s (%.2f ms a frame), weather %u\n", nframe, sec,
                 nframe ? sec * 1000 / nframe : 0.0, *(volatile unsigned char *)port_ptr(WEATHER));
        port_exit(0);
    }
}

/* GameTimeFrame, first thing (instead of the frame rate limit) */
void port_lockstep_frame(void)
{
    static int was_in;
    if (nframe == 0)
        shots_t0 = port_now_us();
    if (shots)
        take_shots();
    if (nframe == 0 && dump)
        write_stack_range();
    if (mission && nframe == 0)
        start_mission_part1();
    else if (mission && nframe == 1)
        start_mission_part2();
    else if (mission) {
        /* the mission ended (won, lost, the player killed): the shell script that takes over
         * then was killed at the start, so the mission's script would end it again */
        int in = *(volatile unsigned char *)port_ptr(IN_MISSION);
        if (was_in && !in) {
            if (dump)
                fclose(dump);
            dump = NULL;
            port_log("lockstep: the mission ended at frame %lu\n", nframe);
            if (port_bench)
                port_bench_cut();
            port_exit(0);
        }
        was_in = in;
        if (port_bench)
            port_bench_frame(in);
    }
    if (dump && (long)nframe >= dump_from) {
        if ((long)nframe >= dump_from + dump_frames) {
            fclose(dump);
            dump = NULL;
            port_log("lockstep: done\n");
            port_exit(0);
        }
        write_frame();
        if (heap_every && ((long)nframe - dump_from) % heap_every == 0)
            write_heap();
    }
    U32(TN_TICKS) += tpf;               /* the timer interrupt's work */
    run_input();
    nframe++;
}

/* GameTimeFrame's frame limiter (game_slow: the menus): the wait ends at once, the clock at its
 * end, 0x32 ticks after the frame's start whatever the clock's ticks a frame */
void port_lockstep_wait(unsigned long start)
{
    if (U32(TN_TICKS) < start + TICKS_PER_FRAME)
        U32(TN_TICKS) = start + TICKS_PER_FRAME;
}

/* ------------------------------------------------------------------ heap (Linux)
 * Fresh blocks are filled with CD bytes, freed ones with DD, like the emulator's heap.  Every
 * block is recorded, so the frame dumps can hold the heap too (FREEFALL_LOCKSTEP_HEAP). */
#ifndef _WIN32
#include <malloc.h>
void *__real_malloc(size_t n);
void *__real_realloc(void *p, size_t n);
void __real_free(void *p);
void __wrap_free(void *p);

#define HEAP_SLOTS (1u << 21)
typedef struct { void *p; size_t n; } heap_block;
static heap_block *blocks;              /* open addressing; p == 1: deleted */
static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned slot_of(void *p) { return (unsigned)(((size_t)p >> 3) * 2654435761u) & (HEAP_SLOTS - 1); }

static void note_block(void *p, size_t n)
{
    unsigned i;
    if (!blocks)
        return;
    pthread_mutex_lock(&heap_lock);
    for (i = slot_of(p); blocks[i].p && blocks[i].p != (void *)1; i = (i + 1) & (HEAP_SLOTS - 1))
        ;
    blocks[i].p = p;
    blocks[i].n = n;
    pthread_mutex_unlock(&heap_lock);
}

static void forget_block(void *p)
{
    unsigned i;
    if (!blocks)
        return;
    pthread_mutex_lock(&heap_lock);
    for (i = slot_of(p); blocks[i].p; i = (i + 1) & (HEAP_SLOTS - 1))
        if (blocks[i].p == p) {
            blocks[i].p = (void *)1;
            break;
        }
    pthread_mutex_unlock(&heap_lock);
}

static int by_addr(const void *a, const void *b)
{
    size_t x = (size_t)((const heap_block *)a)->p, y = (size_t)((const heap_block *)b)->p;
    return x < y ? -1 : x > y;
}

static void heap_init(void)
{
    blocks = calloc(HEAP_SLOTS, sizeof *blocks);
}

static void write_heap(void)
{
    heap_block *list;
    unsigned i, n = 0;
    pthread_mutex_lock(&heap_lock);
    list = __real_malloc(HEAP_SLOTS * sizeof *list);
    for (i = 0; i < HEAP_SLOTS; i++)
        if (blocks[i].p && blocks[i].p != (void *)1)
            list[n++] = blocks[i];
    qsort(list, n, sizeof *list, by_addr);
    put32(0x50414548);                  /* "HEAP" */
    put32(n);
    for (i = 0; i < n; i++) {
        put32((unsigned long)list[i].p);
        put32((unsigned long)list[i].n);
        fwrite(list[i].p, 1, list[i].n, dump);
    }
    __real_free(list);
    pthread_mutex_unlock(&heap_lock);
}

/* FREEFALL_LOCKSTEP_ALIGN="size:low;..." (hex): the first block of each listed size gets an
 * address whose low 16 bits are `low`, those of the original's block in the emulator.  The
 * original sometimes stores the low half of a pointer (reading past the end of hview_cols into
 * the next allocation of the ffmem stack pool, which holds pointers into the pool); with the
 * pool at the same address modulo 64 KB those halves agree. */
#define MAX_ALIGN 16
static struct { size_t n; unsigned low; char *p, *raw; } align[MAX_ALIGN];
static int nalign;

static void align_init(const char *s)
{
    while (s && *s && nalign < MAX_ALIGN) {
        char *e;
        align[nalign].n = strtoul(s, &e, 16);
        if (*e != ':')
            break;
        align[nalign++].low = (unsigned)strtoul(e + 1, &e, 16) & 0xFFFF;
        s = *e == ';' ? e + 1 : NULL;
    }
}

/* The game thread's blocks come from an allocator of our own at a fixed address, so that the
 * heap is laid out the same way in every run (with glibc's malloc the other threads' blocks fall
 * in between at random): two runs can then be compared byte for byte, pointers included
 * (verify/state_compare.py).  Size classes of powers of two, each with its free list. */
#include <sys/mman.h>
#define DET_BASE ((char *)0x20000000)
#define DET_SIZE (0x30000000u)
static __thread int det_thread;
static char *det_base, *det_top;
static void *det_free[40];

void port_lockstep_game_thread(void)
{
    det_thread = 1;
}

static int det_class(size_t n)
{
    int c = 4;
    while (((size_t)1 << c) < n + 8)
        c++;
    return c;
}

static int det_owns(void *p)
{
    return det_base && (char *)p >= det_base && (char *)p < det_base + DET_SIZE;
}

static size_t det_size(void *p)
{
    return ((size_t *)p)[-2];
}

static void *det_malloc(size_t n)
{
    int c = det_class(n);
    char *b;
    if (!det_base) {
        det_base = mmap(DET_BASE, DET_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (det_base == MAP_FAILED) {
            det_base = NULL;
            return NULL;
        }
        det_top = det_base;
    }
    if (det_free[c]) {
        b = det_free[c];
        det_free[c] = *(void **)b;
    } else {
        if ((size_t)(det_top - det_base) + ((size_t)1 << c) > DET_SIZE)
            return NULL;
        b = det_top;
        det_top += (size_t)1 << c;
    }
    ((size_t *)b)[0] = n;
    ((size_t *)b)[1] = (size_t)c;
    return b + 8;
}

static void det_release(void *p)
{
    char *b = (char *)p - 8;
    int c = (int)((size_t *)b)[1];
    *(void **)b = det_free[c];
    det_free[c] = b;
}

size_t port_heap_size(void *p)
{
    return det_owns(p) ? det_size(p) : malloc_usable_size(p);
}

static int aligned_index(void *p)
{
    int i;
    for (i = 0; i < nalign; i++)
        if (align[i].p && align[i].p == p)
            return i;
    return -1;
}

static void *aligned_malloc(size_t n)
{
    int i;
    for (i = 0; i < nalign; i++)
        if (align[i].n == n && !align[i].raw) {
            char *raw = __real_malloc(n + 0x10000);
            if (!raw)
                return NULL;
            align[i].raw = raw;
            align[i].p = raw + ((align[i].low - (size_t)raw) & 0xFFFF);
            return align[i].p;
        }
    return NULL;
}

void *__wrap_malloc(size_t n)
{
    void *p = cmp_heap && nalign ? aligned_malloc(n) : NULL;
    if (!p && cmp_heap && det_thread)
        p = det_malloc(n);
    if (!p)
        p = __real_malloc(n);
    if (p && cmp_heap) {
        memset(p, 0xCD, n);
        note_block(p, n);
    }
    return p;
}

void *__wrap_realloc(void *p, size_t n)
{
    int a = p && cmp_heap ? aligned_index(p) : -1;
    if (a >= 0) {                       /* a moved copy, without the alignment */
        void *q = n ? __wrap_malloc(n) : NULL;
        if (q)
            memcpy(q, p, n < align[a].n ? n : align[a].n);
        if (q || !n)
            __wrap_free(p);
        return q;
    }
    if (p && det_owns(p)) {
        size_t old = det_size(p);
        void *q = n ? __wrap_malloc(n) : NULL;
        if (q)
            memcpy(q, p, n < old ? n : old);
        if (q || !n)
            __wrap_free(p);
        return q;
    }
    size_t old = p && cmp_heap ? malloc_usable_size(p) : 0;
    void *q;
    if (p && cmp_heap)
        forget_block(p);
    q = __real_realloc(p, n);
    if (q && cmp_heap) {
        if (n > old)
            memset((char *)q + old, 0xCD, n - old);
        note_block(q, n);
    }
    return q;
}

void __wrap_free(void *p)
{
    int a = p && cmp_heap ? aligned_index(p) : -1;
    if (a >= 0) {
        forget_block(p);
        memset(p, 0xDD, align[a].n);
        __real_free(align[a].raw);
        align[a].p = NULL;              /* raw stays set: the entry is used up */
        return;
    }
    if (p && det_owns(p)) {
        forget_block(p);
        memset(p, 0xDD, det_size(p));
        det_release(p);
        return;
    }
    if (p && cmp_heap) {
        forget_block(p);
        memset(p, 0xDD, malloc_usable_size(p));
    }
    __real_free(p);
}
#else
void port_lockstep_game_thread(void) {}
static void heap_init(void) {}
static void write_heap(void) { put32(0x50414548); put32(0); }
#endif
