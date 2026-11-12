/* EE COP0 Count: the CPU clock, 294.912 MHz (294912 ticks per ms). It wraps every 14.6 s, so only differences of two
 * reads less than that apart mean anything. Under PCSX2 a read ends the recompiler block (mfc0 timer cost, PR #143):
 * keep reads out of inner loops. Host builds of a source file (offline harnesses, no _EE) read 0. */
#ifndef TICKS_H
#define TICKS_H

static inline unsigned ticks(void)
{
#ifdef _EE
    unsigned c;
    __asm__ volatile("mfc0 %0, $9" : "=r"(c));
    return c;
#else
    return 0;
#endif
}

#endif
