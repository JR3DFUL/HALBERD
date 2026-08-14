/* The two camera slot arrays that splat's listing splits at interior labels.
 *
 * On N64 these are contiguous 4-byte pointer arrays in ovl1's bss:
 *
 *     D_800D79B0[10]   camera GObj per scene camera kind (kinds 10..24 by 2)
 *     D_800D79D8[10]   the matching Camera payloads
 *
 * splat emitted a dlabel wherever other code referenced an interior element,
 * so tools/pc/gen_data.py receives them as seven separate blocks:
 *
 *     D_800D79B0 +0    D_800D79B4 +4    D_800D79B8 +8    D_800D79BC +12..39
 *     D_800D79D8 +0    D_800D79DC +4    D_800D79E0 +8..39
 *
 * The generator's 2x bss doubling keeps each PIECE large enough, but the game
 * uses BOTH views of the same memory: src/ovl1/ovl1_2.c indexes the base with
 * native 8-byte slots (`D_800D79B0[idx] = obj`), and reads interior labels as
 * scalars (`D_800D79BC` is N64 index 3 -- func_800A7394, func_800A71E0,
 * func_800A72AC, func_800A7348). No doubling of split pieces lines those up:
 * index 3 lands 24 bytes past D_800D79B0 while the D_800D79BC symbol sat 48
 * bytes past it. The world-map scene's camera callback dereferenced the gap.
 *
 * So the arrays are defined here WHOLE, at native slot width, and every
 * interior label is a linker alias at index*8. gen_data.py's SUPPRESS_BSS
 * list drops the generated storage for all seven symbols in favour of these.
 */
#include <stddef.h>

void *D_800D79B0[10];
void *D_800D79D8[10];

__asm__("   .globl D_800D79B4\n"
        "   .set D_800D79B4, D_800D79B0 + 8\n"
        "   .globl D_800D79B8\n"
        "   .set D_800D79B8, D_800D79B0 + 16\n"
        "   .globl D_800D79BC\n"
        "   .set D_800D79BC, D_800D79B0 + 24\n"
        "   .globl D_800D79DC\n"
        "   .set D_800D79DC, D_800D79D8 + 8\n"
        "   .globl D_800D79E0\n"
        "   .set D_800D79E0, D_800D79D8 + 16\n");

/* ovl4's planet-map track-id table, split by splat at an interior label:
 * N64 has one s32 run -- D_8015C6A8[0] (the exit dot's track id, written by
 * func_80153AEC_ovl4) immediately followed by D_8015C6AC[9] (the level dots,
 * written by func_801533A8_ovl4) -- and func_801523A0_ovl4 indexes the WHOLE
 * run from D_8015C6A8. As two separate doubled bss objects the alias broke
 * and every level-dot lookup read zero. One array, one +4 alias. */
int D_8015C6A8_ovl4[10];
__asm__("   .globl D_8015C6AC_ovl4\n"
        "   .set D_8015C6AC_ovl4, D_8015C6A8_ovl4 + 4\n");
