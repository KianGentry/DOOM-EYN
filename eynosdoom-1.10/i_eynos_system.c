/*
 * EYN-OS system interface for DOOM (linuxdoom-1.10).
 *
 * Replaces i_system.c.  Key differences from the Linux original:
 *
 *   - mb_used is reduced from 6 to 4.  QEMU boots EYN-OS with 9 MB of RAM;
 *     after the kernel, roughly 5–6 MB of physical memory remains.  4 MB
 *     leaves comfortable headroom while being large enough for DOOM to run
 *     the shareware episode.  Increase QEMU -m and this value together if
 *     wads with more resources are used.
 *
 *   - No signal() registration.  EYN-OS ring-3 processes do not receive
 *     POSIX signals; SIGINT handling is omitted.
 *
 *   - D_QuitNetGame() is not called from I_Quit / I_Error because DOOM
 *     networking is disabled on this platform.
 *
 *   - G_CheckDemoStatus() in I_Error is retained so demo recording stops
 *     cleanly before the fatal message is printed.
 *
 *   - I_WaitVBL, I_BeginRead, I_EndRead, I_StartFrame, I_StartTic are
 *     implemented in i_eynos_video.c, not here, because they belong to the
 *     display / input subsystem.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <sys/time.h>
#include <unistd.h>

#include "doomdef.h"
#include "m_misc.h"
#include "i_video.h"
#include "i_sound.h"
#include "g_game.h"
#include "i_system.h"

/*
 * ABI-INVARIANT: Zone allocator size in megabytes.
 *
 * Why: DOOM's zone allocator (z_zone.c) uses a single contiguous slab whose
 * size is fixed at start-up by I_ZoneBase().  All dynamic allocations during
 * the game come from this region; if it is exhausted DOOM calls I_Error.
 *
 * Implementation note: the backing store is a static .bss array
 * (doom_zone_buf) rather than a malloc() allocation.  The userland bump
 * allocator has a fixed 1 MB pool (USERLAND_HEAP_SIZE in stdlib.c) shared
 * by all small allocations; requesting several megabytes through malloc()
 * always fails regardless of QEMU's -m ceiling.  Using a BSS region sidesteps
 * this limit: the pages are demand-zero and are only backed by physical RAM
 * as DOOM writes into them, which is consistent with the low-memory design
 * constraint.
 *
 * Invariant: DOOM_ZONE_MB * 1 MiB of virtual address space is reserved in
 *            the userland BSS.  Physical pages are faulted in lazily, so the
 *            practical RAM cost is the number of zone pages DOOM actually
 *            touches.  The registered DOOM1 episode needs roughly 4–5 MB of
 *            zone; set DOOM_ZONE_MB >= 5 and QEMU -m large enough to hold the
 *            kernel (~3 MB) + zone pages + WAD cache.
 *
 * Breakage if decreased below ~4 MB: Z_Malloc failures or mid-game crashes.
 * ABI-sensitive: No.  Security-critical: No.
 */
#define DOOM_ZONE_MB 6
static byte doom_zone_buf[DOOM_ZONE_MB * 1024 * 1024];
int mb_used = DOOM_ZONE_MB;

/* Unused tactile feedback stub required by the i_system.h interface. */
void I_Tactile(int on, int off, int total)
{
    (void)on; (void)off; (void)total;
}

ticcmd_t emptycmd;

ticcmd_t* I_BaseTiccmd(void)
{
    return &emptycmd;
}

int I_GetHeapSize(void)
{
    return (int)sizeof(doom_zone_buf);
}

byte* I_ZoneBase(int* size)
{
    /*
     * Return the static BSS backing store directly; no malloc() call.
     * See the DOOM_ZONE_MB invariant comment above for rationale.
     */
    *size = (int)sizeof(doom_zone_buf);
    return doom_zone_buf;
}

/*
 * I_GetTime — returns elapsed time in 1/TICRATE second units.
 *
 * TICRATE is 35 (defined in doomdef.h).  A direct port of the original
 * Linux implementation; works correctly with our gettimeofday() shim.
 */
int I_GetTime(void)
{
    struct timeval  tp;
    struct timezone tzp;
    static int      basetime = 0;

    gettimeofday(&tp, &tzp);
    if (!basetime)
        basetime = (int)tp.tv_sec;

    return (int)((tp.tv_sec - basetime) * TICRATE +
                 tp.tv_usec * TICRATE / 1000000);
}

/* I_Init — called once at start-up; initialise platform subsystems. */
void I_Init(void)
{
    I_InitSound();
    /* I_InitGraphics() is called later by D_DoomMain after R_Init. */
}

/* I_Quit — clean shutdown path invoked by DOOM's quit menu option.
 *
 * Original calls D_QuitNetGame() and M_SaveDefaults() before exiting.
 * We retain M_SaveDefaults() so config changes are written back to
 * default.cfg on the EYNFS image. */
void I_Quit(void)
{
    I_ShutdownSound();
    I_ShutdownMusic();
    M_SaveDefaults();
    I_ShutdownGraphics();
    _exit(0);
}

byte* I_AllocLow(int length)
{
    byte* mem = (byte*)malloc(length);
    if (mem)
        memset(mem, 0, length);
    return mem;
}

/*
 * I_Error — print a fatal error message and terminate.
 *
 * Intentionally minimal: print message to stderr, attempt a clean graphics
 * shutdown so the terminal is left in a readable state, then exit.
 */
void I_Error(char* error, ...)
{
    va_list argptr;

    /* Print the message before any teardown so it is always visible. */
    va_start(argptr, error);
    fprintf(stderr, "Error: ");
    vfprintf(stderr, error, argptr);
    fprintf(stderr, "\n");
    va_end(argptr);
    fflush(stderr);

    /* Attempt to preserve a demo recording if one was in progress. */
    if (demorecording)
        G_CheckDemoStatus();

    I_ShutdownGraphics();
    _exit(-1);
}
