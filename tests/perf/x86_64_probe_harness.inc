/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Entry and exit harness shared by the freestanding x86-64 guest_x86_64_*.S
 * probes, the counterpart of probe_harness.inc. It switches the assembler to
 * Intel syntax without prefixes and emits no code until a probe expands its
 * macros.
 *
 * probe_start opens the text section at the global _start entry point.
 *
 * probe_finish writes "OK\n" to standard output and exits 0, with a trap
 * behind the exit. It defines the .Lfailure label, which exits 1, for the
 * probe's checks.
 *
 * probe_end closes _start and emits the success message.
 *
 * A probe enters its measured loop with a jump from the setup and leaves it
 * with a jump to the result check, even where the jump's target is the next
 * instruction, and assembles the data the loop reads rather than filling it
 * at run time. The engine ends a block at a jump and places each block's
 * stream behind those translated before it, so only the setup's blocks lie
 * in front of the loop's, and none of them holds the loop, the check or the
 * exit.
 */

#define X86_64_PROBE_NR_WRITE 1
#define X86_64_PROBE_NR_EXIT_GROUP 231

	.intel_syntax noprefix

.macro probe_start
	.section .text
	.global _start
	.type _start, @function
_start:
.endm

.macro probe_finish
	mov	eax, X86_64_PROBE_NR_WRITE
	mov	edi, 1
	lea	rsi, [rip + .Lsuccess_message]
	mov	edx, 3
	syscall
	xor	edi, edi
	jmp	.Lexit
.Lfailure:
	mov	edi, 1
.Lexit:
	mov	eax, X86_64_PROBE_NR_EXIT_GROUP
	syscall
	ud2
.endm

.macro probe_end
	.size _start, .-_start

	.section .rodata
.Lsuccess_message:
	.ascii "OK\n"
.endm
