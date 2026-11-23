/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The dispatch arity probes' loop: what a gadget's dispatch costs when its
 * site has one successor against two or three, with nothing else changed.
 * Eight instruction words from eight gadget families each write a register
 * nothing reads, from registers the loop never writes, so no word forwards to
 * its neighbour and no chain runs through the loop. Each word appears three
 * times an iteration in every arrangement:
 *
 *   ARRANGEMENT 1   0 1 2 3 4 5 6 7 three times: every word always has the
 *                   same successor (word 7 has two).
 *   ARRANGEMENT 2   0 1 2 3 4 5 6 7, 0 2 4 6 1 3 5 7, 0 1 2 3 4 5 6 7.
 *   ARRANGEMENT 3   0 1 2 3 4 5 6 7, 0 2 4 6 1 3 5 7, 0 3 6 1 4 7 2 5: nearly
 *                   every word has three successors, in a fixed order.
 *
 * All three run the same words the same number of times, so their difference
 * is the cost of a dispatch site's successor count alone. The including file
 * sets ARRANGEMENT after probe_harness.inc.
 */

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 50000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

.macro word index
.if \index == 0
	add	x1, x20, #1
.elseif \index == 1
	eor	x2, x21, x22, lsl #1
.elseif \index == 2
	rev	x3, x23
.elseif \index == 3
	clz	x4, x24
.elseif \index == 4
	ror	x5, x25, #7
.elseif \index == 5
	movz	x6, #0x1234
.elseif \index == 6
	sub	x7, x26, x27
.else
	ubfx	x9, x20, #4, #8
.endif
.endm

.macro run a, b, c, d, e, f, g, h
	word	\a
	word	\b
	word	\c
	word	\d
	word	\e
	word	\f
	word	\g
	word	\h
.endm

	probe_start
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	mov	x18, #1
	mov	x20, #0x230
	mov	x21, #3
	mov	x22, #5
	mov	x23, #7
	mov	x24, #9
	mov	x25, #11
	mov	x26, #13
	mov	x27, #2
	mov	x10, xzr

.Lloop:
	run	0, 1, 2, 3, 4, 5, 6, 7
.if ARRANGEMENT == 1
	run	0, 1, 2, 3, 4, 5, 6, 7
	run	0, 1, 2, 3, 4, 5, 6, 7
.elseif ARRANGEMENT == 2
	run	0, 2, 4, 6, 1, 3, 5, 7
	run	0, 1, 2, 3, 4, 5, 6, 7
.else
	run	0, 2, 4, 6, 1, 3, 5, 7
	run	0, 3, 6, 1, 4, 7, 2, 5
.endif
	subs	x19, x19, x18
	b.ne	.Lloop

	/* Every word ran: its result is the one its inputs give. */
	cmp	x1, #0x231
	b.ne	.Lfailure
	cmp	x7, #11
	b.ne	.Lfailure
	cmp	x9, #0x23
	b.ne	.Lfailure

	probe_finish

	probe_end
