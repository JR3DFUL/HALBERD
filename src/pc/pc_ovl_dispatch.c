/* Cross-overlay VA dispatch.
 *
 * Three call targets in this build are N64 OVERLAY ADDRESSES reached from
 * outside the owning overlay -- either a direct call compiled into another
 * overlay's C (plylib.c calls 0x801693C4) or a function word inside a
 * converted data table (spawn tables carry 0x80198880 and 0x801DB1E0). On
 * the N64 the jump simply lands in whatever overlay is mapped at that VA;
 * in this statically linked build the splat map resolved each word to ONE
 * symbol, and when several overlays define the address (ovl10..ovl17 share
 * a VRAM window) the unsuffixed name won and fell through to a weak stub.
 *
 * This file gives those unsuffixed names strong definitions:
 *
 *   - func_801693C4 / func_80198880 each have exactly one real definition
 *     (ovl3's track allocator, ovl7's enemy-record helper); the callers
 *     only ever run while that overlay is resident, so they forward
 *     unconditionally. plylib's second argument has no home in the ovl3
 *     definition (a lone s32 parameter) -- on the N64 it merely rode along
 *     in $a1, so it is dropped here.
 *
 *   - func_801DB1E0 exists in EVERY overlay of the shared ovl10..ovl17
 *     window (level-entity mains occupying the same slot), so it must
 *     dispatch on which of those overlays the game most recently loaded.
 *     dma_overlay_load's PORT interception calls
 *     pc_ovl_dispatch_note_load() with the descriptor; matching it against
 *     gOverlayTable (descriptor i belongs to ovl(i+1)) recovers the
 *     overlay number the N64 would have had mapped.
 */
#include <stdio.h>

extern void *gOverlayTable[];

/* Last member of the ovl10..ovl17 VRAM-window family the game loaded
 * (1-based overlay number), -1 before any level/menu overlay load. */
static int sCurWindowOvl = -1;

void pc_ovl_dispatch_note_load(void *ovl) {
    int i;

    for (i = 0; i < 20; i++) {
        if (gOverlayTable[i] == ovl) {
            int n = i + 1;

            if (n >= 10 && n <= 17) {
                sCurWindowOvl = n;
            }
            return;
        }
    }
}

extern int func_801693C4_ovl3();
extern void func_80198880_ovl7();
extern void func_801DB1E0_ovl10();
extern void func_801DB1E0_ovl11();
extern void func_801DB1E0_ovl12();
extern void func_801DB1E0_ovl13();
extern void func_801DB1E0_ovl14();
extern void func_801DB1E0_ovl15();
extern void func_801DB1E0_ovl16();
extern void func_801DB1E0_ovl17();

void func_801693C4(int arg0, void *arg1) {
    (void)arg1; /* dead $a1 passenger at plylib's call site */
    func_801693C4_ovl3(arg0);
}

void func_80198880(void *arg0) {
    func_80198880_ovl7(arg0);
}

void func_801DB1E0(void *arg0) {
    switch (sCurWindowOvl) {
        case 10: func_801DB1E0_ovl10(arg0); break;
        case 11: func_801DB1E0_ovl11(arg0); break;
        case 12: func_801DB1E0_ovl12(arg0); break;
        case 13: func_801DB1E0_ovl13(arg0); break;
        case 14: func_801DB1E0_ovl14(arg0); break;
        case 15: func_801DB1E0_ovl15(arg0); break;
        case 16: func_801DB1E0_ovl16(arg0); break;
        case 17: func_801DB1E0_ovl17(arg0); break;
        default:
            fprintf(stderr,
                    "[pc] func_801DB1E0 dispatched with no ovl10..17 loaded\n");
            break;
    }
}
