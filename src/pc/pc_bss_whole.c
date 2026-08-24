/* Split-bss regions consolidated into whole objects (see pc_save_bss.c for
 * the save-buffer case and the full story: splat splits one N64 object at
 * every interior label, the generator doubles each piece, and any code that
 * does pointer arithmetic across the region -- or indexes the base past the
 * first splinter -- lands in foreign memory on PC).
 *
 * HUD texture arena: 0x800ED510 .. 0x800F4D14 (0x7804 bytes). ovl1_13.c
 * indexes the base as u16[] out to at least [0x271A] (byte 0x4E34), far past
 * the first splinter, and func_800BDE0C row-walks the whole arena.
 *
 * THE END OF THIS ARENA WAS WRONG UNTIL IT CORRUPTED THE PLATFORM LAYER, and
 * the mistake is worth spelling out because the same shape will recur in
 * every other region in this file.
 *
 * It used to be declared 0x6E14 bytes, ending at D_800F4324 -- but
 * D_800F4324 is an INTERIOR LABEL of the arena, not its end, and so is
 * D_800F4D10 beyond it. The first symbol that genuinely belongs to something
 * else is D_800F4D14, which is what actually bounds the region and what
 * ovl1_13.c reads as a separate flag. Taking the first interior label for the
 * end made the object 0x9F0 bytes short.
 *
 * func_800BDE0C then DMAs a HUD theme into the base -- func_800A8934 sizes
 * the read from the asset table and it comes to 0x7800 bytes, which fits
 * 0x7804 exactly and overran 0x6E14 by 0x9F0. The overrun landed in the
 * platform layer's own .bss: the pc_dbg counters first, then the scripted
 * controller's statics, which is how it was caught -- src/pc/pc_input_script.c
 * printed an elapsed time of 390317930 seconds because its epoch had been
 * overwritten with the 16-bit HUD fill pattern (0xCBD4CBD4CBD4CBD4,
 * 0xFFBEFFBEFFBEFFBE). Nothing else in the process noticed, and nothing
 * would have: a wild write into a neighbouring static is silent until the
 * value it hit is used.
 *
 * SO THE RULE FOR THIS FILE: a region ends at the first symbol that is not
 * one of its own interior labels, and the way to tell is that the region's
 * own code names the interior ones through the base. Sizing to the last
 * alias someone happened to list is how you get an object that is exactly
 * long enough to look correct. */
#include "pc/pc_types.h"

u16 D_800ED510[0x7804 / 2] __attribute__((aligned(8)));

#define ALIAS(name, base, off) \
    __asm__("   .globl " #name "\n   .set " #name ", " #base " + " #off "\n");

ALIAS(D_800EDA10, D_800ED510, 0x500)
ALIAS(D_800EDA24, D_800ED510, 0x514)
ALIAS(D_800EDA60, D_800ED510, 0x550)
ALIAS(D_800F03C5, D_800ED510, 0x2EB5)
ALIAS(D_800F4324, D_800ED510, 0x6E14)
ALIAS(D_800F4D10, D_800ED510, 0x7800)

/* Collision result block D_8012BCA0: one N64 object of 0x58 bytes (flags,
 * five ColRecords, then the water annex), splintered into 14 doubled bss
 * fragments on PC while compiled code writes rec[2..4] far past the first
 * one. Defined whole here at the LP64 layout of struct CollisionResult
 * (ovl2_7.c, formerly spelled UnkBCA0);
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

/* Camera free-look config D_801292B0: one N64 object of 0x3C bytes (struct
 * Ovl2CamOut in ovl2_3.c -- twelve f32 then three u32, scalar-only so the
 * LP64 layout equals the N64 one), splintered by splat into a 0x18 head,
 * six f32 scalars and the 0x10 D_801292E0 block. func_800FBBB8 and
 * func_800F9974 (ovl2_3.c) memcpy the whole 0x3C to/from the D_80129270
 * save area and write fields unk18..unk2C across the splinter boundary, and
 * compiled code indexes D_801292C8 as f32[6] spanning C8..DF. Defined whole
 * here (0x40 to absorb D_801292E0's full N64 extent); each splinter name is
 * an alias at its NATIVE offset. */
u8 D_801292B0[0x40] __attribute__((aligned(8)));

ALIAS(D_801292C8, D_801292B0, 0x18)
ALIAS(D_801292CC, D_801292B0, 0x1C)
ALIAS(D_801292D0, D_801292B0, 0x20)
ALIAS(D_801292D4, D_801292B0, 0x24)
ALIAS(D_801292D8, D_801292B0, 0x28)
ALIAS(D_801292DC, D_801292B0, 0x2C)
ALIAS(D_801292E0, D_801292B0, 0x30)
