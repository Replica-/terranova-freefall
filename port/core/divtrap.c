/* divtrap.c -- the divide-error handler of the original (0x2DF9E9, installed for the whole
 * program by g3a_div0_install), for the standalone build.  The decompiled fixed-point code keeps
 * the original's DIV / IDIV instructions, whose quotient can overflow (or whose divisor can be
 * 0); on DOS the handler then skipped the instruction with a saturated result instead of
 * stopping the game:
 *   DIV:  eax = edx = 0xFFFFFFFF, for the 8- and 16-bit forms too
 *   IDIV: same signs of dividend (edx) and divisor: eax = 0x7FFFFFFF, edx = 0xFFFFFFFF;
 *         different: eax = 0x80000001, edx = 0 (16-bit: 0x7FFF/0xFFFF, 0x8001/0; 8-bit: al)
 * and the entry stub (0x2DF910) counted every fault in the dword at 0x35F464.
 * Here the CPU exception arrives as SIGFPE (Linux) or a structured exception (Windows), and the
 * same is done to the thread's saved registers.
 *
 * Any other crash (a bad memory access, ...) is written to the log with the decompiled routine
 * it happened in and the routines found on the stack, before the program ends as usual. */
#ifndef _WIN32
#define _GNU_SOURCE                     /* REG_EIP & co. in ucontext.h */
#endif
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "port.h"

typedef struct {
    uint32_t *eax, *ecx, *edx, *ebx, *esp, *ebp, *esi, *edi, *eip;
} Regs;

static uint32_t reg32(const Regs *r, int n)
{
    uint32_t *t[8] = { r->eax, r->ecx, r->edx, r->ebx, r->esp, r->ebp, r->esi, r->edi };
    return *t[n & 7];
}

/* the divisor's value and the instruction length; 0 if this is not a DIV / IDIV */
static int decode(const Regs *r, const uint8_t *p, int *is_idiv, int *size, int32_t *divisor)
{
    const uint8_t *s = p;
    int opsize16 = 0, mod, reg, rm;
    uint32_t addr = 0;

    for (;;) {
        if (*p == 0x66)
            opsize16 = 1;
        else if (*p != 0x2E && *p != 0x36 && *p != 0x3E && *p != 0x26 && *p != 0x64 && *p != 0x65)
            break;
        p++;
    }
    if (*p != 0xF6 && *p != 0xF7)
        return 0;
    *size = *p == 0xF6 ? 8 : opsize16 ? 16 : 32;
    p++;
    mod = *p >> 6;
    reg = (*p >> 3) & 7;
    rm = *p & 7;
    p++;
    if (reg != 6 && reg != 7)
        return 0;
    *is_idiv = reg == 7;
    if (mod == 3) {
        uint32_t v = reg32(r, rm & (*size == 8 ? 3 : 7));
        if (*size == 8 && rm >= 4)
            v >>= 8;                    /* ah, ch, dh, bh */
        *divisor = (int32_t)v;
    } else {
        if (rm == 4) {                  /* SIB */
            int ss = *p >> 6, idx = (*p >> 3) & 7, base = *p & 7;
            p++;
            if (idx != 4)
                addr += reg32(r, idx) << ss;
            if (base == 5 && mod == 0) {
                addr += p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
                p += 4;
            } else
                addr += reg32(r, base);
        } else if (rm == 5 && mod == 0) {
            addr = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
            p += 4;
        } else
            addr = reg32(r, rm);
        if (mod == 1) {
            addr += (uint32_t)(int32_t)(int8_t)p[0];
            p += 1;
        } else if (mod == 2) {
            addr += p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
            p += 4;
        }
        if (*size == 8)
            *divisor = *(int8_t *)(uintptr_t)addr;
        else if (*size == 16)
            *divisor = *(int16_t *)(uintptr_t)addr;
        else
            *divisor = *(int32_t *)(uintptr_t)addr;
    }
    if (*size == 8 && mod == 3)
        *divisor = (int8_t)*divisor;
    else if (*size == 16 && mod == 3)
        *divisor = (int16_t)*divisor;
    return (int)(p - s);
}

/* divide errors the CPU raised (each one a trip through a debugger, when there is one; the
 * 64-bit divisions find theirs without one: idiv64.c) */
volatile unsigned long port_div_traps;

/* the original's divide-error entry stub counts every fault in the dword at 0x35F464 */
void port_count_div_fault(void)
{
    (*(uint32_t *)(tn_data_obj + (0x35F464 - TN_DATA_BASE)))++;
}

static int fix_divide(Regs *r)
{
    int is_idiv, size, len;
    int32_t divisor;
    const uint8_t *ip = (const uint8_t *)(uintptr_t)*r->eip;

    len = decode(r, ip, &is_idiv, &size, &divisor);
    if (!len)
        return 0;
    port_div_traps++;
    port_count_div_fault();
    if (!is_idiv) {
        /* the original sets al, dl, then ax, dx, then falls through to eax, edx for every size */
        *r->eax = *r->edx = 0xFFFFFFFF;
    } else if (size == 32) {
        int same = ((int32_t)*r->edx >= 0) == (divisor >= 0);
        *r->eax = same ? 0x7FFFFFFF : 0x80000001;
        *r->edx = same ? 0xFFFFFFFF : 0;
    } else if (size == 16) {
        int same = ((int16_t)*r->edx >= 0) == (divisor >= 0);
        *r->eax = (*r->eax & 0xFFFF0000) | (same ? 0x7FFF : 0x8001);
        *r->edx = (*r->edx & 0xFFFF0000) | (same ? 0xFFFF : 0);
    } else {
        int same = ((int16_t)*r->eax >= 0) == (divisor >= 0);
        *r->eax = (*r->eax & 0xFFFF0000) | (same ? 0xFF7F : 0x0081);
    }
    *r->eip += len;
    return 1;
}


/* the decompiled routine whose code holds host address a: the nearest one at or below it */
static const char *routine_at(uintptr_t a, uintptr_t *off)
{
    const char *best = 0;
    uintptr_t at = 0;
    int i;

    for (i = 0; i < port_nfuncs; i++) {
        uintptr_t f = (uintptr_t)port_funcs[i].fn;
        if (f <= a && f > at) {
            at = f;
            best = port_funcs[i].name;
        }
    }
    if (!best || a - at > 0x10000)
        return 0;
    *off = a - at;
    return best;
}

const char *port_routine_at(const void *a, unsigned long *off)
{
    uintptr_t o = 0;
    const char *n = routine_at((uintptr_t)a, &o);
    *off = (unsigned long)o;
    return n;
}

static void crash_report(const char *what, uint32_t eip, uint32_t esp)
{
    static int done;
    const char *n;
    uintptr_t off, lo = (uintptr_t)-1, hi = 0;
    int i, shown = 0;

    if (done++)
        return;
    for (i = 0; i < port_nfuncs; i++) {
        uintptr_t f = (uintptr_t)port_funcs[i].fn;
        if (f < lo)
            lo = f;
        if (f > hi)
            hi = f;
    }
    n = routine_at(eip, &off);
    port_log("CRASH: %s at %08lX", what, (unsigned long)eip);
    if (n)
        port_log(" in %s+0x%lX", n, (unsigned long)off);
    port_log("\nCalled from (routines found on the stack, nearest first):\n");
    for (i = 0; i < 2048 && shown < 16; i++) {
        uint32_t v = ((const uint32_t *)(uintptr_t)esp)[i];
        if (v < lo || v > hi + 0x10000 || !(n = routine_at(v, &off)) || off == 0)
            continue;
        port_log("  %s+0x%lX\n", n, (unsigned long)off);
        shown++;
    }
}

#ifdef _WIN32
#include <windows.h>

static LONG CALLBACK on_exception(EXCEPTION_POINTERS *e)
{
    DWORD code = e->ExceptionRecord->ExceptionCode;
    CONTEXT *c = e->ContextRecord;
    Regs r;

    if (code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_INT_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;
    r.eax = (uint32_t *)&c->Eax; r.ecx = (uint32_t *)&c->Ecx; r.edx = (uint32_t *)&c->Edx;
    r.ebx = (uint32_t *)&c->Ebx; r.esp = (uint32_t *)&c->Esp; r.ebp = (uint32_t *)&c->Ebp;
    r.esi = (uint32_t *)&c->Esi; r.edi = (uint32_t *)&c->Edi; r.eip = (uint32_t *)&c->Eip;
    return fix_divide(&r) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI on_crash(EXCEPTION_POINTERS *e)
{
    char what[64];
    DWORD code = e->ExceptionRecord->ExceptionCode;

    if (code == EXCEPTION_ACCESS_VIOLATION)
        snprintf(what, sizeof what, "bad memory %s at %08lX",
                 e->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                 (unsigned long)e->ExceptionRecord->ExceptionInformation[1]);
    else
        snprintf(what, sizeof what, "exception %08lX", (unsigned long)code);
    crash_report(what, e->ContextRecord->Eip, e->ContextRecord->Esp);
    return EXCEPTION_CONTINUE_SEARCH;
}

void port_divtrap_install(void)
{
    AddVectoredExceptionHandler(1, on_exception);
    SetUnhandledExceptionFilter(on_crash);
}

#else
#include <signal.h>
#include <stdlib.h>
#include <ucontext.h>

static void on_sigfpe(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = ctx;
    greg_t *g = uc->uc_mcontext.gregs;
    Regs r;

    (void)si;
    r.eax = (uint32_t *)&g[REG_EAX]; r.ecx = (uint32_t *)&g[REG_ECX]; r.edx = (uint32_t *)&g[REG_EDX];
    r.ebx = (uint32_t *)&g[REG_EBX]; r.esp = (uint32_t *)&g[REG_ESP]; r.ebp = (uint32_t *)&g[REG_EBP];
    r.esi = (uint32_t *)&g[REG_ESI]; r.edi = (uint32_t *)&g[REG_EDI]; r.eip = (uint32_t *)&g[REG_EIP];
    if (!fix_divide(&r)) {
        crash_report("division error", (uint32_t)g[REG_EIP], (uint32_t)g[REG_ESP]);
        signal(sig, SIG_DFL);           /* not a division: crash as usual */
        raise(sig);
    }
}

static void on_crash(int sig, siginfo_t *si, void *ctx)
{
    greg_t *g = ((ucontext_t *)ctx)->uc_mcontext.gregs;
    char what[64];

    snprintf(what, sizeof what, "signal %d, address %08lX", sig, (unsigned long)(uintptr_t)si->si_addr);
    crash_report(what, (uint32_t)g[REG_EIP], (uint32_t)g[REG_ESP]);
    signal(sig, SIG_DFL);
    raise(sig);
}

void port_divtrap_install(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sigfpe;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGFPE, &sa, NULL);
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
}
#endif
