/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Body shared by the prologue probes: a loop that calls one leaf function
 * whose prologue stores the frame record and the callee-saved registers the
 * leaf overwrites through SP and then points X29 at the frame record.
 * PROLOGUE_FRAME selects the leaf:
 *
 *   0  a 32-byte frame pushed by STP X29, X30 pre-index and STR of one
 *      register at an offset, then MOV X29, SP, the short prologue of
 *      leaves that save one register;
 *   1  a 96-byte frame of six pairs stored by STP at offsets below a SUB SP,
 *      then ADD X29, SP, the most frequent prologue in Geekbench and Clang.
 *
 * PROLOGUE_SPLIT moves the leaf's first body instruction, which reads and
 * writes only X0, from behind the prologue to between its stores and the
 * X29 set-up. Both arrangements execute the same instructions; only the
 * split one has an instruction between its stores and its frame pointer.
 *
 * The leaf's body reads the caller's X19 back through X29, so a frame
 * pointer or a store that went wrong changes the result. Its epilogue keeps
 * an instruction between the restores and the RET in both arrangements, so
 * only the prologue differs between them.
 *
 * The caller keeps its iteration count in X19 and a call count in X22 and
 * holds constants in X20, X21, X23 to X28 and X29, which the leaf stores and
 * the large frame overwrites, so a store that went wrong fails the probe, as
 * does a wrong result.
 *
 * The including file defines PROLOGUE_FRAME and PROLOGUE_SPLIT first.
 */

#ifndef BENCH_ITERATIONS
#if PROLOGUE_FRAME
#define BENCH_ITERATIONS 20000000
#else
#define BENCH_ITERATIONS 40000000
#endif
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	probe_start
	mov	x20, #0x20
	mov	x21, #0x21
	mov	x23, #0x23
	mov	x24, #0x24
	mov	x25, #0x25
	mov	x26, #0x26
	mov	x27, #0x27
	mov	x28, #0x28
	mov	x29, #0x29
	mov	x22, xzr
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16

.Lloop:
	mov	x0, x19
	bl	.Lleaf
	add	x22, x22, #1
	subs	x19, x19, #1
	b.ne	.Lloop

	/* The last call had X0 = 1: (1 + 7 + 3) ^ (1 << 1) = 9, plus 11 and X28
	 * in the large frame, plus 5. */
#if PROLOGUE_FRAME
	cmp	x0, #(9 + 11 + 0x28 + 5)
#else
	cmp	x0, #(9 + 5)
#endif
	b.ne	.Lfailure
	movz	x9, #(ITERATIONS & 0xffff)
	movk	x9, #((ITERATIONS >> 16) & 0xffff), lsl #16
	cmp	x22, x9
	b.ne	.Lfailure
	cmp	x20, #0x20
	b.ne	.Lfailure
	cmp	x21, #0x21
	b.ne	.Lfailure
	cmp	x23, #0x23
	b.ne	.Lfailure
	cmp	x24, #0x24
	b.ne	.Lfailure
	cmp	x25, #0x25
	b.ne	.Lfailure
	cmp	x26, #0x26
	b.ne	.Lfailure
	cmp	x27, #0x27
	b.ne	.Lfailure
	cmp	x28, #0x28
	b.ne	.Lfailure
	cmp	x29, #0x29
	b.ne	.Lfailure

	probe_finish

	.p2align 6
.Lleaf:
#if PROLOGUE_FRAME
	sub	sp, sp, #96
	stp	x29, x30, [sp, #80]
	stp	x20, x19, [sp, #64]
	stp	x22, x21, [sp, #48]
	stp	x24, x23, [sp, #32]
	stp	x26, x25, [sp, #16]
	stp	x28, x27, [sp]
#if PROLOGUE_SPLIT
	add	x0, x0, #7
#endif
	add	x29, sp, #80
#if !PROLOGUE_SPLIT
	add	x0, x0, #7
#endif
	ldur	x9, [x29, #-8]
	add	x19, x0, #3
	eor	x20, x19, x9, lsl #1
	add	x27, x20, x19
	add	x0, x27, x28
	mov	x22, x0
	mov	x24, x0
	ldp	x28, x27, [sp]
	ldp	x26, x25, [sp, #16]
	ldp	x24, x23, [sp, #32]
	ldp	x22, x21, [sp, #48]
	ldp	x20, x19, [sp, #64]
	ldp	x29, x30, [sp, #80]
	add	sp, sp, #96
	add	x0, x0, #5
	ret
#else
	stp	x29, x30, [sp, #-32]!
	str	x19, [sp, #16]
#if PROLOGUE_SPLIT
	add	x0, x0, #7
#endif
	mov	x29, sp
#if !PROLOGUE_SPLIT
	add	x0, x0, #7
#endif
	ldr	x9, [x29, #16]
	add	x19, x0, #3
	eor	x0, x19, x9, lsl #1
	ldr	x19, [sp, #16]
	ldp	x29, x30, [sp], #32
	add	x0, x0, #5
	ret
#endif

	probe_end
	.section .note.GNU-stack, "", %progbits
