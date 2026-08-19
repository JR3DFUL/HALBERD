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

/* Collision result block D_8012BCA0: one N64 object of 0x58 bytes (flags,
 * five ColRecords, then the water annex), splintered into 14 doubled bss
 * fragments on PC while compiled code writes rec[2..4] far past the first
 * one. Defined whole here at the LP64 layout of struct UnkBCA0 (ovl2_7.c);
 * every splinter name becomes an alias at its LP64-equivalent offset.
 * D_8012BCA4 is special: compiled code reads the flags halfword through
 * `&D_8012BCA4[-1]`, so it must sit at base+4 (N64 adjacency), not at
 * rec[0]'s field. The offsets below are locked by the mirror struct and
 * static asserts. */
struct PcColRecordMirror { s32 type; struct X *tri; struct X *norm; };
struct PcUnkBCA0Mirror {
    union { u32 w; } flags;
    struct PcColRecordMirror rec[5];
    void *waterRec[3];
    u32 waterSrc[3];
};
_Static_assert(sizeof(struct PcColRecordMirror) == 24, "ColRecord LP64 size");
_Static_assert(__builtin_offsetof(struct PcUnkBCA0Mirror, rec) == 8, "rec base");
_Static_assert(__builtin_offsetof(struct PcUnkBCA0Mirror, rec[1].tri) == 40, "rec1 tri");
_Static_assert(__builtin_offsetof(struct PcUnkBCA0Mirror, rec[2].type) == 56, "rec2 type");
_Static_assert(__builtin_offsetof(struct PcUnkBCA0Mirror, rec[4].norm) == 120, "rec4 norm");
_Static_assert(__builtin_offsetof(struct PcUnkBCA0Mirror, waterRec) == 128, "annex");
_Static_assert(__builtin_offsetof(struct PcUnkBCA0Mirror, waterSrc) == 152, "annex ids");

u8 D_8012BCA0[168] __attribute__((aligned(8)));

ALIAS(D_8012BCA4, D_8012BCA0, 4)     /* flags idiom: &D_8012BCA4[-1] == base */
ALIAS(D_8012BCA8, D_8012BCA0, 16)    /* rec[0].tri */
ALIAS(D_8012BCB4, D_8012BCA0, 40)    /* rec[1].tri */
ALIAS(D_8012BCBC, D_8012BCA0, 56)    /* rec[2].type */
ALIAS(D_8012BCC0, D_8012BCA0, 64)    /* rec[2].tri */
ALIAS(D_8012BCC4, D_8012BCA0, 72)    /* rec[2].norm */
ALIAS(D_8012BCC8, D_8012BCA0, 80)    /* rec[3].type */
ALIAS(D_8012BCCC, D_8012BCA0, 88)    /* rec[3].tri */
ALIAS(D_8012BCD0, D_8012BCA0, 96)    /* rec[3].norm */
ALIAS(D_8012BCD4, D_8012BCA0, 104)   /* rec[4].type */
ALIAS(D_8012BCD8, D_8012BCA0, 112)   /* rec[4].tri */
ALIAS(D_8012BCDC, D_8012BCA0, 120)   /* rec[4].norm */
ALIAS(D_8012BCE0, D_8012BCA0, 128)   /* water annex */
