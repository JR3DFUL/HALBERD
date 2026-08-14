/* RETIRED. This file used to carry a "player spawn lite" stand-in for the
 * ovl3 player init (func_8016BF60_ovl3) so the camera had a seated track
 * object to aim at while ovl3 was unported. The real chain is now ported:
 *
 *   func_800F64B0 (ovl2.c)         scene postInit -> func_800FF2C8 (ovl2_5.c)
 *   func_8011C720 (plylib.c)       -> func_8016BF60_ovl3 (src/ovl3/kirby.c)
 *     -> func_8011C8F8 plyInit     seats Kirby, registers func_800B531C
 *        (ovl1_8.c) as the motion callback and func_800A9864 attaches the
 *        ability model; camera follow runs through the ovl2_3.c PORT arms
 *        (func_800FBA98 family).
 *
 * Nothing is defined here anymore; the translation unit stays so the build
 * globs don't churn. */
typedef int pc_player_lite_retired;
