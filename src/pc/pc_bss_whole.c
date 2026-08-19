/* Split-bss regions consolidated into whole objects (see pc_save_bss.c for
 * the save-buffer case and the full story: splat splits one N64 object at
 * every interior label, the generator doubles each piece, and any code that
 * does pointer arithmetic across the region -- or indexes the base past the
 * first splinter -- lands in foreign memory on PC).
 *
 * HUD texture arena: 0x800ED510 .. 0x800F4324 (0x6E14 bytes). ovl1_13.c
 * indexes the base as u16[] out to at least [0x271A] (byte 0x4E34), far past
 * the first splinter, and func_800BDE0C row-walks the whole arena. */
#include "pc/pc_types.h"

u16 D_800ED510[0x6E14 / 2] __attribute__((aligned(8)));

#define ALIAS(name, base, off) \
    __asm__("   .globl " #name "\n   .set " #name ", " #base " + " #off "\n");

ALIAS(D_800EDA10, D_800ED510, 0x500)
ALIAS(D_800EDA24, D_800ED510, 0x514)
ALIAS(D_800EDA60, D_800ED510, 0x550)
ALIAS(D_800F03C5, D_800ED510, 0x2EB5)
