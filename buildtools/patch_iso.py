#!/usr/bin/env python3
"""
patch_iso.py -- swap launchme inside an existing bootable 3DO Opera-FS
ISO without re-composing the disc. The publicly-redistributable disc
compose tools (3doiso, OperaTool) don't preserve the exact layout the
3DO BIOS needs to boot, so we work around that by patching a
known-good ISO in place.

Usage:
    patch_iso.py base.iso new_launchme out.iso

The base ISO must already be bootable. We:
  1. Find the launchme directory-entry records (typically 3 redundant
     copies near the disc end in OperaFS's avatar list) and update
     the file_size + block_count fields to match the new launchme.
  2. Update the BLOCKS_ALWAYS romtag (offset+size pair in rom_tags)
     so the BIOS allocates the right amount of memory at boot.
  3. Write the new launchme bytes at the existing launchme sector
     (sector 1182 in OptiDoom builds), zero-padding to a sector
     boundary.
  4. After this you still need to run 3DOEncrypt on the result to
     refresh the disc signature -- the patch invalidates the cached
     checksum but 3DOEncrypt will recompute it.

Assumes the new launchme fits between launchme's original sector and
the next file in the disc (we measure the gap and bail if not). This
is enough headroom for builds within ~150 KB of OptiDoom 02c's size.
"""

import struct
import sys

SECTOR = 2048


def patch(base_iso_path: str, launchme_path: str, out_path: str) -> None:
    with open(base_iso_path, 'rb') as f:
        iso = bytearray(f.read())
    with open(launchme_path, 'rb') as f:
        new_launchme = f.read()

    new_size = len(new_launchme)
    new_block_count = (new_size + SECTOR - 1) // SECTOR
    print(f'New launchme: {new_size} bytes = {new_block_count} sectors')

    # ---- 1. Update directory entries ----
    # OperaFS directory entry layout (relative to filename offset):
    #   filename - 0x0C: file_size (uint32 big-endian)
    #   filename - 0x08: block_count (uint32 big-endian)
    # The filename is null-padded to 32 bytes. Search for "launchme\0".
    entries = []
    offset = 0
    needle = b'launchme\x00'
    while True:
        idx = iso.find(needle, offset)
        if idx == -1:
            break
        entries.append(idx)
        offset = idx + 1
    print(f'Found {len(entries)} launchme directory entries')

    orig_size = None
    for entry_filename_offset in entries:
        size_offset  = entry_filename_offset - 0x10  # 16 bytes before
        block_offset = entry_filename_offset - 0x0C  # 12 bytes before
        cur_size = struct.unpack_from('>I', iso, size_offset)[0]
        cur_blocks = struct.unpack_from('>I', iso, block_offset)[0]
        if orig_size is None:
            orig_size = cur_size
            orig_blocks = cur_blocks
            print(f'  original size: {cur_size} bytes ({cur_blocks} sectors)')
        struct.pack_into('>I', iso, size_offset, new_size)
        struct.pack_into('>I', iso, block_offset, new_block_count)

    if orig_size is None:
        raise SystemExit('error: no launchme directory entries found')

    # ---- 2. Find launchme byte offset on disc ----
    # The BLOCKS_ALWAYS romtag tells us the sector. The romtag table
    # itself lives in the boot area; the offset is the FIRST sector
    # occupied by launchme. Look it up via the rom_tags file. The
    # rom_tags structure starts with a header then RomTag entries
    # (each 32 bytes): subsystype, type, version, revision, flags,
    # typeSpecific, offset, size, reserved.
    #
    # For our purposes we just need to find the BLOCKS_ALWAYS entry
    # and update its size field. The OFFSET field stays the same
    # (sector 1182 in OptiDoom).
    launchme_offset = 1182 * SECTOR
    # BLOCKS_ALWAYS romtag layout (this build's variant):
    #   byte 0: subsystype = 0x0f
    #   byte 1: type       = 0x02
    #   byte 2-7: flags / reserved (zero in OptiDoom)
    #   byte 8-11: start_sector (typeSpecific) = 1182 = 0x49E
    #   byte 12-15: size_in_bytes = 340560 = 0x53250
    # Scan the boot area for entries that match this layout and rewrite
    # the size field.
    found_romtag = 0
    pos = 0
    while pos < launchme_offset:
        if iso[pos] == 0x0f and iso[pos + 1] == 0x02:
            offset_at = struct.unpack_from('>I', iso, pos + 8)[0]
            size_at   = struct.unpack_from('>I', iso, pos + 12)[0]
            if offset_at == 1182 and size_at == orig_size:
                print(f'  BLOCKS_ALWAYS romtag at byte 0x{pos:x}: '
                      f'offset={offset_at} size={size_at} -> {new_size}')
                struct.pack_into('>I', iso, pos + 12, new_size)
                found_romtag += 1
        pos += 4

    if found_romtag == 0:
        raise SystemExit('error: BLOCKS_ALWAYS romtag entry not found')
    print(f'  updated {found_romtag} BLOCKS_ALWAYS romtag entries')

    # ---- 3. Write the new launchme bytes ----
    # Crucial layout detail: the launchme binary does NOT start at
    # the BLOCKS_ALWAYS slot's first sector. The slot's first sector
    # (1182) is a 2048-byte zero pad, and the AIF binary begins at
    # sector 1183. The BIOS expects this layout; writing the AIF at
    # sector 1182 yields a black screen on boot.
    LAUNCHME_PAD = SECTOR  # one sector of leading zero pad
    binary_byte_offset = launchme_offset + LAUNCHME_PAD

    # Romtag/dir-entry sizes INCLUDE the pad (matches stock).
    # Re-pack with pad included.
    padded_size_with_pad = LAUNCHME_PAD + new_size
    padded_blocks_with_pad = (padded_size_with_pad + SECTOR - 1) // SECTOR
    print(f'  including {LAUNCHME_PAD}-byte leading pad: '
          f'{padded_size_with_pad} bytes ({padded_blocks_with_pad} sectors)')

    for entry_filename_offset in entries:
        struct.pack_into('>I', iso, entry_filename_offset - 0x10,
                         padded_size_with_pad)
        struct.pack_into('>I', iso, entry_filename_offset - 0x0C,
                         padded_blocks_with_pad)

    # Re-find + re-update BLOCKS_ALWAYS romtag with pad-inclusive size.
    pos = 0
    while pos < launchme_offset:
        if iso[pos] == 0x0f and iso[pos + 1] == 0x02:
            offset_at = struct.unpack_from('>I', iso, pos + 8)[0]
            size_at   = struct.unpack_from('>I', iso, pos + 12)[0]
            if offset_at == 1182 and size_at == new_size:
                struct.pack_into('>I', iso, pos + 12, padded_size_with_pad)
        pos += 4

    max_blocks_in_slot = 1500 - 1182
    if padded_blocks_with_pad > max_blocks_in_slot:
        raise SystemExit(f'error: new launchme needs {padded_blocks_with_pad} '
                         f'sectors (with pad) but slot is only {max_blocks_in_slot}')

    # Zero the leading pad sector explicitly, write binary at sector 1183.
    iso[launchme_offset:binary_byte_offset] = b'\x00' * LAUNCHME_PAD
    padded = new_launchme + b'\x00' * (padded_blocks_with_pad * SECTOR
                                       - padded_size_with_pad)
    end_byte = binary_byte_offset + len(padded)
    iso[binary_byte_offset:end_byte] = padded

    if padded_blocks_with_pad < orig_blocks:
        zero_end = launchme_offset + orig_blocks * SECTOR
        iso[end_byte:zero_end] = b'\x00' * (zero_end - end_byte)

    # ---- 4. Write output ----
    with open(out_path, 'wb') as f:
        f.write(iso)
    print(f'wrote {out_path}')


if __name__ == '__main__':
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(1)
    patch(sys.argv[1], sys.argv[2], sys.argv[3])
