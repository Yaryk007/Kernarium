/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Body shared by the move-row probes: a loop of four groups, each a row of
 * register moves between the instruction that produces the row's first
 * source and the one that consumes its last destination. The defines:
 *
 *   MOVE_ROW    the moves in the row, 2 to 6;
 *   MOVE_W32    a mask of the moves that are 32-bit (MOV Wd, Wm), bit 0 the
 *               first; their sources hold values above 32 bits, so a move
 *               that keeps the upper half fails the probe;
 *   MOVE_CHAIN  1 for a chain of 64-bit moves, each reading the previous
 *               one's destination, instead of independent moves;
 *   MOVE_SPLIT  1 to put one of the group's other instructions between
 *               every two moves, so no two moves are adjacent.
 *
 * A group is ADD X9, the row, ADD X16 of the row's last destination, and
 * MOVE_ROW - 1 ADD-immediates of registers no move touches. Move n writes
 * X10 + n; the first reads X9, the others X20 + n - 1, or in a chain the
 * previous move's destination. The ordinary words forward a register into
 * the row and out of it in both arrangements, and a chain forwards through
 * every move as well unless split. Both arrangements execute the same
 * instructions.
 *
 * The including file defines the four macros first.
 */

#if MOVE_ROW < 2 || MOVE_ROW > 6
#error "MOVE_ROW is 2 to 6"
#endif
#if MOVE_CHAIN && MOVE_W32
#error "a chain is of 64-bit moves"
#endif

#ifndef BENCH_ITERATIONS
#if MOVE_ROW == 2
#define BENCH_ITERATIONS 40000000
#elif MOVE_ROW == 3
#define BENCH_ITERATIONS 30000000
#elif MOVE_ROW == 4
#define BENCH_ITERATIONS 24000000
#else
#define BENCH_ITERATIONS 16000000
#endif
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

#define MOVE_IS_W32(index) ((MOVE_W32 >> (index)) & 1)

#if MOVE_IS_W32(0)
#define MOVE_0 mov w10, w9
#else
#define MOVE_0 mov x10, x9
#endif
#if MOVE_CHAIN
#define MOVE_1 mov x11, x10
#define MOVE_2 mov x12, x11
#define MOVE_3 mov x13, x12
#define MOVE_4 mov x14, x13
#define MOVE_5 mov x15, x14
#else
#if MOVE_IS_W32(1)
#define MOVE_1 mov w11, w20
#else
#define MOVE_1 mov x11, x20
#endif
#if MOVE_IS_W32(2)
#define MOVE_2 mov w12, w21
#else
#define MOVE_2 mov x12, x21
#endif
#if MOVE_IS_W32(3)
#define MOVE_3 mov w13, w22
#else
#define MOVE_3 mov x13, x22
#endif
#if MOVE_IS_W32(4)
#define MOVE_4 mov w14, w23
#else
#define MOVE_4 mov x14, x23
#endif
#if MOVE_IS_W32(5)
#define MOVE_5 mov w15, w24
#else
#define MOVE_5 mov x15, x24
#endif
#endif

#if MOVE_ROW == 2
#define MOVE_LAST x11
#elif MOVE_ROW == 3
#define MOVE_LAST x12
#elif MOVE_ROW == 4
#define MOVE_LAST x13
#elif MOVE_ROW == 5
#define MOVE_LAST x14
#else
#define MOVE_LAST x15
#endif

/* Behind move n in a split group, before move n + 1. */
#if MOVE_SPLIT
#define MOVE_BETWEEN(register) add register, register, #1
#define MOVE_AFTER(register)
#else
#define MOVE_BETWEEN(register)
#define MOVE_AFTER(register) add register, register, #1
#endif

.macro move_row_group
	add	x9, x9, #1
	MOVE_0
	MOVE_BETWEEN(x2)
	MOVE_1
#if MOVE_ROW > 2
	MOVE_BETWEEN(x3)
	MOVE_2
#endif
#if MOVE_ROW > 3
	MOVE_BETWEEN(x4)
	MOVE_3
#endif
#if MOVE_ROW > 4
	MOVE_BETWEEN(x5)
	MOVE_4
#endif
#if MOVE_ROW > 5
	MOVE_BETWEEN(x6)
	MOVE_5
#endif
	add	x16, x16, MOVE_LAST
	MOVE_AFTER(x2)
#if MOVE_ROW > 2
	MOVE_AFTER(x3)
#endif
#if MOVE_ROW > 3
	MOVE_AFTER(x4)
#endif
#if MOVE_ROW > 4
	MOVE_AFTER(x5)
#endif
#if MOVE_ROW > 5
	MOVE_AFTER(x6)
#endif
.endm

/* Fails the probe unless `register` holds the source `source` as move
 * `index` copies it. */
.macro move_row_check register, source, index
	.if MOVE_IS_W32(\index)
	and	x18, \source, #0xffffffff
	.else
	eor	x18, \source, xzr
	.endif
	cmp	\register, x18
	b.ne	.Lfailure
.endm

	probe_start
	/* Sources above 32 bits, each different. */
	movz	x20, #0x2001
	movk	x20, #0x1020, lsl #32
	movz	x21, #0x2102
	movk	x21, #0x1121, lsl #32
	movz	x22, #0x2203
	movk	x22, #0x1222, lsl #32
	movz	x23, #0x2304
	movk	x23, #0x1323, lsl #32
	movz	x24, #0x2405
	movk	x24, #0x1424, lsl #32
	movz	x9, #0
	movz	x16, #0
	movz	x2, #0
	movz	x3, #0
	movz	x4, #0
	movz	x5, #0
	movz	x6, #0
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16

	.p2align 6
.Lloop:
	move_row_group
	move_row_group
	move_row_group
	move_row_group
	subs	x19, x19, #1
	b.ne	.Lloop

	/* X17 = the groups run, which X9, the first destination and every
	 * other register hold. */
	movz	x17, #(ITERATIONS & 0xffff)
	movk	x17, #((ITERATIONS >> 16) & 0xffff), lsl #16
	lsl	x17, x17, #2
	cmp	x9, x17
	b.ne	.Lfailure
	cmp	x10, x17
	b.ne	.Lfailure
	cmp	x2, x17
	b.ne	.Lfailure
#if MOVE_ROW > 2
	cmp	x3, x17
	b.ne	.Lfailure
#endif
#if MOVE_ROW > 3
	cmp	x4, x17
	b.ne	.Lfailure
#endif
#if MOVE_ROW > 4
	cmp	x5, x17
	b.ne	.Lfailure
#endif
#if MOVE_ROW > 5
	cmp	x6, x17
	b.ne	.Lfailure
#endif
#if MOVE_CHAIN
	/* Every destination holds X9, and X16 is 1 + 2 + ... + groups. */
	cmp	x11, x17
	b.ne	.Lfailure
#if MOVE_ROW > 2
	cmp	x12, x17
	b.ne	.Lfailure
#endif
#if MOVE_ROW > 3
	cmp	x13, x17
	b.ne	.Lfailure
#endif
#if MOVE_ROW > 4
	cmp	x14, x17
	b.ne	.Lfailure
#endif
#if MOVE_ROW > 5
	cmp	x15, x17
	b.ne	.Lfailure
#endif
	add	x18, x17, #1
	mul	x18, x18, x17
	lsr	x18, x18, #1
#else
	/* X16 is the groups times the last destination, which is checked
	 * last and so left in X18. */
	move_row_check x11, x20, 1
#if MOVE_ROW > 2
	move_row_check x12, x21, 2
#endif
#if MOVE_ROW > 3
	move_row_check x13, x22, 3
#endif
#if MOVE_ROW > 4
	move_row_check x14, x23, 4
#endif
#if MOVE_ROW > 5
	move_row_check x15, x24, 5
#endif
	mul	x18, x18, x17
#endif
	cmp	x16, x18
	b.ne	.Lfailure

	probe_finish

	probe_end
	.section .note.GNU-stack, "", %progbits
