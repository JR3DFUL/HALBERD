/* Two libnaudio routines the game has under func_ names, reached here by
 * their upstream names.
 *
 * WHAT IS ACTUALLY WRONG. src/main/libn_audio.c's n_alSynNew is a decompiled
 * draft, and it writes the two lines
 *
 *     n_syn->auxBus->fx = n_alSynAllocFX(0, c, hp);
 *     n_syn->mainBus->filter.handler = (N_ALCmdHandler) n_alFxPull;
 *
 * using the names the routines have in Nintendo's libnaudio source. Kirby's
 * copies of those routines are in the ROM under the tree's own names, and the
 * listing the draft was written from says which:
 *
 *     asm/nonmatchings/main/libn_audio/n_alSynNew.s:85
 *         jal   func_8002D120                    <- n_alSynAllocFX
 *     asm/nonmatchings/main/libn_audio/n_alSynNew.s:88
 *         lui   $t7, %hi(func_80028318)          <- n_alFxPull
 *
 * Both are already compiled into this build -- func_80028318 in libn_audio.c
 * itself, func_8002D120 in libn_audio_2f.c -- so the two upstream names are
 * the ONLY undefined symbols the whole audio library still has, and they are
 * undefined only because the draft spelled them differently.
 *
 * THE REAL FIX IS ONE CHARACTER-FOR-CHARACTER EDIT IN src/main/libn_audio.c,
 * which this lane does not own; it is reported rather than made. These
 * forwarders let the native link close in the meantime, and they are not
 * stubs: each one calls the game's own code and returns what it returns. When
 * the draft is corrected this file stops being referenced and can go.
 *
 * WHY NOT REUSE libreultra INSTEAD, which has both routines. n_synallocfx.c
 * calls n_alFxNew, whose only definition is in n_drvrNew.c -- which also
 * defines alN_PVoiceNew, which the decompilation already has. That is the
 * half-superseded-translation-unit problem the Makefile.pc comment describes,
 * and it costs a "multiple definition" the moment the object is linked.
 * n_reverb.c is linkable but wants _doModFunc and init_lpfilter from the OLD
 * (non-n) audio library, which would drag in fifteen more objects of code
 * this game never calls. Calling the game's own routine is both smaller and
 * more accurate.
 *
 * Prototypes are written with plain scalar and void* parameters rather than
 * the libaudio typedefs so this file needs no audio header: every one of
 * those types is a pointer or an s32/s16 and the ABI is identical. The
 * declarations still have to AGREE with the definitions, so they are copied
 * from src/main/libn_audio.c:204-205 and :424 with the typedefs expanded.
 */
#include <PR/ultratypes.h>

/* Acmd *func_80028318(s32 sampleOffset, Acmd *p);  -- libn_audio.c:424 */
extern void *func_80028318(s32 sampleOffset, void *p);

/* void *func_8002D120(s16 bus, ALSynConfig *c, ALHeap *hp); -- libn_audio_2f.c */
extern void *func_8002D120(s16 bus, void *c, void *hp);

void *n_alFxPull(s32 sampleOffset, void *p) {
    return func_80028318(sampleOffset, p);
}

void *n_alSynAllocFX(s16 bus, void *c, void *hp) {
    return func_8002D120(bus, c, hp);
}
