/* emugcxbox360 - Linux/PPC32 process entry for the qemu-ppc test build */
	.section .text
	.globl _start
_start:
	mr 9, 1                 /* argc at 0(r1), argv at 4(r1) */
	clrrwi 1, 1, 4
	li 0, 0
	stwu 0, -32(1)
	lis 13, _SDA_BASE_@ha
	addi 13, 13, _SDA_BASE_@l
	lis 2, _SDA2_BASE_@ha
	addi 2, 2, _SDA2_BASE_@l
	lwz 3, 0(9)
	addi 4, 9, 4
	bl shim_start
1:	b 1b
	.weak _SDA_BASE_
	.weak _SDA2_BASE_
