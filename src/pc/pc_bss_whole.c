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
 * THE ARENA ENDS AT D_800F4D14, NOT AT D_800F4324, and the distinction is
 * worth spelling out because the same shape will recur in every other region
 * in this file.
 *
 * D_800F4324 is an INTERIOR LABEL of the arena, not its end, and so is
 * D_800F4D10 beyond it. The first symbol that genuinely belongs to something
 * else is D_800F4D14, which is what actually bounds the region and what
 * ovl1_13.c reads as a separate flag. Taking the first interior label for the
 * end (0x6E14 bytes) makes the object 0x9F0 bytes short.
 *
 * func_800BDE0C DMAs a HUD theme into the base -- func_800A8934 sizes the
 * read from the asset table and it comes to 0x7800 bytes, which fits 0x7804
 * exactly and overruns 0x6E14 by 0x9F0. Such an overrun lands in the
 * platform layer's own .bss: the pc_dbg counters first, then the scripted
 * controller's statics, where src/pc/pc_input_script.c's clock check reports
 * an elapsed time of 390317930 seconds because its state is overwritten with
 * the 16-bit HUD fill pattern (0xCBD4CBD4CBD4CBD4, 0xFFBEFFBEFFBEFFBE).
 * Nothing else in the process notices: a wild write into a neighbouring
 * static is silent until the value it hit is used.
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

/* Scene-camera at/eye snapshots (struct Ovl2CamPos in ovl2_3.c: two Vectors,
 * 0x18 bytes, scalar-only so LP64 == N64). D_800D7B20 is the live pair and
 * D_800D7B2C is its eye half by name: func_800FA2D4/func_800FC164 (ovl2_3.c),
 * ovl17.c and ovl1_2.c store `D_800D7B2C = cam->viewMtx.lookAt.eye` while
 * func_80100EE4 (ovl2_6.c) reads the eye back as D_800D7B20[3..5] for the
 * skybox parallax. Split into two objects the eye never arrived and the
 * layers were scrolled for a camera at the origin. D_800D7B38 is the
 * previous-frame pair AND, at +0x18, the six-float park block func_800FC62C
 * saves the live pair to across a camera animation; its N64 extent runs to
 * D_800D7B68, 0x30 bytes, not the 0x18 a split object had. */
u8 D_800D7B20[0x18] __attribute__((aligned(8)));
u8 D_800D7B38[0x30] __attribute__((aligned(8)));

ALIAS(D_800D7B2C, D_800D7B20, 0xC)

/* Player state gKirbyState (struct Player, include/Player.h): one N64 object
 * of 0x204 bytes at 0x8012E7C0, cut by splat at 23 interior labels that
 * compiled code reads and writes by name -- `*(s32 *)((u8 *)&D_8012E7E8 + 8)`
 * is gKirbyState.unk30, D_8012E7C5 is .action, gPositionState is the tail
 * from +0x1A8. Split into separate objects, the two spellings of one field
 * were two variables: the crouch coroutine (func_8016FD88_ovl3) signalled
 * "stand up" through D_8012E7E8 + 8 while the crouch tick (func_8016FFF8_ovl3)
 * waited on gKirbyState.unk30, so a D-DOWN press left Kirby crouched (action
 * 14, vel 0) for good. Defined whole at the LP64 layout: struct Player's one
 * pointer (unk114, N64 +0x114) moves to +0x118 and every later field 8 bytes
 * up, so labels below +0x114 sit at their N64 offset and the rest at N64 + 8,
 * each on the field it names (`ptype /o struct Player` on the binary: total
 * 528, unk114 at 280, unk144 at 332, unk1A8 at 432). */
u8 gKirbyState[0x210] __attribute__((aligned(8)));

ALIAS(D_8012E7C5, gKirbyState, 0x05)     /* action */
ALIAS(D_8012E7D7, gKirbyState, 0x17)     /* unk17 */
ALIAS(D_8012E7DC, gKirbyState, 0x1C)     /* floatTimer */
ALIAS(D_8012E7E8, gKirbyState, 0x28)     /* unk28; +8 is unk30 */
ALIAS(D_8012E7FC, gKirbyState, 0x3C)     /* unk3C; [2] is unk44 */
ALIAS(D_8012E80C, gKirbyState, 0x4C)     /* unk4C */
ALIAS(D_8012E818, gKirbyState, 0x58)
ALIAS(D_8012E81C, gKirbyState, 0x5C)
ALIAS(D_8012E820, gKirbyState, 0x60)
ALIAS(D_8012E824, gKirbyState, 0x64)
ALIAS(D_8012E828, gKirbyState, 0x68)
ALIAS(D_8012E850, gKirbyState, 0x90)     /* ability */
ALIAS(D_8012E860, gKirbyState, 0xA0)     /* abilityInUse */
ALIAS(D_8012E894, gKirbyState, 0xD4)     /* damageType */
ALIAS(D_8012E8C2, gKirbyState, 0x102)    /* floorType */
ALIAS(D_8012E8CA, gKirbyState, 0x10A)
ALIAS(D_8012E904, gKirbyState, 0x14C)    /* unk144 */
ALIAS(D_8012E908, gKirbyState, 0x150)    /* unk148 */
ALIAS(D_8012E90C, gKirbyState, 0x154)    /* unk14C */
ALIAS(D_8012E922, gKirbyState, 0x16A)    /* unk162 */
ALIAS(D_8012E944, gKirbyState, 0x18C)    /* _184 */
ALIAS(gPositionState, gKirbyState, 0x1B0) /* unk1A8: struct PositionState */
ALIAS(D_8012E9B8, gKirbyState, 0x200)    /* unk1F8 */
