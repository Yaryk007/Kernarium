/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Same-base translation reuse.
 *
 * x25 holds K, the host address of the latest leader or follower of the
 * current block execution, or a value with bit 63 set when there is none. A
 * leader is an established transfer entry that also sets K from its own TLB
 * hit; a follower is a transfer whose base register is unwritten since the
 * transfer before it through that base in the same direction, which set K. It
 * reuses that translation: it adds d, its byte displacement from that
 * transfer, to K when the access fits the 4 KiB guest page K points into, and
 * transfers through K + d, which becomes K. Otherwise it runs as its leader,
 * which translates. The builder pairs them (program_builder.cpp), and only a
 * transfer a follower follows becomes a leader.
 *
 * d is a signed field of the follower's operand that its established entry
 * never reads: bits 15:0 for the in-place family, whose transferred register
 * is baked, and bits 55:48 elsewhere. A leader that misses or crosses a page
 * sets K invalid and re-dispatches its word as the established entry, which
 * does what it always did; the in-place leaders that are not baked, which
 * have no established entry of their own form, keep their own miss and
 * crossing paths. Every entry is appended to the run part, so no established
 * gadget moves.
 *
 * Nothing else in the engine names x25 (repository test
 * arm64-gadget-same-base-register); gadget_host_frame.inc saves it and
 * helpers preserve it.
 */

.if UML_GADGET_GUEST_PAGE_SHIFT != 12
	.error "the room check assumes 4 KiB guest pages"
.endif

	.pushsection GADGET_SECTION_RUNS

/* K + d when the access of \bytes stays in K's guest page, else \fallback;
 * an invalid K (bit 63) fails for every d. Scratch: x9, x10. */
.macro sb_room bytes, dfield, fallback
	.if \dfield == 0
	sbfx x9, x8, #0, #16
	.else
	sbfx x9, x8, #48, #8
	.endif
	and x10, x25, #0x8000000000000fff
	add x10, x10, x9
	cmp x10, #(4096 - (\bytes))
	b.hi \fallback
	add x25, x25, x9
.endm

/* The leader's miss, crossing and misaligned-SP exit: K is invalid and the
 * established entry runs the transfer from its word. */
.macro sb_rewind name, established
Lsb_cold_\name:
	movn x25, #0
	sub _stream, _stream, #8
	b CSYM(uml_arm64_gadget_\established)
.endm

/* The guest address of a single-register form in x7, from the operand in x8
 * (Rn offset 31:16, displacement 47:32), with SP's alignment checked and a
 * tag stripped as the established entries do. */
.macro sb_single_address forward, base_sp, signed, misaligned
	ubfx x20, x8, #16, #16
	.if \signed
	sbfx x21, x8, #32, #16
	.else
	ubfx x21, x8, #32, #16
	.endif
	scalar_read \forward, 1, x7, x20
	.if \base_sp
	tst x7, #0xf
	b.ne \misaligned
	.endif
	add x7, x7, x21
	.if !\base_sp
	tbi_strip x7
	.endif
.endm

.macro sb_translate load, bytes, address, cold, resume
	.if \load
	transfer_translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, \bytes, \address, \cold, \resume, \cold
	.else
	transfer_translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, \bytes, \address, \cold, \resume, \cold
	.endif
.endm

/* Accesses through \a, as the established fast paths do them. A leader's
 * forwarded store value is in x22 (a helper may run before it); a
 * follower's is still in x0. Rt's CpuState offset is x19 unless baked. */
.macro sb_access_scalar_load a, leader, forward, instruction, host_register, zero, baked=0
	\instruction \host_register\()0, [\a]
	.if !\zero
	.if \baked
	str x0, [_vcpu, #(\baked & 0x7fff)]
	.else
	str x0, [_vcpu, x19]
	.endif
	.endif
.endm

.macro sb_access_scalar_store a, leader, forward, instruction, host_register, zero, baked=0
	.if \zero
	mov x0, xzr
	.elseif !(\forward & 2)
	.if \baked
	ldr x0, [_vcpu, #(\baked & 0x7fff)]
	.else
	ldr x0, [_vcpu, x19]
	.endif
	.endif
	.if !\zero && (\forward & 2) && \leader
	\instruction \host_register\()22, [\a]
	.else
	\instruction \host_register\()0, [\a]
	.endif
.endm

.macro sb_access_vector_load a, leader, forward, host_register, unused0=0, unused1=0
	ldr \host_register\()0, [\a]
	str q0, [_vcpu, x19]
.endm

.macro sb_access_vector_store a, leader, forward, host_register, unused0=0, unused1=0
	ldr q0, [_vcpu, x19]
	str \host_register\()0, [\a]
.endm

.macro sb_access_pair_load a, instruction, host_register, first_zero, second_zero
	\instruction \host_register\()6, \host_register\()7, [\a]
	.if !\first_zero
	str x6, [_vcpu, x22]
	.endif
	.if !\second_zero
	str x7, [_vcpu, x23]
	.endif
.endm

.macro sb_access_pair_store a, instruction, host_register, first_zero, second_zero
	.if \first_zero
	mov x6, xzr
	.else
	ldr x6, [_vcpu, x22]
	.endif
	.if \second_zero
	mov x7, xzr
	.else
	ldr x7, [_vcpu, x23]
	.endif
	\instruction \host_register\()6, \host_register\()7, [\a]
.endm

.macro sb_access_vpair_load a, instruction, host_register, unused0=0, unused1=0
	ldp \host_register\()0, \host_register\()1, [\a]
	str q0, [_vcpu, x22]
	str q1, [_vcpu, x23]
.endm

.macro sb_access_vpair_store a, instruction, host_register, unused0=0, unused1=0
	ldr q0, [_vcpu, x22]
	ldr q1, [_vcpu, x23]
	stp \host_register\()0, \host_register\()1, [\a]
.endm

/* A single-register leader and follower whose established entry \established
 * (with forwarding mask \forward) reads Rt's offset from the operand. */
.macro sb_single name, established, load, bytes, base_sp, forward, signed, kind, a1, a2=0, a3=0, follower=1
gadget sb_lead_\name
	ldr x8, [_stream], #8
Lsb_body_\name:
	.if !\load && (\forward & 2)
	mov x22, x0
	.endif
	and x19, x8, #0xffff
	sb_single_address \forward, \base_sp, \signed, Lsb_cold_\name
	sb_translate \load, \bytes, x7, Lsb_cold_\name, Lsb_resume_\name
	mov x25, x7
	sb_access_\kind x7, 1, \forward, \a1, \a2, \a3
	next_gadget
	sb_rewind \name, \established
	.if \follower
gadget sb_follow_\name
	ldr x8, [_stream], #8
	sb_room \bytes, 1, Lsb_body_\name
	and x19, x8, #0xffff
	sb_access_\kind x25, 0, \forward, \a1, \a2, \a3
	next_gadget
	.endif
.endm

/* A pair leader and follower: \kind pair (Rt 8:0, Rt2 17:9, displacement
 * 31:18, Rn 47:32) or vpair (Rt 11:0, Rt2 23:12, Rn 35:24, displacement
 * 47:36); \bytes is one element. */
.macro sb_pair name, established, load, bytes, base_sp, kind, a1, a2, a3=0, a4=0
gadget sb_lead_\name
	ldr x8, [_stream], #8
Lsb_body_\name:
	.ifc \kind, pair
	and x22, x8, #0x1ff
	ubfx x23, x8, #9, #9
	ubfx x24, x8, #32, #16
	sbfx x6, x8, #18, #14
	.else
	and x22, x8, #0xfff
	ubfx x23, x8, #12, #12
	ubfx x24, x8, #24, #12
	sbfx x6, x8, #36, #12
	.endif
	ldr x16, [_vcpu, x24]
	.if \base_sp
	tst x16, #0xf
	b.ne Lsb_cold_\name
	.endif
	add x19, x16, x6
	.if !\base_sp
	tbi_strip x19
	.endif
	sb_translate \load, (2 * \bytes), x19, Lsb_cold_\name, Lsb_resume_\name
	mov x25, x19
	.if \load
	sb_access_\kind\()_load x19, \a1, \a2, \a3, \a4
	.else
	sb_access_\kind\()_store x19, \a1, \a2, \a3, \a4
	.endif
	next_gadget
	sb_rewind \name, \established
gadget sb_follow_\name
	ldr x8, [_stream], #8
	sb_room (2 * \bytes), 1, Lsb_body_\name
	.ifc \kind, pair
	and x22, x8, #0x1ff
	ubfx x23, x8, #9, #9
	.else
	and x22, x8, #0xfff
	ubfx x23, x8, #12, #12
	.endif
	.if \load
	sb_access_\kind\()_load x25, \a1, \a2, \a3, \a4
	.else
	sb_access_\kind\()_store x25, \a1, \a2, \a3, \a4
	.endif
	next_gadget
.endm

/* The in-place family through a GPR base: one leader and one follower per
 * transferred register, baked as the established entry is. */
.macro sb_inplace name, load, bits64, reg
	.set Lsbb\@, 4
	.if \bits64
	.set Lsbb\@, 8
	.endif
	.set Lsbr\@, 0x8000 | (\reg * 8)
gadget sb_lead_\name
	ldr x8, [_stream], #8
Lsb_body_\name:
	sb_single_address 0, 0, 0, Lsb_cold_\name
	sb_translate \load, Lsbb\@, x7, Lsb_cold_\name, Lsb_resume_\name
	mov x25, x7
	.if \load
	.if \bits64
	sb_access_scalar_load x7, 1, 0, ldr, x, 0, Lsbr\@
	.else
	sb_access_scalar_load x7, 1, 0, ldr, w, 0, Lsbr\@
	.endif
	.else
	.if \bits64
	sb_access_scalar_store x7, 1, 0, str, x, 0, Lsbr\@
	.else
	sb_access_scalar_store x7, 1, 0, str, w, 0, Lsbr\@
	.endif
	.endif
	next_gadget
	sb_rewind \name, \name
gadget sb_follow_\name
	ldr x8, [_stream], #8
	sb_room Lsbb\@, 0, Lsb_body_\name
	.if \load
	.if \bits64
	sb_access_scalar_load x25, 0, 0, ldr, x, 0, Lsbr\@
	.else
	sb_access_scalar_load x25, 0, 0, ldr, w, 0, Lsbr\@
	.endif
	.else
	.if \bits64
	sb_access_scalar_store x25, 0, 0, str, x, 0, Lsbr\@
	.else
	sb_access_scalar_store x25, 0, 0, str, w, 0, Lsbr\@
	.endif
	.endif
	next_gadget
.endm

/* The in-place forms that are not baked: an SP base, and every forwarded
 * variant. No established entry has this form to re-dispatch to, so these
 * keep the established family's own miss, crossing and alignment paths. */
.macro sb_inplace_shared name, load, bits64, base_sp, forward, follower
	.set Lsbb\@, 4
	.if \bits64
	.set Lsbb\@, 8
	.endif
gadget sb_lead_\name
	ldr x8, [_stream], #8
Lsb_body_\name:
	.if !\load && (\forward & 2)
	mov x22, x0
	.endif
	and x19, x8, #0xffff
	sb_single_address \forward, \base_sp, 0, Lsb_misaligned_\name
	.if \load
	transfer_translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, Lsbb\@, x7, Lsb_miss_\name, Lsb_resume_\name, Lsb_cross_\name
	.else
	transfer_translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, Lsbb\@, x7, Lsb_miss_\name, Lsb_resume_\name, Lsb_cross_\name
	.endif
	mov x25, x7
	.if \load
	.if \bits64
	sb_access_scalar_load x7, 1, \forward, ldr, x, 0
	.else
	sb_access_scalar_load x7, 1, \forward, ldr, w, 0
	.endif
	.else
	.if \bits64
	sb_access_scalar_store x7, 1, \forward, str, x, 0
	.else
	sb_access_scalar_store x7, 1, \forward, str, w, 0
	.endif
	.endif
	next_gadget
	.if \load
	transfer_translate_cold 0, UML_GADGET_TLB_ENTRY_READ_PAGE, Lsbb\@, x7, Lsb_miss_\name, Lsb_resume_\name, Lsb_cross_\name, \forward, outofline=1, fault=uml_arm64_gadget_memory_fault_indexed
	.else
	transfer_translate_cold 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, Lsbb\@, x7, Lsb_miss_\name, Lsb_resume_\name, Lsb_cross_\name, \forward, outofline=1, fault=uml_arm64_gadget_memory_fault_indexed
	.endif
Lsb_cross_\name:
	movn x25, #0
	.if \load
	transfer_slow 0, x7, Lsbb\@, fault=uml_arm64_gadget_memory_fault_indexed
	.if \bits64
	fmov x0, d0
	.else
	fmov w0, s0
	.endif
	str x0, [_vcpu, x19]
	.else
	.if !(\forward & 2)
	ldr x0, [_vcpu, x19]
	.endif
	scalar_store_vector \forward, 2, \bits64
	transfer_slow 1, x7, Lsbb\@, fault=uml_arm64_gadget_memory_fault_indexed
	.endif
	next_gadget
Lsb_misaligned_\name:
	.if \load
	b Lscalar_alignment_fault_read_indexed
	.else
	b Lscalar_alignment_fault_write_indexed
	.endif
	.if \follower
gadget sb_follow_\name
	ldr x8, [_stream], #8
	sb_room Lsbb\@, 1, Lsb_body_\name
	and x19, x8, #0xffff
	.if \load
	.if \bits64
	sb_access_scalar_load x25, 0, \forward, ldr, x, 0
	.else
	sb_access_scalar_load x25, 0, \forward, ldr, w, 0
	.endif
	.else
	.if \bits64
	sb_access_scalar_store x25, 0, \forward, str, x, 0
	.else
	sb_access_scalar_store x25, 0, \forward, str, w, 0
	.endif
	.endif
	next_gadget
	.endif
.endm

/* The in-place family. */
.macro sb_inplace_modes load, bits64, prefix
	.irp reg, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
	sb_inplace \prefix\()_r_\reg, \load, \bits64, \reg
	.endr
.endm
sb_inplace_modes 0, 0, store32_unsigned_inplace
sb_inplace_modes 0, 1, store64_unsigned_inplace
sb_inplace_modes 1, 0, load32_unsigned_inplace
sb_inplace_modes 1, 1, load64_unsigned_inplace
.purgem sb_inplace_modes

/* A doubleword transfer through SP is a run element (program_builder.cpp),
 * never a leader or a follower, so only the word forms have SP entries. */
.macro sb_inplace_shared_modes op, load, bits64
	sb_inplace_shared \op\()_inplace_f1, \load, \bits64, 0, 1, 0
	.if !\load
	sb_inplace_shared \op\()_inplace_f2, \load, \bits64, 0, 2, 1
	sb_inplace_shared \op\()_inplace_f3, \load, \bits64, 0, 3, 0
	.endif
	.if !\bits64
	sb_inplace_shared \op\()_inplace_sp, \load, \bits64, 1, 0, 1
	sb_inplace_shared \op\()_inplace_sp_f1, \load, \bits64, 1, 1, 0
	.if !\load
	sb_inplace_shared \op\()_inplace_sp_f2, \load, \bits64, 1, 2, 1
	sb_inplace_shared \op\()_inplace_sp_f3, \load, \bits64, 1, 3, 0
	.endif
	.endif
.endm
sb_inplace_shared_modes store32, 0, 0
sb_inplace_shared_modes store64, 0, 1
sb_inplace_shared_modes load32, 1, 0
sb_inplace_shared_modes load64, 1, 1
.purgem sb_inplace_shared_modes

/* The XZR forms of the same family. */
.macro sb_zero_modes op, load, bytes
	.if \load
	sb_single \op\()_unsigned_fast_z, \op\()_unsigned_fast_z, 1, \bytes, 0, 0, 0, scalar_load, ldr, w, 1
	sb_single \op\()_unsigned_fast_z_sp, \op\()_unsigned_fast_z_sp, 1, \bytes, 1, 0, 0, scalar_load, ldr, w, 1
	.else
	sb_single \op\()_unsigned_fast_z, \op\()_unsigned_fast_z, 0, \bytes, 0, 0, 0, scalar_store, str, w, 1
	sb_single \op\()_unsigned_fast_z_sp, \op\()_unsigned_fast_z_sp, 0, \bytes, 1, 0, 0, scalar_store, str, w, 1
	.endif
.endm
sb_zero_modes store32, 0, 4
sb_zero_modes load32, 1, 4
.purgem sb_zero_modes
	sb_single store64_unsigned_fast_z, store64_unsigned_fast_z, 0, 8, 0, 0, 0, scalar_store, str, x, 1
	sb_single load64_unsigned_fast_z, load64_unsigned_fast_z, 1, 8, 0, 0, 0, scalar_load, ldr, x, 1

/* Narrow unsigned-offset loads and stores: a leader for the established
 * entry and, for loads, for its forwarded-base variant. */
.macro sb_narrow_load_mode name, mode, instruction, host_register, bytes, base_sp, zero
	sb_single \name\()_\mode, \name\()_\mode, 1, \bytes, \base_sp, 0, 0, scalar_load, \instruction, \host_register, \zero
	sb_single \name\()_\mode\()_forward_1, \name\()_\mode\()_forward_1, 1, \bytes, \base_sp, 1, 0, scalar_load, \instruction, \host_register, \zero, follower=0
.endm
.macro sb_narrow_load name, instruction, host_register, bytes
	sb_narrow_load_mode \name, r, \instruction, \host_register, \bytes, 0, 0
	sb_narrow_load_mode \name, z, \instruction, \host_register, \bytes, 0, 1
	sb_narrow_load_mode \name, r_sp, \instruction, \host_register, \bytes, 1, 0
	sb_narrow_load_mode \name, z_sp, \instruction, \host_register, \bytes, 1, 1
.endm
sb_narrow_load load8_unsigned_fast, ldrb, w, 1
sb_narrow_load load16_unsigned_fast, ldrh, w, 2
sb_narrow_load load8_signed_w_fast, ldrsb, w, 1
sb_narrow_load load8_signed_x_fast, ldrsb, x, 1
sb_narrow_load load16_signed_w_fast, ldrsh, w, 2
sb_narrow_load load16_signed_x_fast, ldrsh, x, 2
sb_narrow_load load32_signed_x_fast, ldrsw, x, 4
.purgem sb_narrow_load
.purgem sb_narrow_load_mode

.macro sb_narrow_store name, instruction, bytes
	sb_single \name\()_r, \name\()_r, 0, \bytes, 0, 0, 0, scalar_store, \instruction, w, 0
	sb_single \name\()_z, \name\()_z, 0, \bytes, 0, 0, 0, scalar_store, \instruction, w, 1
	sb_single \name\()_r_sp, \name\()_r_sp, 0, \bytes, 1, 0, 0, scalar_store, \instruction, w, 0
	sb_single \name\()_z_sp, \name\()_z_sp, 0, \bytes, 1, 0, 0, scalar_store, \instruction, w, 1
.endm
sb_narrow_store store8_unsigned_fast, strb, 1
sb_narrow_store store16_unsigned_fast, strh, 2
.purgem sb_narrow_store

/* Unscaled transfers through a GPR base (signed displacement). */
.macro sb_unscaled name, load, instruction, host_register, bytes
	.if \load
	sb_single scalar_other_\name\()_offset_r, scalar_other_\name\()_offset_r, 1, \bytes, 0, 0, 1, scalar_load, \instruction, \host_register, 0
	sb_single scalar_other_\name\()_offset_z, scalar_other_\name\()_offset_z, 1, \bytes, 0, 0, 1, scalar_load, \instruction, \host_register, 1
	.else
	sb_single scalar_other_\name\()_offset_r, scalar_other_\name\()_offset_r, 0, \bytes, 0, 0, 1, scalar_store, \instruction, \host_register, 0
	sb_single scalar_other_\name\()_offset_z, scalar_other_\name\()_offset_z, 0, \bytes, 0, 0, 1, scalar_store, \instruction, \host_register, 1
	.endif
.endm
sb_unscaled store8, 0, strb, w, 1
sb_unscaled load8u, 1, ldrb, w, 1
sb_unscaled load8sx, 1, ldrsb, x, 1
sb_unscaled load8sw, 1, ldrsb, w, 1
sb_unscaled store16, 0, strh, w, 2
sb_unscaled load16u, 1, ldrh, w, 2
sb_unscaled load16sx, 1, ldrsh, x, 2
sb_unscaled load16sw, 1, ldrsh, w, 2
sb_unscaled store32, 0, str, w, 4
sb_unscaled load32u, 1, ldr, w, 4
sb_unscaled load32sx, 1, ldrsw, x, 4
sb_unscaled store64, 0, str, x, 8
sb_unscaled load64u, 1, ldr, x, 8
.purgem sb_unscaled

/* SIMD&FP single registers: unsigned offset (and its forwarded-base variant)
 * and unscaled. */
.macro sb_vector name, load, host_register, bytes
	.if \load
	sb_single vector_\name\()_unsigned_fast, vector_\name\()_unsigned_fast, 1, \bytes, 0, 0, 0, vector_load, \host_register
	sb_single vector_\name\()_unsigned_fast_forward_1, vector_\name\()_unsigned_fast_forward_1, 1, \bytes, 0, 1, 0, vector_load, \host_register, follower=0
	sb_single vector_\name\()_unsigned_fast_sp, vector_\name\()_unsigned_fast_sp, 1, \bytes, 1, 0, 0, vector_load, \host_register
	sb_single vector_\name\()_unsigned_fast_sp_forward_1, vector_\name\()_unsigned_fast_sp_forward_1, 1, \bytes, 1, 1, 0, vector_load, \host_register, follower=0
	sb_single vector_other_\name\()_offset, vector_other_\name\()_offset, 1, \bytes, 0, 0, 1, vector_load, \host_register
	sb_single vector_other_\name\()_offset_sp, vector_other_\name\()_offset_sp, 1, \bytes, 1, 0, 1, vector_load, \host_register
	.else
	sb_single vector_\name\()_unsigned_fast, vector_\name\()_unsigned_fast, 0, \bytes, 0, 0, 0, vector_store, \host_register
	sb_single vector_\name\()_unsigned_fast_forward_1, vector_\name\()_unsigned_fast_forward_1, 0, \bytes, 0, 1, 0, vector_store, \host_register, follower=0
	sb_single vector_\name\()_unsigned_fast_sp, vector_\name\()_unsigned_fast_sp, 0, \bytes, 1, 0, 0, vector_store, \host_register
	sb_single vector_\name\()_unsigned_fast_sp_forward_1, vector_\name\()_unsigned_fast_sp_forward_1, 0, \bytes, 1, 1, 0, vector_store, \host_register, follower=0
	sb_single vector_other_\name\()_offset, vector_other_\name\()_offset, 0, \bytes, 0, 0, 1, vector_store, \host_register
	sb_single vector_other_\name\()_offset_sp, vector_other_\name\()_offset_sp, 0, \bytes, 1, 0, 1, vector_store, \host_register
	.endif
.endm
sb_vector load8, 1, b, 1
sb_vector load16, 1, h, 2
sb_vector load32, 1, s, 4
sb_vector load64, 1, d, 8
sb_vector load128, 1, q, 16
sb_vector store8, 0, b, 1
sb_vector store16, 0, h, 2
sb_vector store32, 0, s, 4
sb_vector store64, 0, d, 8
sb_vector store128, 0, q, 16
.purgem sb_vector

/* GPR pairs, signed offset, through a GPR base. */
.macro sb_integer_pair name, load, instruction, host_register, bytes
	sb_pair integer_pair_\name\()_0_rr, integer_pair_\name\()_0_rr, \load, \bytes, 0, pair, \instruction, \host_register, 0, 0
	sb_pair integer_pair_\name\()_0_rz, integer_pair_\name\()_0_rz, \load, \bytes, 0, pair, \instruction, \host_register, 0, 1
	sb_pair integer_pair_\name\()_0_zr, integer_pair_\name\()_0_zr, \load, \bytes, 0, pair, \instruction, \host_register, 1, 0
	sb_pair integer_pair_\name\()_0_zz, integer_pair_\name\()_0_zz, \load, \bytes, 0, pair, \instruction, \host_register, 1, 1
.endm
sb_integer_pair store32, 0, stp, w, 4
sb_integer_pair load32, 1, ldp, w, 4
sb_integer_pair load32s, 1, ldpsw, x, 4
sb_integer_pair store64, 0, stp, x, 8
sb_integer_pair load64, 1, ldp, x, 8
.purgem sb_integer_pair

/* SIMD&FP pairs, signed offset. */
.macro sb_vector_pair name, load, host_register, bytes
	sb_pair vector_pair_\name\()_0, vector_pair_\name\()_0, \load, \bytes, 0, vpair, ldp, \host_register
	sb_pair vector_pair_\name\()_0_sp, vector_pair_\name\()_0_sp, \load, \bytes, 1, vpair, ldp, \host_register
.endm
sb_vector_pair store32, 0, s, 4
sb_vector_pair load32, 1, s, 4
sb_vector_pair store64, 0, d, 8
sb_vector_pair load64, 1, d, 8
sb_vector_pair store128, 0, q, 16
sb_vector_pair load128, 1, q, 16
.purgem sb_vector_pair

	.popsection

/* The builder's table: one row per established entry, its leaders by the
 * forwarding mask the builder selected (0 when that variant has none), its
 * followers likewise, and
 *   bits 7:0  the family (0 in-place, 1 narrow, 2 unscaled,
 *             3 SIMD&FP single, 4 GPR pair, 5 SIMD&FP pair);
 *   bit 8     the mask-0 follower's d is in bits 55:48 (else 15:0);
 *   bit 9     the mask-2 follower's likewise. */
.macro sb_ptr name
	.ifc \name, 0
	.quad 0
	.else
	.quad CSYM(uml_arm64_gadget_\name)
	.endif
.endm

.macro sb_row established, family, dfields, l0, l1=0, l2=0, l3=0, f0=0, f1=0, f2=0, f3=0
	sb_ptr \established
	sb_ptr \l0
	sb_ptr \l1
	sb_ptr \l2
	sb_ptr \l3
	sb_ptr \f0
	sb_ptr \f1
	sb_ptr \f2
	sb_ptr \f3
	.quad (\family) | ((\dfields) << 8)
.endm

.pushsection __DATA_CONST,__gadget_inplace
.p2align 3
.globl CSYM(uml_arm64_gadget_same_base_rows)
CSYM(uml_arm64_gadget_same_base_rows):

.macro sb_inplace_rows op, prefix, load, bits64
	.irp reg, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
	.if \load
	sb_row \prefix\()_r_\reg, 0, 0, sb_lead_\prefix\()_r_\reg, sb_lead_\op\()_inplace_f1, 0, 0, sb_follow_\prefix\()_r_\reg
	.if !\bits64
	sb_row \prefix\()_r_sp_\reg, 0, 1, sb_lead_\op\()_inplace_sp, sb_lead_\op\()_inplace_sp_f1, 0, 0, sb_follow_\op\()_inplace_sp
	.endif
	.else
	sb_row \prefix\()_r_\reg, 0, 2, sb_lead_\prefix\()_r_\reg, sb_lead_\op\()_inplace_f1, sb_lead_\op\()_inplace_f2, sb_lead_\op\()_inplace_f3, sb_follow_\prefix\()_r_\reg, 0, sb_follow_\op\()_inplace_f2
	.if !\bits64
	sb_row \prefix\()_r_sp_\reg, 0, 3, sb_lead_\op\()_inplace_sp, sb_lead_\op\()_inplace_sp_f1, sb_lead_\op\()_inplace_sp_f2, sb_lead_\op\()_inplace_sp_f3, sb_follow_\op\()_inplace_sp, 0, sb_follow_\op\()_inplace_sp_f2
	.endif
	.endif
	.endr
.endm
sb_inplace_rows store32, store32_unsigned_inplace, 0, 0
sb_inplace_rows store64, store64_unsigned_inplace, 0, 1
sb_inplace_rows load32, load32_unsigned_inplace, 1, 0
sb_inplace_rows load64, load64_unsigned_inplace, 1, 1
.purgem sb_inplace_rows

.irp name, store32_unsigned_fast_z, store32_unsigned_fast_z_sp, load32_unsigned_fast_z, load32_unsigned_fast_z_sp, store64_unsigned_fast_z, load64_unsigned_fast_z
	sb_row \name, 0, 1, sb_lead_\name, 0, 0, 0, sb_follow_\name
.endr

/* An .irp parameter is not substituted before \(), so each family's names
 * are formed by a macro. */
.macro sb_narrow_rows name, load
	.irp mode, r, z, r_sp, z_sp
	sb_narrow_row \name, \mode, \load
	.endr
.endm
.macro sb_narrow_row name, mode, load
	.if \load
	sb_row \name\()_\mode, 1, 1, sb_lead_\name\()_\mode, sb_lead_\name\()_\mode\()_forward_1, 0, 0, sb_follow_\name\()_\mode
	.else
	sb_row \name\()_\mode, 1, 1, sb_lead_\name\()_\mode, 0, 0, 0, sb_follow_\name\()_\mode
	.endif
.endm
.irp name, load8_unsigned_fast, load16_unsigned_fast, load8_signed_w_fast, load8_signed_x_fast, load16_signed_w_fast, load16_signed_x_fast, load32_signed_x_fast
	sb_narrow_rows \name, 1
.endr
.irp name, store8_unsigned_fast, store16_unsigned_fast
	sb_narrow_rows \name, 0
.endr

.macro sb_unscaled_row name
	sb_row scalar_other_\name\()_offset_r, 2, 1, sb_lead_scalar_other_\name\()_offset_r, 0, 0, 0, sb_follow_scalar_other_\name\()_offset_r
	sb_row scalar_other_\name\()_offset_z, 2, 1, sb_lead_scalar_other_\name\()_offset_z, 0, 0, 0, sb_follow_scalar_other_\name\()_offset_z
.endm
.irp name, store8, load8u, load8sx, load8sw, store16, load16u, load16sx, load16sw, store32, load32u, load32sx, store64, load64u
	sb_unscaled_row \name
.endr

.macro sb_vector_row name
	sb_row vector_\name\()_unsigned_fast, 3, 1, sb_lead_vector_\name\()_unsigned_fast, sb_lead_vector_\name\()_unsigned_fast_forward_1, 0, 0, sb_follow_vector_\name\()_unsigned_fast
	sb_row vector_\name\()_unsigned_fast_sp, 3, 1, sb_lead_vector_\name\()_unsigned_fast_sp, sb_lead_vector_\name\()_unsigned_fast_sp_forward_1, 0, 0, sb_follow_vector_\name\()_unsigned_fast_sp
	sb_row vector_other_\name\()_offset, 3, 1, sb_lead_vector_other_\name\()_offset, 0, 0, 0, sb_follow_vector_other_\name\()_offset
	sb_row vector_other_\name\()_offset_sp, 3, 1, sb_lead_vector_other_\name\()_offset_sp, 0, 0, 0, sb_follow_vector_other_\name\()_offset_sp
.endm
.irp name, load8, load16, load32, load64, load128, store8, store16, store32, store64, store128
	sb_vector_row \name
.endr

.macro sb_integer_pair_row name, zero
	sb_row integer_pair_\name\()_0_\zero, 4, 1, sb_lead_integer_pair_\name\()_0_\zero, 0, 0, 0, sb_follow_integer_pair_\name\()_0_\zero
.endm
.macro sb_integer_pair_rows name
	.irp zero, rr, rz, zr, zz
	sb_integer_pair_row \name, \zero
	.endr
.endm
.irp name, store32, load32, load32s, store64, load64
	sb_integer_pair_rows \name
.endr

.macro sb_vector_pair_row name
	sb_row vector_pair_\name\()_0, 5, 1, sb_lead_vector_pair_\name\()_0, 0, 0, 0, sb_follow_vector_pair_\name\()_0
	sb_row vector_pair_\name\()_0_sp, 5, 1, sb_lead_vector_pair_\name\()_0_sp, 0, 0, 0, sb_follow_vector_pair_\name\()_0_sp
.endm
.irp name, store32, load32, store64, load64, store128, load128
	sb_vector_pair_row \name
.endr

.globl CSYM(uml_arm64_gadget_same_base_rows_end)
CSYM(uml_arm64_gadget_same_base_rows_end):
.popsection

.purgem sb_vector_pair_row
.purgem sb_integer_pair_rows
.purgem sb_integer_pair_row
.purgem sb_vector_row
.purgem sb_unscaled_row
.purgem sb_narrow_row
.purgem sb_narrow_rows
.purgem sb_row
.purgem sb_ptr
.purgem sb_pair
.purgem sb_single
.purgem sb_inplace
.purgem sb_inplace_shared
.purgem sb_access_vpair_store
.purgem sb_access_vpair_load
.purgem sb_access_pair_store
.purgem sb_access_pair_load
.purgem sb_access_vector_store
.purgem sb_access_vector_load
.purgem sb_access_scalar_store
.purgem sb_access_scalar_load
.purgem sb_translate
.purgem sb_single_address
.purgem sb_rewind
.purgem sb_room
