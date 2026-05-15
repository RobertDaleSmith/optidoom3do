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

#endif
