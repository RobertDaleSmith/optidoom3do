# OptiDoom3DO — Keyboard Controls

Add WASD + mouse-look + standard FPS keyboard bindings to
Optimus6128's OptiDoom 3DO build, so it can be played with the
3DO Company never-shipped keyboard pod (or a modern USB-to-PBUS
adapter running joypad-ai/joypad-os USB23DO firmware).

## Why this is feasible

- The host-side `KeyboardDriver.c` is complete (`trapexit/portfolio_os`
  PR from RobertDaleSmith). When statically linked into the event
  broker, a PBUS device with class byte `0x02` or `0x4B` emits
  `EVENTNUM_KeyboardKeyPressed` / `KeyboardKeyReleased` /
  `KeyboardUpdate` / `KeyboardDataArrived`, each carrying a
  256-bit key matrix.
- OptiDoom's `source/input.c` already uses `GetControlPad` +
  `GetMouse` cleanly; adding a third `updateKeyboard()` next to
  those is the natural shape.
- `threedo.c:448` calls `InitEventUtility(1, 1, FALSE)`. The SDK
  utility doesn't expose a `GetKeyboard` analogue (no retail
  keyboard ever existed), so we drop down to the broker message
  port and read keyboard events directly. The matrix is small
  (32 bytes); a tiny EB listener parallel to the existing one is
  enough.

## Bind set (WASD + mouse-look)

Following standard PC Doom conventions, mapped onto OptiDoom's
existing action enum in `input.h`:

| Action          | Keyboard scan-code | Existing pad equivalent          |
|-----------------|--------------------|----------------------------------|
| Forward         | W                  | D-pad Up                         |
| Back            | S                  | D-pad Down                       |
| Strafe Left     | A                  | LShift + D-pad Left              |
| Strafe Right    | D                  | LShift + D-pad Right             |
| Turn Left       | Left arrow         | D-pad Left                       |
| Turn Right      | Right arrow        | D-pad Right                      |
| Fire            | Left Ctrl, Space   | A                                |
| Use / Open      | E, Enter           | C                                |
| Run (toggle)    | Left Shift         | LShift                           |
| Weapon next     | ] / mouse wheel    | RShift                           |
| Weapon prev     | [                  | (none, currently pad-only cycle) |
| Map toggle      | Tab                | Start menu -> Map                |
| Menu            | Escape             | Start                            |
| Console (mods)  | Backtick           | (none)                           |

Scan-code = PS/2 Set 2 byte from `KeyboardDriver` matrix bit
position. Numbers in the table column will be filled in by the
implementation pass against `PS2_TO_ACTION[256]`.

## Implementation phases

### Phase 1: keyboard plumbing (no game logic changes)

1. Add `source/keyboard.c` + `source/keyboard.h`. Set up an
   `EB_Configure` listener for keyboard events (parallel to the
   one the SDK's InitEventUtility creates internally for pad +
   mouse). Provide `initKeyboard()` and `pollKeyboard()`.
2. Add `keyboardActionPressed[]` / `keyboardActionPressedOnce[]`
   arrays in `input.c`, populate from the polled matrix.
3. Don't touch `threedo.c`'s InitEventUtility call; the broker
   subscription is separate and additive.

### Phase 2: action lookup table

`PS2_TO_ACTION[256]` maps each Set 2 scancode (regular + the
`(byte | 0x80)` E0-extended subset for arrow keys) to an OptiDoom
action enum from `input.h`. Unmapped scancodes return ACTION_NONE.

### Phase 3: wire actions into existing game-input call sites

OptiDoom currently reads `isJoyButtonPressed(JOY_BUTTON_UP)` etc.
in the game loop. Either:

  (a) Add `isActionPressed(ACTION_FORWARD)` helpers and replace each
      JOY_BUTTON_* call site; or
  (b) Have `updateKeyboard` OR-fold keyboard state directly into the
      joybits stream (`joybits |= ControlUp` when W is held) so no
      game-logic changes are needed.

(b) is much smaller and likely sufficient for a v0.1.

### Phase 4: mouse-look refinement

OptiDoom's mouse delta currently feeds turn velocity. With
keyboard turn-keys also active, decide which wins each frame
(probably max of the two, or sum them, or have keyboard arrows
override mouse if held). Tune until it feels right.

## Build infrastructure

Upstream Optidoom builds with Windows .bat files + the old ARM
SDT compiler. The Norcroft ARM toolchain in our existing
[joypad-tester 3DO build image](https://github.com/joypad-ai/joypad-tester/blob/main/3do/buildtools/Dockerfile)
is the same lineage and should compile OptiDoom's sources too.

Plan: write `build_docker.sh` that wraps the upstream `source/makefile`
under our 3DO toolchain image, plus does the OperaTool / 3doiso /
3doEncrypt steps the README describes. User supplies their own
commercial `doom.iso` (legal grey, can't ship).

## Testing path

1. Build the modified `.iso` via docker wrapper.
2. Run under Opera libretro core. (Caveat: Opera doesn't enumerate
   a keyboard pod, so emulator testing only verifies "doesn't crash
   when keyboard subscription returns nothing.")
3. Real hardware: USB23DO firmware with `TDO_MODE_KEYBOARD` (joypad-os
   project, pending) emits PS/2 Set 2 over PBUS. With the modified
   broker (joypad-tester's rebuilt one) loading our `KeyboardDriver`,
   OptiDoom sees the keyboard pod, polls it, and responds.

## PR target

`RobertDaleSmith/optidoom3do feature/keyboard-controls -> Optimus6128:master`
once verified on real hardware. Optimus has been open to PRs based on
the v0.3 "Massive commit" log style.
