// SPDX-License-Identifier: Apache-2.0
//
// The static native-method thunks of the old-application layer (32-bit ARM).
//
// Dalvik freed the local references of a native method when it returned. ART does the same for its own references, but
// the layer's byte[] blocks live outside ART's tables, so the layer has to see the return. The layer hands ART the
// address of one of these thunks in place of a native function of the application's library (jni_shim.cpp,
// thunk_for); no code is generated, the thunks are part of this library.
//
// A thunk is `movw ip, #index; b entry`. The entry
//   - saves the arguments of the native method (r0-r3; the stack arguments are not touched),
//   - asks oldapp_thunk_enter(index, lr, sp) for the function to jump to and whether a frame record was pushed,
//   - when it was, points lr at a return stub in the state of the function (ARM or Thumb: a return that does not
//     interwork lands in the right state), and
//   - restores the arguments and jumps to the function with the stack as the runtime left it.
// The stub saves what a JNI function returns (r0 and r1: softfp has no other result register), asks
// oldapp_thunk_leave(sp) for the runtime's return address, which ends the blocks of the invocation, and returns there.
// With no record (too many nested invocations, no thread state) the function returns to the runtime directly.

    .syntax unified
    .arm
    .text

    .p2align 2
    .type oldapp_native_entry, %function
    .hidden oldapp_native_entry
oldapp_native_entry:
    push    {r0-r3}
    push    {ip, lr}
    add     r2, sp, #24             @ sp as the native method was entered
    mov     r1, lr
    mov     r0, ip
    bl      oldapp_thunk_enter      @ r0 = function, r1 = 1 when a record was pushed
    mov     ip, r0
    tst     r1, #1
    pop     {r2, lr}
    beq     1f
    tst     ip, #1                  @ a Thumb function returns through the Thumb stub
    adreq   lr, oldapp_native_return_arm
    adrne   lr, oldapp_native_return_thumb + 1
1:  pop     {r0-r3}
    bx      ip
    .size oldapp_native_entry, .-oldapp_native_entry

    .p2align 2
    .type oldapp_native_return_arm, %function
    .hidden oldapp_native_return_arm
oldapp_native_return_arm:
    push    {r0, r1}
    add     r0, sp, #8              @ sp as the native method was entered
    bl      oldapp_thunk_leave      @ r0 = the runtime's return address
    mov     lr, r0
    pop     {r0, r1}
    bx      lr
    .size oldapp_native_return_arm, .-oldapp_native_return_arm

    .thumb
    .thumb_func
    .p2align 2
    .type oldapp_native_return_thumb, %function
    .hidden oldapp_native_return_thumb
oldapp_native_return_thumb:
    push    {r0, r1}
    add     r0, sp, #8
    bl      oldapp_thunk_leave
    mov     lr, r0
    pop     {r0, r1}
    bx      lr
    .size oldapp_native_return_thumb, .-oldapp_native_return_thumb

    .arm
    .p2align 3
    .global oldapp_thunks
    .hidden oldapp_thunks
    .type oldapp_thunks, %function
oldapp_thunks:
    .set oldapp_thunk_index, 0
    .rept 1024
    movw    ip, #oldapp_thunk_index
    b       oldapp_native_entry
    .set oldapp_thunk_index, oldapp_thunk_index + 1
    .endr
    .global oldapp_thunks_end
    .hidden oldapp_thunks_end
oldapp_thunks_end:
    .size oldapp_thunks, .-oldapp_thunks

    .section .note.GNU-stack,"",%progbits
