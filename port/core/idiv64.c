/* idiv64.c -- the 64-bit divisions of the decompiled files (see port/include/port_libc.h).
 *
 * The original divides a 64-bit edx:eax by a 32-bit operand with idiv / div; a quotient that
 * does not fit 32 bits (or a divisor of 0) raises a divide error, and the game's handler gives
 * a saturated result (port/core/divtrap.c does the same here).  The decompiled C writes these as
 * 64-bit divisions, which GCC turns into calls of __divdi3 / __udivdi3: correct quotients, never
 * a fault.  The decompiled files call these two instead.  A divisor that does not fit 32 bits
 * cannot come from the original's code; it gets a plain 64-bit division.
 *
 * The overflow is found before dividing and given the handler's result (and counted, as the
 * handler does), so no divide error is raised: under a debugger every one stopped the whole
 * program for a trip through the debugger (the sky's divisions overflow every frame). */
#include <stdint.h>

long long tn_divdi3(long long n, long long d);
unsigned long long tn_udivdi3(unsigned long long n, unsigned long long d);
void port_count_div_fault(void);        /* divtrap.c */

long long tn_divdi3(long long n, long long d)
{
    uint64_t un;
    uint32_t ud, uq, r;
    int neg;
    if (d != (int32_t)d)
        return n / d;
    /* IDIV by the magnitudes: the quotient truncates toward 0, so its sign can be put back */
    neg = (n < 0) != (d < 0);
    un = n < 0 ? 0 - (uint64_t)n : (uint64_t)n;
    ud = d < 0 ? 0u - (uint32_t)d : (uint32_t)d;
    if ((uint32_t)(un >> 32) < ud) {    /* DIV below cannot fault: the quotient fits 32 bits */
        __asm__ ("divl %4" : "=a"(uq), "=d"(r) : "0"((uint32_t)un), "1"((uint32_t)(un >> 32)), "r"(ud) : "cc");
        (void)r;
        if (uq <= (neg ? 0x80000000u : 0x7FFFFFFFu))
            return (int32_t)(neg ? 0u - uq : uq);
    }
    /* the divide error: the handler's result for IDIV (divtrap.c fix_divide), by the signs of
     * edx (the dividend's high dword) and the divisor */
    port_count_div_fault();
    return (int32_t)(((int32_t)(n >> 32) >= 0) == ((int32_t)d >= 0) ? 0x7FFFFFFFu : 0x80000001u);
}

unsigned long long tn_udivdi3(unsigned long long n, unsigned long long d)
{
    uint32_t q, r;
    if (d >> 32)
        return n / d;
    if ((uint32_t)(n >> 32) >= (uint32_t)d) {   /* the divide error: DIV's result is all ones */
        port_count_div_fault();
        return 0xFFFFFFFFu;
    }
    __asm__ ("divl %4" : "=a"(q), "=d"(r)
             : "0"((uint32_t)n), "1"((uint32_t)(n >> 32)), "r"((uint32_t)d) : "cc");
    (void)r;
    return q;
}

/* a 64-bit division that never faults (src/3d/points.c checks the overflow itself) */
long long tn_ldiv_exact(long long n, long long d)
{
    return n / d;
}
