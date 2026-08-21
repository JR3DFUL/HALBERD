/* func_800BE320 / func_800BE374 -- the game's setjmp/longjmp -- for the
 * x86-64 WINDOWS build. The Windows makefile compiles this file INSTEAD of
 * src/pc/pc_setjmp.c; everything in that file's long comment (why this must
 * be the function the game calls, why no wrapper can work) applies here and
 * is not repeated. What changes is the ABI, in three ways:
 *
 * 1. ARGUMENT REGISTERS. MS x64 passes (buf, val) in rcx, rdx -- not
 *    rdi, rsi. Every buffer access below is off %rcx.
 *
 * 2. THE CALLEE-SAVED SET IS BIGGER. SysV: rbx rbp r12-r15. MS x64 adds
 *    rsi, rdi and xmm6-xmm15. A longjmp that fails to restore those breaks
 *    the setjmp caller's live values. The GPRs fit the game's 0x6C buffer:
 *    8 GPRs + rsp + return address = 0x50 bytes <= 0x6C. The ten xmm
 *    registers (0xA0 bytes) do NOT fit, so they go to a side slot keyed by
 *    the buffer address:
 *
 *      game_tick (src/main/main.c) is the only setjmp caller, with one
 *      static buffer, so ONE slot suffices; it is a slot rather than a bare
 *      global only so that a second buffer appearing in a future decompiled
 *      file trips the guard (slot busy with a DIFFERENT buf -> abort with a
 *      message) instead of silently corrupting float state. Nesting depth
 *      one, same-buffer re-setjmp (the game re-arms every tick) and
 *      longjmp-many-times all work; two concurrent live buffers is the one
 *      unsupported shape, and it announces itself.
 *
 * 3. ASSEMBLER DIRECTIVES. PE/COFF: no .type/.size (those lines are what
 *    made pc_setjmp.c fail to assemble under mingw); .def/.endef instead,
 *    kept minimal. Win64 C symbols have no leading underscore, same as ELF.
 *
 * MXCSR / x87 CW are still deliberately unsaved -- same reasoning as the
 * SysV file: nothing in the port changes rounding modes, and restoring a
 * stale control word is a bug vector, not a safety net.
 *
 * No SEH unwind info is emitted (.seh_proc): these functions never unwind --
 * one returns normally, the other RESTORES a context -- and the game builds
 * with -fno-exceptions semantics (plain C, no pthreads-cancel). A debugger
 * stepping through longjmp sees a bare jmp, which is the truth.
 */
#include <ultra64.h>

#include "pc/pc_platform.h"

#if !defined(__x86_64__) || !defined(_WIN32)
#error "pc_setjmp_win64.c is the MS-x64 variant; use src/pc/pc_setjmp.c \
for System V targets."
#endif

/* One side slot: buffer address + xmm6..xmm15 (16 bytes each, 16-aligned so
 * movaps can be used). Referenced from the asm below by name. */
struct pc_sjw64_slot {
    void *buf;
    unsigned char xmm[10 * 16] __attribute__((aligned(16)));
};
struct pc_sjw64_slot pc_sjw64_slot0;

void pc_sjw64_two_bufs_abort(void); /* defined after the asm */

__asm__(
    ".text\n"

    ".globl func_800BE320\n"
    ".def func_800BE320; .scl 2; .type 32; .endef\n"
    "func_800BE320:\n"
    /* GPRs into the game's buffer (rcx). Layout extends the SysV file's:
     *   +0x00 rbx  +0x08 rbp  +0x10 r12  +0x18 r13
     *   +0x20 r14  +0x28 r15  +0x30 rsp  +0x38 return address
     *   +0x40 rsi  +0x48 rdi                                     (MS extra) */
    "    movq %rbx,  0x00(%rcx)\n"
    "    movq %rbp,  0x08(%rcx)\n"
    "    movq %r12,  0x10(%rcx)\n"
    "    movq %r13,  0x18(%rcx)\n"
    "    movq %r14,  0x20(%rcx)\n"
    "    movq %r15,  0x28(%rcx)\n"
    "    leaq 8(%rsp), %rax\n"          /* caller's rsp */
    "    movq %rax,  0x30(%rcx)\n"
    "    movq (%rsp), %rax\n"           /* return address */
    "    movq %rax,  0x38(%rcx)\n"
    "    movq %rsi,  0x40(%rcx)\n"
    "    movq %rdi,  0x48(%rcx)\n"
    /* Claim (or re-arm) the xmm side slot. Free slot or same buffer: take
     * it. A DIFFERENT live buffer: abort loudly -- see header comment. */
    "    leaq pc_sjw64_slot0(%rip), %rax\n"
    "    movq (%rax), %rdx\n"
    "    testq %rdx, %rdx\n"
    "    je 1f\n"
    "    cmpq %rdx, %rcx\n"
    "    je 1f\n"
    "    jmp pc_sjw64_two_bufs_abort\n" /* diverges; noreturn */
    "1:  movq %rcx, (%rax)\n"
    "    movaps %xmm6,  0x10(%rax)\n"
    "    movaps %xmm7,  0x20(%rax)\n"
    "    movaps %xmm8,  0x30(%rax)\n"
    "    movaps %xmm9,  0x40(%rax)\n"
    "    movaps %xmm10, 0x50(%rax)\n"
    "    movaps %xmm11, 0x60(%rax)\n"
    "    movaps %xmm12, 0x70(%rax)\n"
    "    movaps %xmm13, 0x80(%rax)\n"
    "    movaps %xmm14, 0x90(%rax)\n"
    "    movaps %xmm15, 0xA0(%rax)\n"
    "    xorl %eax, %eax\n"
    "    ret\n"

    ".globl func_800BE374\n"
    ".def func_800BE374; .scl 2; .type 32; .endef\n"
    "func_800BE374:\n"
    "    movq 0x00(%rcx), %rbx\n"
    "    movq 0x08(%rcx), %rbp\n"
    "    movq 0x10(%rcx), %r12\n"
    "    movq 0x18(%rcx), %r13\n"
    "    movq 0x20(%rcx), %r14\n"
    "    movq 0x28(%rcx), %r15\n"
    "    movq 0x40(%rcx), %rsi\n"
    "    movq 0x48(%rcx), %rdi\n"
    /* xmm6..15 back from the side slot, if this buffer owns it. A longjmp
     * through a buffer that never claimed the slot restores GPRs only --
     * matching what the SysV file does for ALL float state. */
    "    leaq pc_sjw64_slot0(%rip), %rax\n"
    "    cmpq (%rax), %rcx\n"
    "    jne 2f\n"
    "    movaps 0x10(%rax), %xmm6\n"
    "    movaps 0x20(%rax), %xmm7\n"
    "    movaps 0x30(%rax), %xmm8\n"
    "    movaps 0x40(%rax), %xmm9\n"
    "    movaps 0x50(%rax), %xmm10\n"
    "    movaps 0x60(%rax), %xmm11\n"
    "    movaps 0x70(%rax), %xmm12\n"
    "    movaps 0x80(%rax), %xmm13\n"
    "    movaps 0x90(%rax), %xmm14\n"
    "    movaps 0xA0(%rax), %xmm15\n"
    "2:  movq 0x38(%rcx), %r8\n"        /* return address (r8: volatile) */
    "    movq 0x30(%rcx), %rsp\n"       /* caller's stack, frame intact */
    /* C requires longjmp(buf, 0) to make setjmp return 1. */
    "    movl %edx, %eax\n"
    "    testl %eax, %eax\n"
    "    jne 3f\n"
    "    movl $1, %eax\n"
    "3:  jmp *%r8\n"
);

/* Out-of-line so the asm above stays register-pure; noreturn by fiat. */
#include <stdio.h>
#include <stdlib.h>
void pc_sjw64_two_bufs_abort(void) {
    fprintf(stderr,
            "[pc] func_800BE320: a second concurrent setjmp buffer appeared; "
            "pc_setjmp_win64.c supports one (see its header comment).\n");
    abort();
}
