/*
 * EYN-OS sound backend for DOOM (linuxdoom-1.10) — all functions are stubs.
 *
 * EYN-OS does not currently expose a PCM audio API to ring-3 programs.
 * All sound and music functions are implemented as no-ops so that the rest
 * of DOOM compiles and runs correctly without audio output.  When an audio
 * syscall is added to EYN-OS, this file is the right place to integrate it.
 *
 * Functions exported here must match the signatures in i_sound.h exactly.
 *
 * sndserver_filename and sndserver are defined here because i_sound.h only
 * declares them under #ifdef SNDSERV, which we disabled.  m_misc.c references
 * sndserver_filename regardless, so we provide the definition unconditionally.
 */

#include <stdlib.h>
#include <stdio.h>

#include "doomdef.h"
#include "i_sound.h"
#include "i_system.h"

/*
 * sndserver_filename: default path to the sound server binary.
 * m_misc.c stores this path in the default.cfg settings table regardless
 * of whether SNDSERV is defined.  We define it here so the build links.
 */
char* sndserver_filename = "sndserver";

/* -----------------------------------------------------------------------
 * Sound effects
 * ---------------------------------------------------------------------- */

void I_InitSound(void)
{
    /* No audio hardware available; silence is the correct behaviour. */
}

void I_ShutdownSound(void)
{
    /* Nothing to tear down. */
}

void I_SetChannels(void)
{
    /* No-op: channel mixing not implemented. */
}

int I_GetSfxLumpNum(sfxinfo_t* sfx)
{
    /* Return the W_GetNumForName result the caller would normally compute.
     * Returning -1 causes DOOM to mark the sfx as unavailable, which is
     * safer than crashing on a bad lump reference. */
    (void)sfx;
    return -1;
}

int I_StartSound(int id, int vol, int sep, int pitch, int priority)
{
    /* No hardware to start on; return a dummy handle that StopSound will
     * silently accept. */
    (void)id; (void)vol; (void)sep; (void)pitch; (void)priority;
    return -1;
}

void I_StopSound(int handle)
{
    (void)handle;
}

int I_SoundIsPlaying(int handle)
{
    (void)handle;
    return 0;   /* always "done" so DOOM can reuse the channel */
}

void I_UpdateSound(void)
{
    /* Mixing callback — no-op. */
}

void I_SubmitSound(void)
{
    /* DMA submit — no-op. */
}

void I_UpdateSoundParams(int handle, int vol, int sep, int pitch)
{
    (void)handle; (void)vol; (void)sep; (void)pitch;
}

/* -----------------------------------------------------------------------
 * Music
 * ---------------------------------------------------------------------- */

void I_InitMusic(void)       { }
void I_ShutdownMusic(void)   { }

void I_SetMusicVolume(int volume)
{
    (void)volume;
}

void I_PauseSong(int handle)
{
    (void)handle;
}

void I_ResumeSong(int handle)
{
    (void)handle;
}

int I_RegisterSong(void* data)
{
    (void)data;
    return -1;  /* invalid handle; caller must tolerate this */
}

void I_PlaySong(int handle, int looping)
{
    (void)handle; (void)looping;
}

void I_StopSong(int handle)
{
    (void)handle;
}

void I_UnRegisterSong(int handle)
{
    (void)handle;
}

int I_QrySongPlaying(int handle)
{
    (void)handle;
    return 0;
}
