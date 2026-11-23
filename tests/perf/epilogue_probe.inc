/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Body shared by the epilogue probes: a loop that calls one leaf function
 * with a frame, whose epilogue restores the callee-saved registers the leaf
 * overwrote and then returns. EPILOGUE_FRAME selects the leaf:
 *
 *   0  a frame of two pairs, pushed with a pre-index store and popped by
 *      LDP at an offset then LDP X29, X30 post-index, the shape most
 *      common leaves end in;
 *   1  a 96-byte frame of six pairs below a SUB SP, popped by six LDP at
 *      offsets then ADD SP, the most frequent epilogue in Geekbench and
 *      Clang.
 *
 * EPILOGUE_SPLIT moves the leaf's last body instruction, which reads and
 * writes only X0, from in front of the epilogue to between the epilogue's
 * last instruction and its RET. Both arrangements execute the same
 * instructions; only the split one has an instruction between its restores
 * and its return.
 *
 * EPILOGUE_HINT, with the two-pair frame, signs the return address on entry
 * with PACIASP and authenticates it with AUTIASP after the restores, as code
 * built for pointer authentication does; both are hints, and the virtual CPU,
 * which implements no pointer authentication, runs them as NOPs.
 *
 * The caller keeps its iteration count in X19 and a call count in X22 and
 * holds constants in X20, X21 and X23 to X28, which the leaf overwrites, so a
 * restore that went wrong fails the probe, as does a wrong result.
 *
 * The including file defines EPILOGUE_FRAME and EPILOGUE_SPLIT first, and
 * EPILOGUE_HINT when it wants it.
 */

#ifndef EPILOGUE_HINT
#define EPILOGUE_HINT 0
#endif

#ifndef BENCH_ITERATIONS
#if EPILOGUE_FRAME
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
	mov	x22, xzr
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16

.Lloop:
	mov	x0, x19
	bl	.Lleaf
	add	x22, x22, #1
	subs	x19, x19, #1
	b.ne	.Lloop

	/* The last call had X0 = 1: (1 + 3) ^ (1 << 1) = 6, plus 4 and X28 in
	 * the large frame or plus 4 in the small one, plus 7. */
#if EPILOGUE_FRAME
	cmp	x0, #(6 + 4 + 0x28 + 7)
#else
	cmp	x0, #(6 + 4 + 7)
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

	probe_finish

	.p2align 6
.Lleaf:
#if EPILOGUE_FRAME
	sub	sp, sp, #96
	stp	x29, x30, [sp, #80]
	stp	x20, x19, [sp, #64]
	stp	x22, x21, [sp, #48]
	stp	x24, x23, [sp, #32]
	stp	x26, x25, [sp, #16]
	stp	x28, x27, [sp]
	add	x29, sp, #80
	add	x19, x0, #3
	eor	x20, x19, x0, lsl #1
	add	x27, x20, x19
	add	x0, x27, x28
	mov	x22, x0
	mov	x24, x0
#if !EPILOGUE_SPLIT
	add	x0, x0, #7
#endif
	ldp	x28, x27, [sp]
	ldp	x26, x25, [sp, #16]
	ldp	x24, x23, [sp, #32]
	ldp	x22, x21, [sp, #48]
	ldp	x20, x19, [sp, #64]
	ldp	x29, x30, [sp, #80]
	add	sp, sp, #96
#if EPILOGUE_SPLIT
	add	x0, x0, #7
#endif
	ret
#else
#if EPILOGUE_HINT
	hint	#25		/* PACIASP */
#endif
	stp	x29, x30, [sp, #-32]!
	stp	x20, x19, [sp, #16]
	mov	x29, sp
	add	x19, x0, #3
	eor	x20, x19, x0, lsl #1
	add	x0, x20, x19
#if !EPILOGUE_SPLIT
	add	x0, x0, #7
#endif
	ldp	x20, x19, [sp, #16]
	ldp	x29, x30, [sp], #32
#if EPILOGUE_SPLIT
	add	x0, x0, #7
#endif
#if EPILOGUE_HINT
	hint	#29		/* AUTIASP */
#endif
	ret
#endif

	probe_end
	.section .note.GNU-stack, "", %progbits
