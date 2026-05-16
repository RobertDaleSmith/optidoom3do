#!/usr/bin/env python3
"""
patch_broker.py -- swap system/Tasks/eventbroker inside a 02c-derived
ISO with a keyboard-aware broker.

The stock OptiDoom 02c broker (16160 bytes) has NO driverlet support
-- it doesn't scan $DRIVERS/CPORT*.ROM, so the CPORT4B.ROM keyboard
driverlet we injected is never loaded. Replacing the broker with the
trapexit/portfolio_os build (with KeyboardDriver) wires the chain back
up: broker -> driverlet -> EventFrame -> our keyboard.c subscription.

Disc layout note: eventbroker's slot is 8 sectors (16384 bytes). Our
keyboard-aware broker is ~21948 bytes (11 sectors). It overflows into
the next file's sectors -- which is `shell`, the Portfolio dev CLI.
Shell is never invoked by an end-user game launch, so corrupting its
bytes is harmless. We patch the directory entry sizes so the OS sees
eventbroker as ~21948 bytes and any reference to shell still points at
its (now bogus) old sector range.

Usage:
    patch_broker.py base.iso new_broker out.iso
"""

import struct
import sys

SECTOR = 2048


def patch(base_iso_path: str, broker_path: str, out_path: str) -> None:
    with open(base_iso_path, 'rb') as f:
        iso = bytearray(f.read())
    with open(broker_path, 'rb') as f:
        new_broker = f.read()

    new_size = len(new_broker)
    new_block_count = (new_size + SECTOR - 1) // SECTOR
    print(f'new broker: {new_size} bytes = {new_block_count} sectors')

    # Locate eventbroker directory entries. OperaFS triple-copies each
    # entry to redundant avatar lists, so search the whole iso.
    needle = b'eventbroker\x00'
    entries = []
    offset = 0
    while True:
        idx = iso.find(needle, offset)
        if idx == -1:
            break
        # Filter to dir entries: size + block_count fields preceding
        # the filename must look sane (block_size = 2048).
        if idx >= 0x14:
            blkz = struct.unpack_from('>I', iso, idx - 0x14)[0]
            if blkz == 2048:
                entries.append(idx)
        offset = idx + 1
    print(f'found {len(entries)} eventbroker directory entries')

    if not entries:
        raise SystemExit('no eventbroker dir entries -- abort')

    # First entry tells us the original size + block_count + start sector.
    fn = entries[0]
    orig_size = struct.unpack_from('>I', iso, fn - 0x10)[0]
    orig_blocks = struct.unpack_from('>I', iso, fn - 0x0C)[0]
    print(f'original: {orig_size} bytes = {orig_blocks} sectors')

    # Disc-wide start-sector for eventbroker: derive from the BLOCKS_TABLE
    # / extent in the dir entry. 3dt told us byte offset 0x09be4000 in a
    # 02c.iso -- that's sector 0x4DF2 = 19954. Sanity-scan for the AIF
    # header byte pattern at that location to confirm.
    AIF_PREAMBLE = b'\xe1\xa0\x00\x00'  # MOV r0,r0 first instr (NOP)
    # Search for the eventbroker's AIF entry in the disc by content
    # match -- compare first 256 bytes of stock vs the disc starting
    # near the documented sector. The original stock broker bytes are
    # what's currently in the slot.
    # Simplest: find the FIRST instance of the AIF header pattern that
    # has the stock broker's known size of 16160 bytes following it
    # somewhere on disc.
    # Most reliable: use the disc-extent table in OperaFS dir entries.
    # The avatar block_count + an offset table tells us the start.
    # The disc-byte offset for eventbroker (per 3dt list on this 02c
    # build): 0x09be4000 = sector 19954. Hard-code rather than derive
    # from dir-entry fields since the OperaFS extent layout varies.
    start_sector = 0x09be4000 // SECTOR
    print(f'eventbroker start sector: {start_sector}')

    binary_byte_offset = start_sector * SECTOR
    # Show the current first bytes for sanity (the broker is a
    # compressed/self-relocating AIF whose preamble starts with a
    # BL instruction, not the MOV r0,r0 NOP).
    print(f'  bytes at sector {start_sector}: '
          f'{iso[binary_byte_offset:binary_byte_offset+16].hex()}')

    # Update each entry's size + block_count to the new (larger) values.
    for fn in entries:
        struct.pack_into('>I', iso, fn - 0x10, new_size)
        struct.pack_into('>I', iso, fn - 0x0C, new_block_count)

    # Write the new broker bytes, zero-padded to a sector boundary.
    padded = new_broker + b'\x00' * (new_block_count * SECTOR - new_size)
    end_byte = binary_byte_offset + len(padded)
    iso[binary_byte_offset:end_byte] = padded

    with open(out_path, 'wb') as f:
        f.write(iso)
    print(f'wrote {out_path}')


if __name__ == '__main__':
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(1)
    patch(sys.argv[1], sys.argv[2], sys.argv[3])
