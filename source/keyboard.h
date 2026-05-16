/*
 *	source/keyboard.h
 *
 *	PBUS keyboard support for OptiDoom. Opens a private subscription
 *	to the Portfolio Event Broker, watches for keyboard events, and
 *	OR-folds the held-key state into the joypad bit stream so the
 *	existing game-input pipeline picks it up without further changes.
 *
 *	Requires a broker that knows how to decode PS/2 Set 2 scancodes
 *	from a 0x02 / 0x4B PBUS device (joypad-ai's KeyboardDriver.c
 *	patch to trapexit/portfolio_os). Without that broker the calls
 *	below are harmless no-ops -- no keyboard events ever arrive.
 */

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "Burger.h"

/* Subscribe to keyboard events on the broker. Safe to call after
 * InitEventUtility -- the broker is multi-subscriber, our listener
 * runs in parallel with the SDK's pad/mouse one. Returns 0 on
 * success, negative on broker connect failure (any failure also
 * disables keyboard polling; readKeyboardBits will return 0). */
void initKeyboard(void);

/* Drain any pending keyboard events from the broker, then return
 * the held-key state as a bitmask of Pad* values matching the
 * upstream joypad bits. Call once per frame, before reading the
 * combined input state, then OR the result into the joypad value. */
Word readKeyboardBits(void);

/* One-shot weapon-select request from number keys 1-7. Returns the
 * requested weapon index (wp_fist=0 .. wp_bfg=6) and CLEARS the
 * request, or -1 if no number key was newly pressed. Game logic
 * polls this each frame and sets player->pendingweapon when valid.
 * Matches the DOS Doom 1-7 weapon-select behaviour. */
int  readRequestedWeapon(void);

/* DOS Doom cheat codes: type letters during play and the game watches
 * for known sequences. Returns the cheat ID and CLEARS the buffer if
 * a code was just completed, or -1. Game polls each frame and
 * dispatches to the matching OptiDoom action. */
enum {
  KBD_CHEAT_NONE     = -1,
  KBD_CHEAT_IDDQD    = 0,  /* god mode */
  KBD_CHEAT_IDKFA    = 1,  /* all weapons + keys + ammo */
  KBD_CHEAT_IDFA     = 2,  /* all weapons + ammo, no keys */
  KBD_CHEAT_IDCLIP   = 3,  /* noclip */
  KBD_CHEAT_IDDT     = 4,  /* automap reveal (toggle 3-state) */
  KBD_CHEAT_IDMYPOS  = 5,  /* show X / Y / angle coordinates */
  KBD_CHEAT_BEHOLD_V = 6,  /* invulnerability */
  KBD_CHEAT_BEHOLD_S = 7,  /* berserk strength */
  KBD_CHEAT_BEHOLD_I = 8,  /* invisibility */
  KBD_CHEAT_BEHOLD_R = 9,  /* radiation suit */
  KBD_CHEAT_BEHOLD_A = 10, /* automap item */
  KBD_CHEAT_BEHOLD_L = 11  /* light amplification */
};
int  readRequestedCheat(void);

/* IDCLEV xx warp: returns 0-99 once when "idclev[2-digit]" was typed,
 * or -1. Game polls each frame to trigger level warp. */
int  readRequestedWarp(void);

/* +/- keys: returns +1 / -1 / 0 to grow/shrink the HUD viewport. */
int  readRequestedScreenSize(void);

/* Caps Lock auto-run toggle. Returns 1 if persistent run is on. */
int  isAutoRunEnabled(void);

/* One-shot: returns 1 if Caps Lock was just pressed this frame. Used
 * by the game to show an "Always Run ON/OFF" status message. */
int  readAutoRunToggled(void);

#endif
