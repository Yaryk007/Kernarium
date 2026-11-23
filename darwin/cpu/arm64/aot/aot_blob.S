/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The whole of the AOT framework's code: the blob tools/aot/rewrite.py
 * generates. It is position independent and carries its own tables, so the
 * image has no data segment, no relocation and no fixup. AOT_BLOB is the
 * path of blob.bin. */
	.section __TEXT,__text,regular,pure_instructions
	.p2align 14
	.globl _uml_aot_blob
_uml_aot_blob:
	.incbin AOT_BLOB
