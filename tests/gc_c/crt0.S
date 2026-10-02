# Minimal GameCube startup: set a stack and call main. GPL-2.0-or-later
    .section .text.start, "ax"
    .globl _start
_start:
    lis     1, 0x816F
    ori     1, 1, 0xFFF0
    li      0, 0
    stwu    0, -16(1)
    bl      main
1:  b       1b
