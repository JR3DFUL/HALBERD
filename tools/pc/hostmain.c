/* Host entry point for the native build.
 *
 * The N64 binary has no main(): the boot ROM jumps to EntryPoint, which sets
 * up the stack and starts the idle thread. EntryPoint is still assembly, so
 * for now this just calls it and the stub layer reports it as unimplemented --
 * which is the correct first answer, and the point of being able to run at all.
 *
 * When the boot path is decompiled or replaced, this is where the host-side
 * setup (window, audio device, ROM file) goes before handing over.
 */
#include <stdio.h>

extern void cboot(void);

/* The stub layer's end-of-run summary. It exists only on the tools/pc/link.sh
 * path, which generates build/pc/stubs.c and defines this STRONGLY, so that
 * definition wins there. The honest link (tools/pc/link.py, and the Makefile's
 * `link` target) has no stub layer at all and there is nothing to report, so
 * the weak do-nothing below is the whole truth for it. Without this the
 * stub-free link failed on one undefined reference -- from the port's own
 * entry file, which was the only thing standing between it and a binary. */
__attribute__((weak)) void pc_stub_report(void) {
}

/* asm/entry.s does two things: zero 0x589B0 bytes of RAM starting at
   gEntryStack, then jump to cboot with $sp pointing into gIdleThread.
   Neither survives the port. The host C runtime already zeroes statics, and
   the stack belongs to the host thread, so the faithful equivalent of the
   whole routine is the tail call. Defining it here (strongly) overrides the
   weak stub. */
void EntryPoint(void) {
    cboot();
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("kirby64-pc: entering game\n");
    fflush(stdout);
    EntryPoint();
    /* Only reached under KIRBY_PC_TRACE=1, where a missing symbol logs and
       returns instead of exiting. */
    pc_stub_report();
    return 0;
}
