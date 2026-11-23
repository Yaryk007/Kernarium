/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Macros the walk probes share (link_walk_probe.inc, return_walk_probe.inc).
 *
 * walk_order_fill fills `count` halfwords at `table` with block indices below
 * `blocks`, both powers of two: a xorshift sequence when `random` is 1, the
 * indices in order otherwise. It runs once, before the timed loop, and uses
 * x10 to x12.
 *
 * walk_family emits the first instruction of a block whose index is `index`:
 * one of eight families, each a gadget entry of its own and none of them a
 * memory access or an ADD-immediate. walk_family_inputs gives their source
 * registers values first.
 */

.macro walk_order_fill table, count, blocks, random
	movz	x10, #0x9e37
	movk	x10, #0x79b9, lsl #16
	movk	x10, #0x7f4a, lsl #32
	movk	x10, #0x7c15, lsl #48
	mov	x11, xzr
1:
.if \random
	eor	x10, x10, x10, lsl #13
	eor	x10, x10, x10, lsr #7
	eor	x10, x10, x10, lsl #17
	ubfx	x12, x10, #32, #16
.else
	mov	x12, x11
.endif
	and	x12, x12, #(\blocks - 1)
	strh	w12, [\table, x11, lsl #1]
	add	x11, x11, #1
	cmp	x11, #\count
	b.ne	1b
.endm

.macro walk_family_inputs
	mov	x2, #2
	mov	x3, #3
	mov	x4, #4
	mov	x5, #5
	mov	x6, #6
	mov	x7, #7
	mov	x11, #11
	mov	x12, #12
.endm

.macro walk_family index
.if (\index % 8) == 0
	movz	x0, #0x1234
.elseif (\index % 8) == 1
	eor	x2, x2, x3, lsl #1
.elseif (\index % 8) == 2
	rev	x3, x3
.elseif (\index % 8) == 3
	clz	x4, x4
.elseif (\index % 8) == 4
	ror	x5, x5, #7
.elseif (\index % 8) == 5
	adc	x6, x6, xzr
.elseif (\index % 8) == 6
	mul	x7, x7, x7
.else
	csel	x10, x11, x12, ne
.endif
.endm
