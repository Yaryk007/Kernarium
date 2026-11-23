/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Macros the x86-64 walk probes share (x86_64_memory_walk_probe.inc,
 * x86_64_link_walk_probe.inc and x86_64_return_walk_probe.inc), the
 * counterpart of walk_order.inc. Include it after x86_64_probe_harness.inc.
 *
 * walk_order_table emits `count` halfwords of block indices below `blocks`,
 * both powers of two: a xorshift sequence when `random` is 1, the indices in
 * order otherwise. The assembler computes them, so no code runs to fill the
 * table and none is translated in front of the timed loop.
 *
 * walk_family emits one register-only instruction of eight families, chosen
 * by `index` modulo eight, each a gadget entry of its own. It reads r11 and
 * writes r10.
 */

.macro walk_order_table count, blocks, random
	.set walk_order_state, 0x7f4a7c159e3779b9
	.set walk_order_index, 0
	.rept \count
.if \random
	/* xorshift64 (13, 7, 17), the shifts right logical. */
	.set walk_order_state, walk_order_state ^ (walk_order_state << 13)
	.set walk_order_state, walk_order_state ^ ((walk_order_state >> 7) & 0x01ffffffffffffff)
	.set walk_order_state, walk_order_state ^ (walk_order_state << 17)
	.short ((walk_order_state >> 32) & 0xffffffff) & (\blocks - 1)
.else
	.short walk_order_index & (\blocks - 1)
.endif
	.set walk_order_index, walk_order_index + 1
	.endr
.endm

.macro walk_family index
.if ((\index) & 7) == 0
	add	r10, r11
.elseif ((\index) & 7) == 1
	sub	r10, r11
.elseif ((\index) & 7) == 2
	xor	r10, r11
.elseif ((\index) & 7) == 3
	or	r10, r11
.elseif ((\index) & 7) == 4
	and	r10, r11
.elseif ((\index) & 7) == 5
	mov	r10, r11
.elseif ((\index) & 7) == 6
	lea	r10, [r10 + r11 * 2]
.else
	neg	r10
.endif
.endm
