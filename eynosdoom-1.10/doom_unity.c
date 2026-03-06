/*
 * doom_unity.c — Unity (single-TU) build of DOOM for compilation
 * with chibicc on EYN-OS.
 *
 * This file #includes all the DOOM source files in one translation
 * unit, allowing chibicc (which only handles one input file at a
 * time) to compile the entire game.
 *
 * The rcsid static variable is defined in several original sources;
 * we use __COUNTER__ to rename it to avoid duplicate-definition
 * errors in the merged TU.
 */

#define NORMALUNIX
#define LINUX
#undef SNDSERV

/* Rename static rcsid per file to avoid redefinition conflicts. */
#define rcsid DOOM_PASTE(rcsid_, __COUNTER__)
#define DOOM_PASTE(a,b)  DOOM_PASTE2(a,b)
#define DOOM_PASTE2(a,b) a##b

#include "am_map.c"
#include "d_items.c"
#include "d_main.c"
#include "d_net.c"
#include "doomdef.c"
#include "doomstat.c"
#include "dstrings.c"
#include "f_finale.c"
#include "f_wipe.c"
#include "g_game.c"
#include "hu_lib.c"
#include "hu_stuff.c"
#include "i_eynos_net.c"
#include "i_eynos_sound.c"
#include "i_eynos_system.c"
#include "i_eynos_video.c"
#include "info.c"
#include "m_argv.c"
#include "m_bbox.c"
#include "m_cheat.c"
#include "m_fixed.c"
#include "m_menu.c"
#include "m_misc.c"
#include "m_random.c"
#include "m_swap.c"
#include "p_ceilng.c"
#include "p_doors.c"
#include "p_enemy.c"
#include "p_floor.c"
#include "p_inter.c"
#include "p_lights.c"
#include "p_map.c"
#include "p_maputl.c"
#include "p_mobj.c"
#include "p_plats.c"
#include "p_pspr.c"
#include "p_saveg.c"
#include "p_setup.c"
#include "p_sight.c"
#include "p_spec.c"
#include "p_switch.c"
#include "p_telept.c"
#include "p_tick.c"
#include "p_user.c"
#include "r_bsp.c"
#include "r_data.c"
#include "r_draw.c"
#include "r_main.c"
#include "r_plane.c"
#include "r_segs.c"
#include "r_sky.c"
#include "r_things.c"
#include "s_sound.c"
#include "sounds.c"
#include "st_lib.c"
#include "st_stuff.c"
#include "tables.c"
#include "v_video.c"
#include "w_wad.c"
#include "wi_stuff.c"
#include "z_zone.c"
#include "i_main.c"

/*
 * When compiling with chibicc (single-file, no separate linking), inline the
 * EYN-OS userland libc sources so all standard-library symbols are available.
 * GCC builds link against libeync.a instead.
 */
#ifdef __chibicc__
#include "../../userland/libc/errno.c"
#include "../../userland/libc/string.c"
#include "../../userland/libc/ctype.c"
#include "../../userland/libc/stdlib.c"
#include "../../userland/libc/unistd.c"
#include "../../userland/libc/fcntl.c"
#include "../../userland/libc/stat.c"
#include "../../userland/libc/time.c"
#include "../../userland/libc/stdio.c"
#include "../../userland/libc/gui.c"
#include "../../userland/libc/math.c"
#endif /* __chibicc__ */
