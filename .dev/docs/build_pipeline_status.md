# OptiDoom 3DO Build Pipeline — Status

Status as of 2026-05-15.

## Works ✅

- **Cross-compile via `build_docker.sh`** — produces a `source/LaunchMe.`
  binary against trapexit/3do-devkit's Norcroft ARM toolchain + the
  full trapexit/portfolio_os source release for SDK headers.
- **Vendored `lib/3dlib/`** — with two surgical patches to match
  Toolkit 1.5's 3dlib.h against the modern SDK. Plus the user-supplied
  `userdefs3D.h` config (MAX3DOBJECTS=200).
- **Keyboard support code** — `source/keyboard.c` + `source/keyboard.h`,
  EB-broker subscription, scancode -> Pad* OR-fold, 1993 PC Doom
  default bindings (arrows + Alt-strafe + Ctrl-fire etc.). Compiles
  clean into the LaunchMe binary.
- **3DOEncrypt signing** — re-signs existing OptiDoom 02c.iso and the
  re-signed disc still boots in the Opera libretro emulator. So
  3DOEncrypt isn't the problem.

## Doesn't work ❌

- **ISO compose round-trip.** `3dt unpack` + `3doiso` does NOT
  reproduce a bootable disc, even when re-composing the known-working
  OptiDoom 02c.iso WITHOUT any modifications.
- **OperaTool compose.** Decompile works fine (with Wine Mono
  installed), but `-c` compile produces corrupted output:
  - `file_count: 5` instead of ~297
  - `BLOCKS_ALWAYS size 0xFFFFFFFF` (garbage)
  - `SIGNATURE_BLOCK Offset 0xCDCDCDCC` (wine memory-poison)
  - OperaTool reports itself as "0.01a" -- pre-alpha, compose mode
    appears unfinished.

## Root cause (best guess)

3DO disc compose tools available in the homebrew toolchain (3doiso,
OperaTool, modern 3dt) **don't preserve the exact disc-layout** that
the 3DO BIOS expects for boot. Specifically:

- The 3DO BIOS expects SIGNATURE_BLOCK to land AFTER BLOCKS_ALWAYS
  (which on OptiDoom is sectors 1182-1348). Working OptiDoom 02c has
  it at sector 4352. Our `3doiso` always places it at sector 105 --
  BEFORE BLOCKS_ALWAYS -- which the BIOS rejects ("no disc in drive"
  screen after boot logo).
- OperaTool's compile mode is too broken to produce a valid disc at
  all.

Optimus6128 builds OptiDoom on Windows with the **original ARM251 +
3DODev installation** circa 1995-96. Presumably his copy of 3doiso (or
some other tool we don't have) places the structural blocks correctly.
None of the publicly-redistributable versions reproduce this.

## What this means for the keyboard PR

The keyboard support is implemented entirely in C, lives in source/,
and **does compile correctly into a LaunchMe binary** via our pipeline.
We just can't package that binary into a bootable ISO ourselves.

Two paths forward:

1. **PR to Optimus6128** — open a PR with the keyboard code; he can
   build + verify in his Windows env, then ship a release.
2. **Find/recover Optimus's exact compose tooling** — long-tail
   research effort. Possibly involves disassembling the 3doiso his
   build uses, or finding original 3DO Company SDK build tools that
   produce BIOS-compatible discs.

## Files staged / committed for future work

- `build_docker.sh` — docker-based compile pipeline
- `lib/3dlib/` — vendored Toolkit 1.5 3DLib + patches
- `source/keyboard.{c,h}` — keyboard module
- `source/input.c`, `source/threedo.c` — keyboard hooks
- `ISOdecompile/CDextra/system/Tasks/eventbroker` — rebuilt broker with
  KeyboardDriver static-linked in (matches the joypad-tester build)
- `ISOdecompile/CDextra/system/Kernel/boot_code` — note the
  capitalization (Kernel, not kernel) -- OperaFS is case-sensitive,
  Optimus's CDextra had it lowercase which causes a path duplication
  on case-sensitive build hosts.
