/* The save buffers, defined WHOLE.
 *
 * On N64, gSaveBuffer1 (0x800EC9F8, 0x1B8 bytes) and gSaveBuffer2 (0x800ECBB0,
 * same) are single objects, and a dozen other names are just addresses inside
 * them: D_800EC9FC is header word 1 ("last used file"), D_800ECA08 is
 * files[0] (the file-select exists test reads its world field), D_800ECB10 is
 * the config block at +0x118, and so on. splat's bss listing split them at
 * every label, and the PC data generator emitted each splinter as its own
 * (doubled) object -- so struct writes through gSaveBuffer1 landed at native
 * offsets across a stretched patchwork, and every reader that used a splinter
 * name saw different memory than the writers. That is what produced the
 * half-real save records (world/level poisoned, minigame fields fine) that
 * bricked the world map.
 *
 * struct EEPROM is all scalars, so its LP64 layout equals the N64 layout and
 * plain native offsets are correct. The splinters become `.set` aliases at
 * those offsets, the same mechanism pc_camera_slots.c uses. gen_data.py
 * SUPPRESS_BSS keeps the generated bss from re-emitting any of these names.
 */
#include "pc/pc_types.h"

u8 gSaveBuffer1[0x1B8] __attribute__((aligned(8)));
u8 gSaveBuffer2[0x1B8] __attribute__((aligned(8)));

#define ALIAS(name, base, off) \
    __asm__("   .globl " #name "\n   .set " #name ", " #base " + " #off "\n");

ALIAS(D_800EC9FC, gSaveBuffer1, 0x4)
ALIAS(D_800ECA00, gSaveBuffer1, 0x8)
ALIAS(D_800ECA04, gSaveBuffer1, 0xC)
ALIAS(D_800ECA08, gSaveBuffer1, 0x10)
ALIAS(D_800ECA14, gSaveBuffer1, 0x1C)
ALIAS(D_800ECA5C, gSaveBuffer1, 0x64)
ALIAS(D_800ECA60, gSaveBuffer1, 0x68)
ALIAS(D_800ECAB8, gSaveBuffer1, 0xC0)
ALIAS(D_800ECB00, gSaveBuffer1, 0x108)
ALIAS(D_800ECB10, gSaveBuffer1, 0x118)
ALIAS(D_800ECBA8, gSaveBuffer1, 0x1B0)
ALIAS(D_800ECBAC, gSaveBuffer1, 0x1B4)
ALIAS(D_800ECBC0, gSaveBuffer2, 0x10)
