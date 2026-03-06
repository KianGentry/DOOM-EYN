/*
 * EYN-OS video + input backend for DOOM (linuxdoom-1.10).
 *
 * Replaces the X11/SHM i_video.c implementation.
 *
 * Responsibilities:
 *   - I_InitGraphics / I_ShutdownGraphics : create/destroy a GUI window.
 *   - I_SetPalette                        : build a 256-entry RGB565 look-up table
 *                                           from the DOOM 768-byte palette.
 *   - I_FinishUpdate                      : palette-expand screens[0] to RGB565,
 *                                           blit to the EYN-OS GUI.
 *   - I_StartTic                          : drain GUI event queue and inject DOOM
 *                                           keyboard / mouse events.
 *   - I_StartFrame / I_UpdateNoBlit
 *     I_ReadScreen / I_WaitVBL             : stubs or trivial implementations.
 *
 * Screen allocation: screens[0..4] are allocated by V_Init() via I_AllocLow()
 * before I_InitGraphics is called.  We do not allocate them here.
 *
 * Key model: EYN-OS now delivers both GUI_EVENT_KEY (press/repeat) and
 * GUI_EVENT_KEY_UP (release) events.  Press events fire ev_keydown; release
 * events fire ev_keyup.  This gives DOOM accurate held-key state without
 * relying on keyboard autorepeat timing.
 *
 * Modifier handling:
 *   - GUI_KEY_CTRL (0x8000) is OR'd onto nav-key codes (0x1xxx) by the EYN-OS
 *     keyboard driver when Ctrl is held.  We detect this bit and synthesise a
 *     KEY_RCTRL (fire) event around the decorated key.
 *   - GUI_KEY_SHIFT (0x3000) is OR'd onto nav keys similarly; we synthesise
 *     KEY_RSHIFT (run).
 *   - Named Ctrl+letter combos (0x2xxx) also trigger KEY_RCTRL synthesis.
 *   - There is no standalone Ctrl/Shift event.  Players who want continuous
 *     fire must use the left mouse button.
 *
 * Mouse: relative deltas are derived from successive absolute positions.
 * Buttons: bit-0 = left (fire), bit-1 = right (use), bit-2 = middle.
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "doomdef.h"
#include "doomstat.h"
#include "i_system.h"
#include "i_video.h"
#include "v_video.h"
#include "m_argv.h"
#include "d_main.h"
#include "d_event.h"

#include <gui.h>
#include <unistd.h>

/* -----------------------------------------------------------------------
 * Static state
 * ---------------------------------------------------------------------- */

/* GUI window handle returned by gui_attach/gui_create. */
static int g_win = -1;

/*
 * ABI-INVARIANT: RGB565LE look-up table for the current DOOM palette.
 * 256 entries, one per 8-bit palette index.  Updated by I_SetPalette().
 */
static uint16_t g_pal565[256];

/*
 * Intermediate RGB565 blit buffer.
 * Size: 320 * 200 * 2 = 128 000 bytes — small enough to live on the stack
 * but we keep it static to avoid repeatedly dirtying the stack.
 */
static uint16_t g_blit[SCREENWIDTH * SCREENHEIGHT];

/*
 * Mouse centre-warp state.
 *
 * Each tic we warp the physical cursor back to (g_center_x, g_center_y)
 * (window-content-relative) so there is always room to move in every
 * direction.  The last known cursor position after the warp is stored in
 * g_last_mx / g_last_my so the next motion event delta is computed
 * correctly: delta = (new_pos − center).
 *
 * Initialised in I_InitGraphics from the content area size; defaults to
 * the DOOM framebuffer centre in case gui_get_content_size fails.
 */
static int g_center_x = SCREENWIDTH  / 2;
static int g_center_y = SCREENHEIGHT / 2;
static int g_last_mx  = -1;
static int g_last_my  = -1;

/* -----------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static inline uint16_t pack_rgb565(unsigned char r, unsigned char g,
                                   unsigned char b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/*
 * Map an EYN-OS GUI key code to a DOOM key code.
 * Returns 0 if the key should be ignored.
 *
 * Key code space (from gui.h):
 *   0x0001–0x001F  : ASCII control characters
 *   0x0020–0x007E  : printable ASCII
 *   0x1001–0x1009  : navigation keys (up/down/left/right/del/home/end/pgup/pgdn)
 *   0x2xxx         : named Ctrl+letter combos
 *   0x3001–0x3009  : Shift+nav keys  (0x3000 | nav)
 *   0x4001–0x4009  : Super+nav keys  (0x4000 | nav)
 *   0x5001–0x500C  : F1–F12
 *   0x9001–0x9009  : Ctrl+nav keys   (0x8000 | nav)
 */
static int xlate_key(int gk)
{
    /* Strip modifier bits to get the base key value. */
    int base = gk & ~(0x8000 | 0x4000 | 0x3000);

    switch (base) {
        /* Navigation */
        case 0x1001: return KEY_UPARROW;
        case 0x1002: return KEY_DOWNARROW;
        case 0x1003: return KEY_LEFTARROW;
        case 0x1004: return KEY_RIGHTARROW;
        case 0x1005: return 127;          /* Delete → backspace in DOOM */
        case 0x1006: return 0;            /* Home → unmapped */
        case 0x1007: return 0;            /* End  → unmapped */
        case 0x1008: return KEY_MINUS;    /* PgUp → previous weapon */
        case 0x1009: return KEY_EQUALS;   /* PgDn → next weapon */

        /* Function keys
         * DOOM F-key encoding: KEY_F1 = 0x80 + 0x3b, F2 = 0x3c, …
         * gui.h:                GUI_KEY_F1 = 0x5001, F2 = 0x5002, … */
        case 0x5001: return KEY_F1;
        case 0x5002: return KEY_F2;
        case 0x5003: return KEY_F3;
        case 0x5004: return KEY_F4;
        case 0x5005: return KEY_F5;
        case 0x5006: return KEY_F6;
        case 0x5007: return KEY_F7;
        case 0x5008: return KEY_F8;
        case 0x5009: return KEY_F9;
        case 0x500A: return KEY_F10;
        case 0x500B: return KEY_F11;
        case 0x500C: return KEY_F12;

        default: break;
    }

    /*
     * Named Ctrl+letter combos live in the 0x2xxx range.  We intentionally
     * fall through here (return 0) so the caller can decide to synthesise
     * KEY_RCTRL; no further key translation is needed for these.
     */
    if ((base & 0xFF00) == 0x2000 || (base & 0xFF00) == 0x2100 ||
        (base & 0xFF00) == 0x2200)
        return 0;

    /* Printable ASCII — normalise to lowercase (DOOM keybinds are lowercase). */
    if (gk >= 'A' && gk <= 'Z')
        return gk - 'A' + 'a';
    if (gk >= 0x20 && gk <= 0x7E)
        return gk;

    /* Common non-printable ASCII */
    if (gk == '\r' || gk == '\n') return KEY_ENTER;
    if (gk == 27)                 return KEY_ESCAPE;
    if (gk == '\t')               return KEY_TAB;
    if (gk == 8 || gk == 127)    return KEY_BACKSPACE;

    return 0;
}

/*
 * Post a single DOOM key event (press or release).
 */
static void post_doom_key(int doom_key, int is_down)
{
    if (doom_key <= 0) return;
    event_t ev;
    ev.type  = is_down ? ev_keydown : ev_keyup;
    ev.data1 = doom_key;
    ev.data2 = ev.data3 = 0;
    D_PostEvent(&ev);
}

void I_InitGraphics(void)
{
    /*
     * Attach to the current tile rather than spawning a new window.
     * This avoids focus-switching issues while ring-3 is active and the
     * mouse/keyboard pump is not running on the tiling side.
     */
    g_win = gui_attach("DOOM", "EYN-OS Port");
    if (g_win < 0)
        I_Error("I_InitGraphics: gui_attach failed (rc=%d)", g_win);

    gui_set_continuous_redraw(g_win, 1);

    /* Greyscale fallback palette until I_SetPalette is called. */
    for (int i = 0; i < 256; i++) {
        unsigned char v = (unsigned char)i;
        g_pal565[i] = pack_rgb565(v, v, v);
    }

    /*
     * Determine content area centre for cursor warping.
     * gui_get_content_size may return 0 if the tile isn't sized yet;
     * fall back to the DOOM framebuffer dimensions in that case.
     */
    {
        gui_size_t sz;
        if (gui_get_content_size(g_win, &sz) == 0 && sz.w > 0 && sz.h > 0) {
            g_center_x = sz.w / 2;
            g_center_y = sz.h / 2;
        } else {
            g_center_x = SCREENWIDTH  / 2;
            g_center_y = SCREENHEIGHT / 2;
        }
    }
    g_last_mx = g_center_x;
    g_last_my = g_center_y;
    gui_warp_mouse(g_win, g_center_x, g_center_y);

    /* Hide the system cursor sprite — the game doesn't need it. */
    gui_set_cursor_visible(g_win, 0);
}

void I_ShutdownGraphics(void)
{
    /* Restore cursor visibility before exiting. */
    if (g_win >= 0)
        gui_set_cursor_visible(g_win, 1);
}

/*
 * I_SetPalette — convert a 768-byte DOOM palette (256 × RGB) into the
 * RGB565 look-up table used by I_FinishUpdate.
 *
 * DOOM calls this on palette changes (e.g., damage flash, pickup flash).
 * The original implementation also applies gamma via gammatable[usegamma].
 * We omit gamma for now because EYN-OS displays are typically calibrated
 * without the Gamma requirement.
 */
void I_SetPalette(byte* palette)
{
    for (int i = 0; i < 256; i++) {
        unsigned char r = *palette++;
        unsigned char g = *palette++;
        unsigned char b = *palette++;
        g_pal565[i] = pack_rgb565(r, g, b);
    }
}

/*
 * I_FinishUpdate — compositor step called once per game tic.
 *
 * Converts screens[0] (8-bit paletted, 320×200) to RGB565 using the
 * current palette LUT, then blits to the GUI.
 */
void I_FinishUpdate(void)
{
    if (g_win < 0)
        return;

    /* Expand 8-bit indexed → RGB565. */
    const byte*     src = screens[0];
    uint16_t*       dst = g_blit;
    const int       n   = SCREENWIDTH * SCREENHEIGHT;
    for (int i = 0; i < n; i++)
        dst[i] = g_pal565[src[i]];

    /* Send to EYN-OS compositor. */
    gui_blit_rgb565_t cmd;
    cmd.src_w  = SCREENWIDTH;
    cmd.src_h  = SCREENHEIGHT;
    cmd.pixels = g_blit;
    cmd.dst_w  = 0;  /* honour content area width */
    cmd.dst_h  = 0;
    gui_blit_rgb565(g_win, &cmd);
}

void I_UpdateNoBlit(void) { /* no-op: blit happens in I_FinishUpdate */ }

void I_StartFrame(void) { /* no-op */ }

void I_ReadScreen(byte* scr)
{
    memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT);
}

void I_WaitVBL(int count)
{
    /* 70 Hz vertical blanking period = ~14 285 µs each. */
    usleep((unsigned int)(count * (1000000 / 70)));
}

void I_BeginRead(void) { }
void I_EndRead(void)   { }

/*
 * I_StartTic — called every game tic to drain the input queue.
 *
 * Translates EYN-OS GUI events into DOOM event_t records and posts them
 * via D_PostEvent().  See module header for key-event model details.
 */
void I_StartTic(void)
{
    if (g_win < 0)
        return;

    gui_event_t ge;
    while (gui_poll_event(g_win, &ge) == 1) {

        if (ge.type == GUI_EVENT_NONE)
            break;

        if (ge.type == GUI_EVENT_KEY || ge.type == GUI_EVENT_KEY_UP) {
            int gk      = ge.a;
            int is_down = (ge.type == GUI_EVENT_KEY);

            /* Detect Ctrl modifier patterns. */
            int has_ctrl       = (gk & 0x8000) != 0;      /* Ctrl+nav  */
            int has_ctrl_combo = ((gk & 0x7F00) >= 0x2000 &&
                                  (gk & 0x7F00) <= 0x2200); /* Ctrl+letter */
            int is_ctrl_c      = (gk == 0x2206);

            int dk = xlate_key(gk);

            if (has_ctrl || has_ctrl_combo || is_ctrl_c) {
                /*
                 * Ctrl is held: treat KEY_RCTRL as pressed/released (fire),
                 * and also post the inner directional key if any.
                 */
                post_doom_key(KEY_RCTRL, is_down);
                if (dk) post_doom_key(dk, is_down);
            } else if (dk) {
                post_doom_key(dk, is_down);
            }
        }
        else if (ge.type == GUI_EVENT_MOUSE) {
            int mx  = ge.a;   /* window-content-relative x after last warp */
            int my  = ge.b;
            int btn = ge.c;

            event_t ev;
            ev.type  = ev_mouse;
            /* DOOM button bits: 0=fire(left), 1=use(right), 2=forward(middle) */
            ev.data1 = (btn & 1) | ((btn & 2) ? 2 : 0) | ((btn & 4) ? 4 : 0);

            /*
             * Compute delta from the last known warp target (centre).
             * Because we warp back to centre after every event, the delta
             * here equals the actual physical mouse movement since the
             * previous tic, unbounded by screen edges.
             */
            ev.data2 = (mx - g_last_mx) << 2;   /* horiz ×4 sensitivity */
            ev.data3 = (g_last_my - my)  << 2;   /* vert  ×4, Y inverted */

            /*
             * Re-centre: warp the cursor back to the content-area centre
             * and record that position as our new baseline.  The next
             * hardware delta will be relative to this known origin.
             */
            g_last_mx = g_center_x;
            g_last_my = g_center_y;
            gui_warp_mouse(g_win, g_center_x, g_center_y);

            D_PostEvent(&ev);
        }
        else if (ge.type == GUI_EVENT_CLOSE) {
            /* Window close → treat as Escape press. */
            post_doom_key(KEY_ESCAPE, 1);
        }
    }
}
