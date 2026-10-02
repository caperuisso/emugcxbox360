#!/usr/bin/env python3
# Wraps a flat binary into a single-section DOL. SPDX-License-Identifier: GPL-2.0-or-later
import struct, sys
src, out, addr = sys.argv[1], sys.argv[2], int(sys.argv[3], 0)
text = open(src, "rb").read()
text += b"\0" * (-len(text) % 32)
h = bytearray(0x100)
struct.pack_into(">I", h, 0x00, 0x100)
struct.pack_into(">I", h, 0x48, addr)
struct.pack_into(">I", h, 0x90, len(text))
struct.pack_into(">I", h, 0xE0, addr)
open(out, "wb").write(bytes(h) + text)
print(f"wrote {out}: {len(text)} bytes at {addr:#x}")
