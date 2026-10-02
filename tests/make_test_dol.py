#!/usr/bin/env python3
# emugcxbox360 - builds a tiny GameCube DOL used to test the emulator end to end.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The program configures the VI for 640x480 progressive scan-out from an XFB,
# enables SI polling on port 1 and redraws scrolling colour bars every frame:
#   - the main stick scrolls the bars faster/slower,
#   - holding A turns the whole screen white.
# No toolchain needed: instructions are encoded by the mini assembler below.
import struct
import sys

TEXT_ADDR = 0x80003100
DATA_ADDR = 0x80004000
XFB_PHYS = 0x00500000


class Asm:
    def __init__(self, base):
        self.base = base
        self.items = []  # ints or (kind, args) fixups
        self.labels = {}

    def here(self):
        return self.base + 4 * len(self.items)

    def label(self, name):
        self.labels[name] = self.here()

    def emit(self, v):
        self.items.append(v)

    # --- encoders ---
    def d(self, op, rt, ra, imm):
        self.emit((op << 26) | (rt << 21) | (ra << 16) | (imm & 0xFFFF))

    def x(self, rt, ra, rb, xo, rc=0):
        self.emit((31 << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc)

    def li(self, r, v): self.d(14, r, 0, v)
    def lis(self, r, v): self.d(15, r, 0, v)
    def addi(self, rt, ra, v): self.d(14, rt, ra, v)
    def addic_(self, rt, ra, v): self.d(13, rt, ra, v)
    def ori(self, ra, rs, v): self.d(24, rs, ra, v)
    def cmpwi(self, ra, v): self.d(11, 0, ra, v)
    def lwz(self, rt, off, ra): self.d(32, rt, ra, off)
    def stw(self, rs, off, ra): self.d(36, rs, ra, off)
    def sth(self, rs, off, ra): self.d(44, rs, ra, off)
    def lwzx(self, rt, ra, rb): self.x(rt, ra, rb, 23)
    def add(self, rt, ra, rb): self.x(rt, ra, rb, 266)
    def srawi(self, ra, rs, sh): self.x(rs, ra, sh, 824)

    def rlwinm(self, ra, rs, sh, mb, me):
        self.emit((21 << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1))

    def slwi(self, ra, rs, n): self.rlwinm(ra, rs, n, 0, 31 - n)

    def mtctr(self, rs): self.emit((31 << 26) | (rs << 21) | (9 << 16) | (467 << 1))

    def b(self, target): self.emit(("b", target))
    def bdnz(self, target): self.emit(("bc", 16, 0, target))
    def beq(self, target): self.emit(("bc", 12, 2, target))
    def bne(self, target): self.emit(("bc", 4, 2, target))

    def assemble(self):
        out = []
        for i, it in enumerate(self.items):
            pc = self.base + 4 * i
            if isinstance(it, int):
                out.append(it)
                continue
            kind = it[0]
            target = self.labels[it[-1]]
            off = target - pc
            if kind == "b":
                out.append((18 << 26) | (off & 0x03FFFFFC))
            else:
                bo, bi = it[1], it[2]
                out.append((16 << 26) | (bo << 21) | (bi << 16) | (off & 0xFFFC))
        return b"".join(struct.pack(">I", w) for w in out)


def yuyv(y, u, v):
    return (y << 24) | (u << 16) | (y << 8) | v


# 75% colour bars in YCbCr (BT.601)
BARS = [
    yuyv(180, 128, 128),  # white
    yuyv(162, 44, 142),   # yellow
    yuyv(131, 156, 44),   # cyan
    yuyv(112, 72, 58),    # green
    yuyv(84, 184, 198),   # magenta
    yuyv(65, 100, 212),   # red
    yuyv(35, 212, 114),   # blue
    yuyv(16, 128, 128),   # black
]
WHITE = yuyv(235, 128, 128)


def build_program():
    a = Asm(TEXT_ADDR)
    # r30 = VI regs, r31 = SI regs, r10 = colour table
    a.lis(30, 0xCC00); a.ori(30, 30, 0x2000)
    a.lis(31, 0xCC00); a.ori(31, 31, 0x6400)
    a.lis(10, DATA_ADDR >> 16); a.ori(10, 10, DATA_ADDR & 0xFFFF)

    # VI: 480 active lines, progressive (NIN + ENB), 640 px wide, XFB in page-offset mode
    a.li(3, (480 << 4) | 6); a.sth(3, 0x00, 30)
    a.li(3, 0x0005); a.sth(3, 0x02, 30)
    a.li(3, (40 << 8) | 40); a.sth(3, 0x48, 30)
    tfbl = 0x10000000 | (XFB_PHYS >> 5)
    a.lis(3, tfbl >> 16); a.ori(3, 3, tfbl & 0xFFFF); a.stw(3, 0x1C, 30)

    # SI: poll channel 0
    a.li(3, 0x80); a.stw(3, 0x30, 31)
    a.li(29, 0)  # scroll phase

    a.label("frame")
    a.lwz(28, 4, 31)               # SICINBUFH: buttons<<16 | stickX<<8 | stickY
    a.rlwinm(27, 28, 8, 31, 31)    # A button (bit 0x0100 of the button word)
    a.rlwinm(26, 28, 24, 24, 31)   # stick X
    a.addi(26, 26, -128)
    a.srawi(26, 26, 3)
    a.add(29, 29, 26)
    a.addi(29, 29, 2)              # constant scroll

    a.lis(3, 0xC000 | (XFB_PHYS >> 16))  # uncached XFB pointer
    a.li(5, 480)
    a.label("row")
    a.li(6, 320); a.mtctr(6)
    a.li(7, 0)
    a.label("pixel")
    a.slwi(8, 7, 1)
    a.add(8, 8, 29)
    a.rlwinm(8, 8, 26, 29, 31)     # ((x + phase) >> 6) & 7
    a.slwi(8, 8, 2)
    a.lwzx(9, 10, 8)
    a.cmpwi(27, 0)
    a.beq("store")
    a.lis(9, WHITE >> 16); a.ori(9, 9, WHITE & 0xFFFF)
    a.label("store")
    a.stw(9, 0, 3)
    a.addi(3, 3, 4)
    a.addi(7, 7, 1)
    a.bdnz("pixel")
    a.addic_(5, 5, -1)
    a.bne("row")
    a.b("frame")
    return a.assemble()


def build_dol(text, data):
    header = bytearray(0x100)
    text_off = 0x100
    data_off = text_off + len(text)
    struct.pack_into(">I", header, 0x00, text_off)
    struct.pack_into(">I", header, 0x48, TEXT_ADDR)
    struct.pack_into(">I", header, 0x90, len(text))
    struct.pack_into(">I", header, 0x1C, data_off)
    struct.pack_into(">I", header, 0x64, DATA_ADDR)
    struct.pack_into(">I", header, 0xAC, len(data))
    struct.pack_into(">I", header, 0xD8, 0x80200000)  # bss
    struct.pack_into(">I", header, 0xDC, 0)
    struct.pack_into(">I", header, 0xE0, TEXT_ADDR)   # entry
    return bytes(header) + text + data


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "test.dol"
    text = build_program()
    data = b"".join(struct.pack(">I", c) for c in BARS)
    with open(out, "wb") as f:
        f.write(build_dol(text, data))
    print(f"wrote {out}: {len(text) // 4} instructions")


if __name__ == "__main__":
    main()
