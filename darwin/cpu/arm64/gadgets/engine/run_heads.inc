/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Run heads, in a part of their own behind every other part of the region,
 * so that no established entry moves for them.
 *
 * An epilogue head stands in front of a run the builder recognized: SP-based
 * 64-bit pair loads, 64-bit single loads and at most one ADD SP, SP, #imm,
 * then the RET through X30 that ends the block. The run's own words and the
 * RET's follow the head unchanged; a hint between them that the virtual CPU
 * runs as a NOP, such as AUTIASP, leaves no word. When SP is 16-byte aligned and the TLB
 * maps every byte the head reads in one guest page for reading, the head does
 * the whole run: each load, the final SP and the return, which takes X30 from
 * the register the last pair load left it in rather than from CpuState.
 * Otherwise it dispatches the run's first word, and the run executes, faults
 * and retires exactly as it would without a head.
 *
 * A head is named for its loads and its ADD SP count, which fix where the
 * RET's cell is, so the cell's load waits for no operand word.
 *
 * The page test is transfer_translate's with a span the builder computes: the
 * entry is indexed by the page of the first byte and keyed by the page of the
 * last, so a span that crosses a page, or wraps, can only miss.
 *
 * The first operand word:
 *   bits 12:0   the first byte the head reads, in 8-byte units from SP at
 *               entry, signed
 *   bits 21:13  the last byte it reads, in bytes from the first
 *   bits 33:22  SP at exit, in 16-byte units from SP at entry, signed
 *   bits 48:34  the first load
 *   bits 63:49  the second load
 * then four loads a word, at bits 0, 15, 30 and 45. A load is its address in
 * 8-byte units from the first byte (bits 4:0), then the CpuState indices of
 * Rt (9:5) and Rt2 (14:10). Every load reads 16 bytes and stores Rt2 before
 * Rt, so a single load names its register twice and keeps its own 8 bytes;
 * the builder counts the other 8 in the span. The pair that loads X30 as
 * Rt2 is the last load, so X30 is in x10 when the loads are done.
 */
	.pushsection GADGET_SECTION_RUNS

/* Each element and the RET is a group of two words, behind its
 * execution_count group in a counting build. */
#if UML_GADGET_EXECUTION_HISTOGRAM
	.set Lrun_count_words, 3
#else
	.set Lrun_count_words, 0
#endif

.macro run_epilogue_load word, shift
	ubfx x11, \word, #\shift, #5
	ubfx x12, \word, #(\shift + 5), #5
	ubfx x13, \word, #(\shift + 10), #5
	add x11, x16, x11, lsl #3
	ldp x9, x10, [x11]
	str x10, [_vcpu, x13, lsl #3]
	str x9, [_vcpu, x12, lsl #3]
.endm

.macro run_epilogue loads, adds
	.set Lrun_operand_words\@, 1 + (\loads + 1) / 4
	/* The cell, in words from the first operand word: past the operands,
	 * the elements' groups and the RET's counting group and gadget word. */
	.set Lrun_elements\@, (\loads + \adds) * (Lrun_count_words + 2)
	.set Lrun_cell\@, Lrun_operand_words\@ + Lrun_elements\@ + Lrun_count_words + 1
gadget run_epilogue_\loads\()_\adds
	ldr x8, [_stream]
	ldr x17, [_vcpu, #UML_GADGET_CPU_SP]
	sbfx x9, x8, #0, #13
	ubfx x10, x8, #13, #9
	tst x17, #0xf
	b.ne Lrun_epilogue_fallback\@
	add x16, x17, x9, lsl #3
	add x11, x16, x10
	ubfx x9, x16, #UML_GADGET_GUEST_PAGE_SHIFT, #UML_GADGET_TLB_INDEX_BITS
	add x10, _swtlb, x9, lsl #UML_GADGET_TLB_SET_SHIFT
	ldp x12, x13, [x10, #UML_GADGET_TLB_ENTRY_READ_PAGE]
	lsr x11, x11, #UML_GADGET_GUEST_PAGE_SHIFT
	cmp x12, x11
	b.ne Lrun_epilogue_fallback\@
	add x16, x13, x16
	sbfx x15, x8, #22, #12
	add x17, x17, x15, lsl #4
	run_epilogue_load x8, 34
	.if \loads > 1
	run_epilogue_load x8, 49
	.endif
	.if \loads > 2
	ldr x14, [_stream, #8]
	run_epilogue_load x14, 0
	.endif
	.if \loads > 3
	run_epilogue_load x14, 15
	.endif
	.if \loads > 4
	run_epilogue_load x14, 30
	.endif
	.if \loads > 5
	run_epilogue_load x14, 45
	.endif
	.if \loads > 6
	ldr x14, [_stream, #16]
	run_epilogue_load x14, 0
	.endif
	.if \loads > 7
	run_epilogue_load x14, 15
	.endif
	str x17, [_vcpu, #UML_GADGET_CPU_SP]
	ldr x9, [_stream, #(8 * Lrun_cell\@)]
	b Lreturn_x30_follow
Lrun_epilogue_fallback\@:
	/* Past the operand words, to the run's first word: in a counting
	 * build its execution_count group, which names the head as the
	 * first word's predecessor, so the census counts the fallbacks. */
	add _stream, _stream, #(8 * Lrun_operand_words\@)
	next_gadget
.endm

	.irp loads, 1, 2, 3, 4, 5, 6, 7, 8
	run_epilogue \loads, 0
	run_epilogue \loads, 1
	.endr

.purgem run_epilogue
.purgem run_epilogue_load

/*
 * A prologue head stands in front of a run the builder recognized: SP-based
 * 64-bit pair stores, at most one 64-bit single store, at most one
 * SUB SP, SP, #imm and exactly one ADD X29, SP, #imm (MOV X29, SP among
 * them), with at least one pair. The run's own words follow the head
 * unchanged. When SP is 16-byte aligned and the TLB maps every byte the head
 * writes in one guest page for writing, the head does the whole run: each
 * store, then the final SP and X29, and continues behind the run's words.
 * Otherwise it dispatches the run's first word, and the run executes, faults
 * and retires exactly as it would without a head.
 *
 * The page test is the epilogue's with the write entry, which only a
 * translation for writing fills, so a hit means the page is writable now. A
 * page below the stack that the guest has not touched, a guard page and a
 * read-only page all miss, as does a span across a page, and the head then
 * stores nothing: the run's words fault where they would, at the store that
 * reaches the page, with the words before it done. On a hit the head computes
 * every address from SP at entry, stores each register as CpuState holds it,
 * and writes SP and X29 last. That is the run's own order for every value:
 * the builder refuses a run that stores X29 after setting it, and no store can
 * name SP. Stores below SP before SP is lowered would be lost to a signal frame
 * pushed between them, but nothing runs between them: a page the TLB holds
 * for writing cannot fault, and an interrupt is taken only between blocks.
 *
 * A head is named for its pairs, its single store and its SUB SP count, which
 * fix how far behind the head the run's words end, so the next dispatch waits
 * for no operand word.
 *
 * The first operand word:
 *   bits 9:0    the first byte the head writes, in 8-byte units from SP at
 *               entry, signed
 *   bits 18:10  the last byte it writes, in bytes from the first
 *   bits 28:19  SP at exit, in 16-byte units from SP at entry, signed
 *   bits 33:29  X29 at exit, in 8-byte units from the first byte
 *   bits 48:34  the first store
 *   bits 63:49  the second store
 * then four stores a word, at bits 0, 15, 30 and 45. A store is its address
 * in 8-byte units from the first byte (bits 4:0), then the CpuState indices
 * of Rt (9:5) and Rt2 (14:10). The single store, when there is one, is the
 * first and writes only Rt's 8 bytes.
 */
.macro run_prologue_pair word, shift
	ubfx x11, \word, #\shift, #5
	ubfx x12, \word, #(\shift + 5), #5
	ubfx x13, \word, #(\shift + 10), #5
	ldr x9, [_vcpu, x12, lsl #3]
	ldr x10, [_vcpu, x13, lsl #3]
	add x11, x16, x11, lsl #3
	stp x9, x10, [x11]
.endm

.macro run_prologue_single word, shift
	ubfx x11, \word, #\shift, #5
	ubfx x12, \word, #(\shift + 5), #5
	ldr x9, [_vcpu, x12, lsl #3]
	str x9, [x16, x11, lsl #3]
.endm

/* The store in operand slot `slot`, of `kind` pair or single. */
.macro run_prologue_slot slot, kind
	.if \slot == 0
	run_prologue_\kind x8, 34
	.elseif \slot == 1
	run_prologue_\kind x8, 49
	.elseif \slot == 2
	ldr x14, [_stream, #8]
	run_prologue_\kind x14, 0
	.elseif \slot == 3
	run_prologue_\kind x14, 15
	.elseif \slot == 4
	run_prologue_\kind x14, 30
	.elseif \slot == 5
	run_prologue_\kind x14, 45
	.elseif \slot == 6
	ldr x14, [_stream, #16]
	run_prologue_\kind x14, 0
	.else
	.error "a prologue head has at most seven stores"
	.endif
.endm

.macro run_prologue pairs, single, subs
	.set Lrun_operand_words\@, 1 + (\pairs + \single + 1) / 4
	/* Past the operands and every element's group: one store each, the
	 * SUB SP and the ADD X29. */
	.set Lrun_elements\@, (\pairs + \single + \subs + 1) * (Lrun_count_words + 2)
	.set Lrun_skip\@, Lrun_operand_words\@ + Lrun_elements\@
gadget run_prologue_\pairs\()_\single\()_\subs
	ldr x8, [_stream]
	ldr x17, [_vcpu, #UML_GADGET_CPU_SP]
	sbfx x9, x8, #0, #10
	ubfx x10, x8, #10, #9
	tst x17, #0xf
	b.ne Lrun_prologue_fallback\@
	add x16, x17, x9, lsl #3
	add x11, x16, x10
	ubfx x9, x16, #UML_GADGET_GUEST_PAGE_SHIFT, #UML_GADGET_TLB_INDEX_BITS
	add x10, _swtlb, x9, lsl #UML_GADGET_TLB_SET_SHIFT
	ldp x12, x13, [x10, #UML_GADGET_TLB_ENTRY_WRITE_PAGE]
	lsr x11, x11, #UML_GADGET_GUEST_PAGE_SHIFT
	cmp x12, x11
	b.ne Lrun_prologue_fallback\@
	sbfx x15, x8, #19, #10
	add x17, x17, x15, lsl #4
	ubfx x15, x8, #29, #5
	add x15, x16, x15, lsl #3
	add x16, x13, x16
	.if \single
	run_prologue_slot 0, single
	.endif
	.if \pairs > 0
	run_prologue_slot (\single+0), pair
	.endif
	.if \pairs > 1
	run_prologue_slot (\single+1), pair
	.endif
	.if \pairs > 2
	run_prologue_slot (\single+2), pair
	.endif
	.if \pairs > 3
	run_prologue_slot (\single+3), pair
	.endif
	.if \pairs > 4
	run_prologue_slot (\single+4), pair
	.endif
	.if \pairs > 5
	run_prologue_slot (\single+5), pair
	.endif
	str x17, [_vcpu, #UML_GADGET_CPU_SP]
	str x15, [_vcpu, #(UML_GADGET_CPU_X0 + 8 * 29)]
	add _stream, _stream, #(8 * Lrun_skip\@)
	next_gadget
Lrun_prologue_fallback\@:
	/* As the epilogue's: the run's first word, which in a counting build
	 * names the head as its predecessor. */
	add _stream, _stream, #(8 * Lrun_operand_words\@)
	next_gadget
.endm

	.irp pairs, 1, 2, 3, 4, 5, 6
	run_prologue \pairs, 0, 0
	run_prologue \pairs, 0, 1
	run_prologue \pairs, 1, 0
	run_prologue \pairs, 1, 1
	.endr

.purgem run_prologue
.purgem run_prologue_slot
.purgem run_prologue_single
.purgem run_prologue_pair

/*
 * A move head replaces a row of register moves the builder recognized,
 * MOV Xd, Xm and MOV Wd, Wm (ORR with XZR, unshifted), and is the row's only
 * group. A move cannot fault and touches no memory but CpuState, so the head
 * has no fallback, and the block's retirement count, fault positions and
 * inline suffixes count the row's instructions as its ordinary words would.
 *
 * The builder hands the head the row as a parallel copy of two to four moves
 * with a destination each: a move that reads an earlier move's destination
 * reads that move's source instead, at the narrower of their widths, and a
 * move whose destination a later one writes is dropped. That is the row's
 * result in order, so the head loads every source before it stores any
 * destination.
 *
 * A head is named for the width of each move, the first first: run_moves_xw
 * is a 64-bit and then a 32-bit move. It leaves the last move's value in x0,
 * the register the builder forwards to the instruction behind the row; its
 * _forward form takes the first move's source from x0, where the group in
 * front of the row left it.
 *
 * The operand word holds the moves at bits 0, 15, 30 and 45: the source's
 * CpuState byte offset (bits 9:0; XZR reads the zero slot), then the
 * destination's CpuState index (bits 14:10).
 */
.macro run_move_load forward, width, scratch, value, shift
	.if \forward
	mov \width\value, \width\()0
	.else
	ubfx x\scratch, x8, #\shift, #10
	ldr \width\value, [_vcpu, x\scratch]
	.endif
.endm

.macro run_move_store value, scratch, shift
	ubfx x\scratch, x8, #(\shift + 10), #5
	str x\value, [_vcpu, x\scratch, lsl #3]
.endm

.macro run_moves forward, w0, w1, w2, w3
	.ifnb \w3
	.set Lrun_moves\@, 4
	.else
	.ifnb \w2
	.set Lrun_moves\@, 3
	.else
	.set Lrun_moves\@, 2
	.endif
	.endif
	.if \forward
gadget run_moves_\w0\w1\w2\w3\()_forward
	.else
gadget run_moves_\w0\w1\w2\w3
	.endif
	ldr x8, [_stream], #8
	run_move_load \forward, \w0, 9, 9, 0
	.if Lrun_moves\@ == 2
	run_move_load 0, \w1, 10, 0, 15
	.else
	run_move_load 0, \w1, 10, 10, 15
	.endif
	.if Lrun_moves\@ == 3
	run_move_load 0, \w2, 11, 0, 30
	.elseif Lrun_moves\@ == 4
	run_move_load 0, \w2, 11, 11, 30
	run_move_load 0, \w3, 12, 0, 45
	.endif
	run_move_store 9, 13, 0
	.if Lrun_moves\@ == 2
	run_move_store 0, 14, 15
	.else
	run_move_store 10, 14, 15
	.endif
	.if Lrun_moves\@ == 3
	run_move_store 0, 15, 30
	.elseif Lrun_moves\@ == 4
	run_move_store 11, 15, 30
	run_move_store 0, 16, 45
	.endif
	next_gadget
.endm

	.irp w0, x, w
	.irp w1, x, w
	run_moves 0, \w0, \w1
	run_moves 1, \w0, \w1
	.endr
	.endr
	.irp w0, x, w
	.irp w1, x, w
	.irp w2, x, w
	run_moves 0, \w0, \w1, \w2
	run_moves 1, \w0, \w1, \w2
	.endr
	.endr
	.endr
	.irp w0, x, w
	.irp w1, x, w
	.irp w2, x, w
	.irp w3, x, w
	run_moves 0, \w0, \w1, \w2, \w3
	run_moves 1, \w0, \w1, \w2, \w3
	.endr
	.endr
	.endr
	.endr

.purgem run_moves
.purgem run_move_store
.purgem run_move_load


	.popsection
