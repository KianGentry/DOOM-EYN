/*
 * EYN-OS network interface for DOOM (linuxdoom-1.10) — stub implementation.
 *
 * Multiplayer networking is not supported on EYN-OS.  I_InitNetwork allocates
 * a minimal single-player doomcom_t so that D_CheckNetGame (which immediately
 * dereferences doomcom after the call) does not crash.  The structure is
 * initialised for a 1-node / 1-player game with ticdup=1 and all other flags
 * zeroed; DOOM will then run in single-player mode throughout.
 */

#include <stdlib.h>  /* malloc */
#include <string.h>  /* memset */

#include "doomdef.h"
#include "i_net.h"
#include "d_net.h"    /* doomcom_t, DOOMCOM_ID */
#include "doomstat.h" /* doomcom (extern doomcom_t*) */

/* Called by D_DoomMain during start-up.
 *
 * D_CheckNetGame dereferences doomcom immediately after this returns,
 * checking for the DOOMCOM_ID magic at offset 0.  A NULL doomcom causes an
 * instant page fault.  We allocate a zeroed structure and populate the
 * minimum fields required for single-player:
 *
 *   id            - DOOMCOM_ID magic (checked at +0x00 before anything else)
 *   numnodes      - 1  (only us)
 *   ticdup        - 1  (no duplication; DOOM divides ticlen by ticdup)
 *   consoleplayer - 0  (player 0)
 *   numplayers    - 1
 *
 * Everything else stays 0: no deathmatch, no savegame, single episode/map
 * (DOOM's command-line parser will override episode/map/skill anyway).
 */
void I_InitNetwork(void)
{
    doomcom_t* dc = (doomcom_t*)malloc(sizeof(doomcom_t));
    if (!dc)
        I_Error("I_InitNetwork: out of memory allocating doomcom");

    memset(dc, 0, sizeof(doomcom_t));

    dc->id           = DOOMCOM_ID;
    dc->numnodes     = 1;
    dc->ticdup       = 1;
    dc->consoleplayer = 0;
    dc->numplayers   = 1;

    doomcom = dc;
}

/* Called by the network loop to exchange tic commands.
 * No-op: single-player, no remote nodes to communicate with. */
void I_NetCmd(void)
{
}
