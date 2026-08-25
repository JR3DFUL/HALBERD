/* A controller nobody has to hold.
 *
 * WHY THIS EXISTS. The port boots, plays all three attract demos and wraps
 * back round the attract loop -- and that is the ceiling of an unattended
 * run, because game_tick() only leaves the loop when someone presses START.
 * Everything past it (the file select menu, the world map, an actual level)
 * is untested and unreachable from a script, so the port's own smoke test
 * could not go there and neither could anyone chasing a crash beyond it.
 *
 * So the platform layer grows a synthetic pad. It is a DIAGNOSTIC, not an
 * input method: the real backends already deliver real input, and this only
 * overrides them when KIRBY_PC_INPUT is set.
 *
 * TIME IS GAME TIME, NOT WALL TIME, and that is the one design decision here
 * that matters. Script times are read against pc_count64(), the same scaled
 * count register the game itself sees, so a script written once behaves
 * identically at KIRBY_PC_TIMESCALE=1 and =8. Against wall time an
 * accelerated run would race past every cue.
 *
 * THE FORMAT
 *
 *   KIRBY_PC_INPUT=autostart
 *       Pulse START, then A, once a second forever. Blunt, and exactly what a
 *       smoke test wants: it punches through the title screen, the file
 *       select and any confirm prompt without knowing what any of them are.
 *
 *   KIRBY_PC_INPUT=advance
 *       Drive by WHAT IS ON SCREEN rather than by the clock: the button
 *       pressed is chosen from gGameState, and the cycle restarts whenever
 *       gGameState changes. This exists because `autostart` cannot get
 *       in-game, and the reason is not a port bug.
 *
 *       src/ovl1/game.c's func_800A3408 runs the world 1-1 opening cutscene
 *       as
 *
 *           if (func_80227308_ovl18(0) != 0)
 *               do { gGameState = 0xE; func_800A3150(4); }
 *               while (func_80227308_ovl18(1) == 1);
 *
 *       and func_80227308_ovl18 is the WATCH-THE-CUTSCENE PROMPT, returning
 *       the YES/NO the player picked. Its object (func_80226FD8_ovl18)
 *       defaults the answer to YES, moves it to NO on D-LEFT (buttonPressed
 *       & 0x200) and back to YES on D-RIGHT, and CONFIRMS on A or START.
 *       `autostart` presses only START and A, so it confirms YES at every
 *       re-prompt and the cutscene replays for ever. That is the loop
 *       previously recorded as "does not reach gGameState 15, whether that
 *       is an overlay-18 bug is the open question" -- it is not a bug, it is
 *       a prompt nobody was answering.
 *
 *       The table is in button_for_state() below. The important entry is
 *       states 11 and 14, where the prompt lives: a two-second cycle presses
 *       D-LEFT, then A one second later, so every confirm is preceded by a
 *       "no" within the same prompt.
 *
 *   KIRBY_PC_INPUT=12.0:START,14.5:A:20,18:DDOWN+A
 *       An explicit timeline. Each entry is
 *
 *           [g]<seconds>:<buttons>[:<frames>]
 *
 *       <seconds> is elapsed game time; <buttons> is one or more of
 *       A B Z START L R DUP DDOWN DLEFT DRIGHT CUP CDOWN CLEFT CRIGHT joined
 *       with '+', or NONE; <frames> is how long to hold it, in 60ths, and
 *       defaults to PRESS_FRAMES below. Entries may be listed in any order.
 *
 *       Four more names -- SR SL SU SD -- push the ANALOG STICK right, left,
 *       up or down instead of pressing a button. They combine with buttons:
 *       `DRIGHT+SR` is "walk right on both inputs".
 *
 *       WRITE DRIGHT, NOT SR, FOR MOVEMENT. This file used to say the
 *       opposite -- that the D-pad only drives menus and the player is read
 *       from the stick -- and that is not true of this build: measured with
 *       KIRBY_PC_PLAYERPOS, a held stick leaves the player at his spawn X
 *       for the whole run at any deflection, and a held D-pad walks him.
 *       The full measurement is in the stick note at the bottom of this
 *       file.
 *
 *       A LEADING `g` makes the time relative to the moment gameplay starts
 *       (the first tick with gGameState == 15) rather than to the first
 *       poll, and it is the only form worth writing for anything in a level.
 *       Absolute times cannot address gameplay: reaching it means answering
 *       the world 1-1 cutscene prompt, and how many times that prompt has to
 *       be answered -- at about 90 seconds of cutscene per turn -- varies
 *       run to run, so the wall second at which a level starts is not a
 *       constant. Measured over the two runs that produced this note it
 *       moved by more than a minute. A `g`-relative cue lands in the same
 *       place in the level every time.
 *
 *       Listing ANY `g` cue also switches the menus onto `advance`'s
 *       state-driven table automatically, so a gameplay script does not have
 *       to restate how to get through the title screen and the prompt.
 *
 *   KIRBY_PC_INPUT=play
 *       A built-in `g` timeline (kPlayProgram below) that walks, jumps,
 *       inhales, swallows, opens and closes the pause menu and keeps
 *       walking. It exists so a crash hunt has one reproducible route
 *       through a level's object types rather than a fresh ad-hoc string
 *       each time; edit the program, do not paste a variant.
 *
 * WHY A PULSE AND NOT A LEVEL. Menus latch on the RISING edge of a button, so
 * a held START is one press however long it is held, and a START that is
 * never released can wedge a menu that waits for the release. Every cue is
 * therefore a bounded pulse with gaps between, and `autostart` alternates
 * START and A so a screen that only answers to one of them still advances.
 */
#include <ultra64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc/pc_platform.h"
#include "pc/pc_backend.h"

#define PRESS_FRAMES 8       /* ~133 ms at 60 Hz: past any debounce, well
                              * short of anything that reads it as a hold */
#define MAX_CUES     64

/* Stick pushes ride in the same word as the button mask, above the CONT_
 * bits, so one parse and one table cover both. Split out again in apply().
 * 0x50 is the magnitude, matching mode 4's held walk. */
#define STICK_R      0x00010000u
#define STICK_L      0x00020000u
#define STICK_U      0x00040000u
#define STICK_D      0x00080000u
#define STICK_MASK   0x000F0000u
#define STICK_PUSH   0x50

struct Cue {
    u64 at;        /* count-register ticks from the cue's epoch */
    u64 until;
    u32 button;    /* CONT_ bits, plus the STICK_ bits above */
    int rel;       /* 1: `at` counts from the start of gameplay, not the run */
};

static struct Cue sCues[MAX_CUES];
static int sNumCues;
static int sMode;            /* 0 off, 1 scripted, 2 autostart, 3 advance,
                              * 4 advance + hold RIGHT once in gameplay */
static int sMenuAuto;        /* mode 1 only: drive menus from gGameState, as
                              * `advance` does, and leave gameplay to the
                              * cues. Set by any `g`-relative cue. */
static int sReady;
static u64 sEpoch;
static int sVerbose;

/* Mode 3 only. The game's own state variable, and the epoch of the slot cycle
 * -- restarted whenever the state changes, so a fresh screen always sees the
 * first button of its cycle rather than whatever phase the clock happened to
 * be in. */
extern u32 gGameState;
static u32 sLastState = 0xFFFFFFFFu;
static u64 sStateEpoch;

/* The tick gameplay began, in the same timeline as `now`, or 0 if it has not
 * begun yet. A `g` cue is inert until this is set, so a script cannot fire
 * into the menus by accident. */
static u64 sPlayEpoch;

/* KIRBY_PC_INPUT=play. Times are game-seconds after gameplay starts.
 *
 * The route is deliberately made of DIFFERENT ACTIONS rather than a longer
 * walk: the class of bug this is built to find is a display list assembled
 * from a mis-read data slot, and every distinct action is a different
 * object type being drawn for the first time. Walking further through
 * scenery that already renders adds none.
 *
 * Held stick pushes are long (10 s) and the button presses inside them are
 * short, so the player keeps moving across a jump or an inhale instead of
 * stopping dead at every cue. */
static const char kPlayProgram[] =
    /* DRIGHT, not SR: the D-pad is what moves the player, and nothing else
     * ever will -- the stick has no reader anywhere in the ROM. See the
     * stick note at the bottom of this file for the measurement. SR is still
     * pressed alongside it, because it costs nothing and a run that sets
     * both is the one that would notice if that ever changed. */
    "g0:DRIGHT+SR:60000,"  /* walk right for the whole run; the cues below
                            * OR into this one rather than replacing it */
    /* JUMP ON A SHORT CYCLE, and this is the fix for the wedge this program
     * carried. Kirby 64's world 1-1 has a solid ledge whose collision plane
     * is x = -1480 (normal (-1,0,0), collisionType 0 -- read out of the
     * level data with a debugger, see docs/PC_PORT_LIBULTRASHIP.md). A
     * walking Kirby stops dead against it, which two lanes read as the level
     * ending or the engine clamping. It is neither: he has to jump, and the
     * old program's jumps at g8/g24/g52 simply never coincided with standing
     * at the ledge. A 2-second cycle always does. Measured with the same
     * cycle bolted onto a plain DRIGHT script: -2096.47 -> -1480.00 ->
     * -1224.80 -> -947.75, i.e. straight over. */
    "g2:A:12,g4:A:12,g6:A:12,g8:A:12,"
    "g10:A:12,g12:A:12,g14:A:12,g16:A:12,g18:A:12,"
    "g20:A:12,g22:A:12,g24:A:12,g26:A:12,g28:A:12,"
    "g30:A:12,g32:A:12,g34:A:12,g36:A:12,g38:A:12,"
    "g40:A:12,g42:A:12,g44:A:12,g46:A:12,g48:A:12,"
    /* AND THE OTHER ACTIONS COME AFTER THE WALKING, which is the second half
     * of the fix. With B at g14 and DDOWN at g19 -- where they used to be --
     * Kirby arrives at the ledge in action 14 with vel = 0.0000, and a held
     * D-RIGHT does not restart him: measured at +87, +96, +104, +112, +120
     * and +128 s, all six samples identical at x = -1480.00 t = 0.517857
     * vel = 0.0000 action = 14 with held = 0100 the whole time. Jumping
     * cannot help from there because he never walks into the ledge again.
     * Walk first, act later.
     *
     * The tail keeps driving to the end of the run on purpose: an earlier
     * program ran out of cues at g129 and left the pad neutral for two
     * thirds of the run, which showed up as distinct=123 of 474 sampled
     * frames against a walking run's 450 of 452. A script that stops driving
     * stops measuring. */
    "g52:B:120,"          /* inhale: the suction effect and what it catches */
    "g57:DDOWN+SD:60,"    /* swallow / crouch */
    "g62:START:8,"        /* pause menu on top of the level */
    "g68:START:8,"        /* and back out of it */
    "g74:A:12,g76:A:12,g78:A:12,"
    "g80:B:120,"
    "g86:A:12,g88:A:12,g90:A:12,g92:A:12,g94:A:12,"
    "g96:A:12,g98:A:12";

/* WHERE THIS PROGRAM NOW ENDS, so the next lane does not re-find it. It
 * clears the ledge -- the last probe line before the crash reads
 *
 *   8E6C.obj0 #423  node=3 left=0.520089 old=0.520089 -> 0.522321
 *                   vel=5.0000 len=224.000 x=-1475.00
 *
 * with no *UNDONE*, i.e. past t = 0.517857 and still accelerating -- and then
 * SIGSEGVs a few frames later. TWO different faults have been seen just past
 * the ledge, on two different routes through it, so this is a region of the
 * level the port has not run before rather than one bug:
 *
 *   func_8010E5B0 (ovl2_8.c:137) <- func_8010E740 <- func_8010FC30 <-
 *   func_80110FD4 (ovl2_9.c:1131) <- func_8019F650_ovl7 (ovl7_2.c:207)
 *       -- this program, at x = -1475.00
 *
 *   utilFuncTableJump (util.c:151) <- func_800FCFF0 (spawn.c:201)
 *       -- a plain DRIGHT + 2 s A cycle, at x = -947.75. That one is
 *       diagnosed: the spawn callback table D_801244A4 is a run of RAW
 *       cross-overlay addresses in the decomp's own listing rather than
 *       symbol references, so it is emitted as u32[] and called at the wrong
 *       stride. Decomp-side work first; see docs/PC_PORT_LIBULTRASHIP.md.
 *
 * The previous note here recorded `play` wedging at x = -2198.29 and asked
 * for the A jumps, the B inhale and the DDOWN swallow to be bisected. That
 * bisect is moot. Re-measured on the current tree, the OLD program stopped at
 * x = -1480.00 with node=3 t=0.517857 vel=5.0000 -- the same ledge a plain
 * held DRIGHT stops at, not a state its own cues had put the player in. The
 * -2198.29 figure does not reproduce here; it was measured on a differently
 * staged tree and nothing in this repository can now say what that tree was,
 * which is its own lesson (see the note at the top of build.sh). */

static u32 button_of(const char *name, size_t n) {
    static const struct { const char *name; u32 bit; } kNames[] = {
        { "A", CONT_A },        { "B", CONT_B },
        { "Z", CONT_G },        { "START", CONT_START },
        { "L", CONT_L },        { "R", CONT_R },
        { "DUP", CONT_UP },     { "DDOWN", CONT_DOWN },
        { "DLEFT", CONT_LEFT }, { "DRIGHT", CONT_RIGHT },
        { "CUP", CONT_E },      { "CDOWN", CONT_D },
        { "CLEFT", CONT_C },    { "CRIGHT", CONT_F },
        { "SR", STICK_R },      { "SL", STICK_L },
        { "SU", STICK_U },      { "SD", STICK_D },
        { "NONE", 0 },
    };
    int i;

    for (i = 0; i < (int)(sizeof(kNames) / sizeof(kNames[0])); i++) {
        if (strlen(kNames[i].name) == n
            && strncmp(kNames[i].name, name, n) == 0) {
            return kNames[i].bit;
        }
    }
    fprintf(stderr, "[input] unknown button '%.*s' -- ignored\n", (int)n, name);
    return 0;
}

static u32 buttons_of(const char *s, size_t n) {
    u32 mask = 0;
    size_t start = 0;
    size_t i;

    for (i = 0; i <= n; i++) {
        if (i == n || s[i] == '+') {
            if (i > start) {
                mask |= button_of(s + start, i - start);
            }
            start = i + 1;
        }
    }
    return mask;
}

/* One `[g]<seconds>:<buttons>[:<frames>]` entry. */
static void parse_cue(const char *s, size_t n) {
    const char *colon1 = NULL;
    const char *colon2 = NULL;
    size_t i;
    double secs;
    long frames = PRESS_FRAMES;
    u32 mask;
    int rel = 0;

    if (n > 0 && (s[0] == 'g' || s[0] == 'G')) {
        rel = 1;
        s++;
        n--;
    }
    for (i = 0; i < n; i++) {
        if (s[i] == ':') {
            if (colon1 == NULL) {
                colon1 = s + i;
            } else if (colon2 == NULL) {
                colon2 = s + i;
            }
        }
    }
    if (colon1 == NULL) {
        fprintf(stderr, "[input] cue '%.*s' has no ':' -- ignored\n",
                (int)n, s);
        return;
    }
    secs = strtod(s, NULL);
    if (colon2 != NULL) {
        frames = strtol(colon2 + 1, NULL, 10);
        if (frames < 1) {
            frames = 1;
        }
        mask = buttons_of(colon1 + 1, (size_t)(colon2 - colon1 - 1));
    } else {
        mask = buttons_of(colon1 + 1, (size_t)(s + n - colon1 - 1));
    }
    if (sNumCues >= MAX_CUES) {
        fprintf(stderr, "[input] more than %d cues -- '%.*s' dropped\n",
                MAX_CUES, (int)n, s);
        return;
    }
    sCues[sNumCues].at = (u64)(secs * (double)PC_COUNTER_HZ);
    sCues[sNumCues].until =
        sCues[sNumCues].at + (u64)frames * (PC_COUNTER_HZ / 60u);
    sCues[sNumCues].button = mask;
    sCues[sNumCues].rel = rel;
    if (rel) {
        sMenuAuto = 1;
    }
    sNumCues++;
}

/* Split one comma-separated spec into cues. Shared by KIRBY_PC_INPUT and by
 * `play`'s built-in program, so both go through exactly one parser. */
static void parse_spec(const char *spec) {
    size_t n = strlen(spec);
    size_t start = 0;
    size_t i;

    for (i = 0; i <= n; i++) {
        if (i == n || spec[i] == ',') {
            if (i > start) {
                parse_cue(spec + start, i - start);
            }
            start = i + 1;
        }
    }
}

/* Mode 3's whole table. `slot` counts game-seconds since gGameState last
 * changed, so slot 0 is always the first press a screen sees.
 *
 * The states are src/ovl1/game.c's game_tick() switch, and the mapping from
 * state to milestone name is the same one src/pc/pc_progress.c uses --
 * anything changed here should be changed there too. */
static u16 button_for_state(u32 gs, u64 slot) {
    switch (gs) {
    case 10: /* file select: A picks the highlighted file, and nothing else
              * here should be touched -- a stray D-pad press moves the
              * selection onto a different save. */
    case 12: /* the planet map. A enters the level under the cursor; the
              * D-pad would walk the cursor off it. */
        return CONT_A;

    case 11: /* THE GALAXY MAP -- AND THE CUTSCENE PROMPT, which
              * func_800A3408 runs with gGameState still 11. */
    case 14: /* the cutscene do-while, and the re-prompt at the bottom of it.
              *
              * D-LEFT selects NO, A confirms. Pressing them in that order is
              * what stops the world 1-1 opening cutscene replaying for ever
              * -- see the header. On the galaxy map proper the D-LEFT is
              * harmless: with one planet unlocked the cursor has nowhere to
              * go, and the A that follows selects it.
              *
              * SEVEN D-LEFTS PER A, not one each, and the ratio is measured
              * rather than chosen. The prompt is not up for most of this
              * state: one turn of the do-while plays the whole world 1-1
              * opening, which takes about 90 SECONDS of wall time (timed
              * with a breakpoint on the gtlCreateScene at
              * ovl18/code_239080.c:339 -- 90.0 s between the state being
              * entered and the prompt being built). Whatever phase the
              * alternation is in when the prompt finally appears is
              * therefore fixed for that whole turn, and a one-for-one
              * alternation lands A-before-D-LEFT half the time -- at 90
              * seconds a retry, that is the difference between a test that
              * finishes and one that does not. Making A the rare button
              * costs at most eight seconds of waiting at a prompt and makes
              * the "no" essentially certain to be in place first. */
        return ((slot & 7) == 7) ? CONT_A : CONT_LEFT;

    case 15: /* IN-GAME. Stop driving. Everything past this point is the
              * thing being measured, and a synthetic START here opens the
              * pause menu on top of it. */
        return 0;

    default: /* logos, opening movie, the title screen and the attract demos
              * -- START skips, A answers anything that wants A. */
        return (slot & 1) ? CONT_A : CONT_START;
    }
}

void pc_input_script_init(void) {
    const char *spec;

    if (sReady) {
        return;
    }
    sReady = 1;
    sVerbose = getenv("KIRBY_PC_PROGRESS") != NULL;

    spec = getenv("KIRBY_PC_INPUT");
    if (spec == NULL || spec[0] == '\0') {
        return;
    }
    if (strcmp(spec, "autostart") == 0) {
        sMode = 2;
        fprintf(stderr, "[input] autostart: pulsing START/A once a second\n");
        return;
    }
    if (strcmp(spec, "advance") == 0) {
        sMode = 3;
        fprintf(stderr, "[input] advance: driving from gGameState\n");
        return;
    }
    /* `advance` stops at the moment gameplay starts, on purpose: a synthetic
     * button in state 15 opens the pause menu on top of the thing being
     * measured. `walk` is the same script with one addition -- once in state
     * 15 it holds RIGHT -- and it exists because a screenshot of a standing
     * start cannot distinguish "the level renders and the player is
     * off-camera" from "the level renders and there is no player". Walking
     * moves the camera; if the view follows, there is a player object being
     * simulated and followed. */
    if (strcmp(spec, "walk") == 0) {
        sMode = 4;
        fprintf(stderr, "[input] walk: advance, then hold RIGHT in gameplay\n");
        return;
    }
    /* `play` is the built-in program and nothing else: it goes through the
     * same parser as a hand-written spec, so there is one code path to be
     * wrong in. */
    if (strcmp(spec, "play") == 0) {
        spec = kPlayProgram;
    }

    parse_spec(spec);
    if (sNumCues == 0) {
        fprintf(stderr, "[input] KIRBY_PC_INPUT set but no usable cues\n");
        return;
    }
    sMode = 1;
    fprintf(stderr, "[input] %d scripted cue(s)%s\n", sNumCues,
            sMenuAuto ? ", menus driven from gGameState" : "");
}

/* Called from os_cont.c's snapshot(), after the backend has filled the pads.
 *
 * PORT 0 ONLY, and it always reports the pad as PRESENT. A script is
 * meaningless on a port the game is not reading, and the headless backend
 * reports no pad at all -- which is right for a normal run and would make
 * every cue invisible here. */
void pc_input_script_apply(PCPad *pads, int n) {
    u64 now;
    u32 mask = 0;
    int i;
    static u32 sLast;

    if (!sReady) {
        pc_input_script_init();
    }
    if (sMode == 0 || n < 1 || pads == NULL) {
        return;
    }
    if (sEpoch == 0) {
        sEpoch = pc_count64();
        if (sEpoch == 0) {
            sEpoch = 1;
        }
    }
    now = pc_count64() - sEpoch;

    /* Latched once and never moved. gGameState leaves 15 for the pause menu
     * and comes back, and re-zeroing the epoch on the way back would restart
     * a script that is halfway through a level. */
    if (sPlayEpoch == 0 && gGameState == 15) {
        sPlayEpoch = now ? now : 1;
        if (sVerbose) {
            fprintf(stderr, "[input] gameplay starts at %+.2fs; `g` cues now "
                    "count from here\n", (double)now / (double)PC_COUNTER_HZ);
            fflush(stderr);
        }
    }

    if (sMode == 2) {
        /* One second on the game's clock per slot; press for the first
         * PRESS_FRAMES of it and alternate which button. */
        u64 slot = now / PC_COUNTER_HZ;
        u64 into = now - slot * PC_COUNTER_HZ;

        if (into < (u64)PRESS_FRAMES * (PC_COUNTER_HZ / 60u)) {
            mask = (slot & 1) ? CONT_A : CONT_START;
        }
    } else if (sMode == 3 || sMode == 4) {
        u32 gs = gGameState;
        u64 since;
        u64 slot;
        u64 into;

        if (gs != sLastState) {
            sLastState = gs;
            sStateEpoch = now;
        }
        since = now - sStateEpoch;
        slot = since / PC_COUNTER_HZ;
        into = since - slot * PC_COUNTER_HZ;
        if (sMode == 4 && gs == 15) {
            /* HELD, not pulsed. A one-frame D-pad tap is a step; the
             * question here is whether the camera follows sustained
             * movement. */
            mask = CONT_RIGHT;
        } else if (into < (u64)PRESS_FRAMES * (PC_COUNTER_HZ / 60u)) {
            mask = button_for_state(gs, slot);
        }
    } else {
        for (i = 0; i < sNumCues; i++) {
            u64 t;

            if (sCues[i].rel) {
                if (sPlayEpoch == 0) {
                    continue;    /* gameplay has not started; cue is inert */
                }
                t = now - sPlayEpoch;
            } else {
                t = now;
            }
            if (t >= sCues[i].at && t < sCues[i].until) {
                mask |= sCues[i].button;
            }
        }
        /* Cues own gameplay; the state table owns everything before it. A
         * script that lists a `g` cue would otherwise have to spell out the
         * title screen and the cutscene prompt for itself. */
        if (sMenuAuto && gGameState != 15) {
            u64 since;
            u64 slot;
            u64 into;

            if (gGameState != sLastState) {
                sLastState = gGameState;
                sStateEpoch = now;
            }
            since = now - sStateEpoch;
            slot = since / PC_COUNTER_HZ;
            into = since - slot * PC_COUNTER_HZ;
            if (into < (u64)PRESS_FRAMES * (PC_COUNTER_HZ / 60u)) {
                mask |= button_for_state(gGameState, slot);
            }
        }
    }

    /* THIS EPOCH IS A CANARY, and it has already earned its keep once.
     *
     * sEpoch sits in the platform layer's .bss, a few hundred bytes past the
     * end of the game's own bss objects, and a game-side buffer overrun lands
     * on it before it lands on anything that complains. That is exactly how
     * the HUD arena overrun in src/pc/pc_bss_whole.c was found: this timer
     * started reporting 390317930 seconds because its epoch had been
     * overwritten with a repeating 16-bit fill pattern. Nothing else in the
     * process had noticed.
     *
     * A run cannot plausibly last a year, so say so rather than printing an
     * absurd number and hoping somebody looks twice. Once only -- if the bss
     * is being scribbled on, one line is a diagnosis and a thousand is
     * noise. */
    if (now > (u64)PC_COUNTER_HZ * 60u * 60u * 24u) {
        static int said;

        if (!said) {
            said = 1;
            fprintf(stderr,
                    "[input] EPOCH CORRUPTED (now=%llx epoch=%llx). This "
                    "static lives in the platform layer's .bss; something has "
                    "written past the end of a game bss object into it.\n",
                    (unsigned long long)now, (unsigned long long)sEpoch);
            fflush(stderr);
        }
        return;
    }

    if (sVerbose && mask != sLast) {
        fprintf(stderr, "[input] %+7.2fs  buttons %04x  stick %c%c  "
                "gGameState=%u\n",
                (double)now / (double)PC_COUNTER_HZ,
                (unsigned)(mask & ~STICK_MASK),
                (mask & STICK_R) ? 'R' : (mask & STICK_L) ? 'L' : '-',
                (mask & STICK_U) ? 'U' : (mask & STICK_D) ? 'D' : '-',
                (unsigned)gGameState);
        fflush(stderr);
    }

    /* WHAT THE GAME LATCHED, which is a different fact from what was pressed
     * and the only one a menu reacts to.
     *
     * Menus read gPlayerControllers[0].buttonPressed -- the RISING EDGE that
     * src/main/contpad.c's read_controller_input computes and
     * contSetPlayerPads publishes for exactly one game tick before clearing
     * it. Everything between this function and there can drop a press: the
     * SI read, the errno check, the channel map, a game tick that does not
     * run. Printing only the button this script asked for therefore proves
     * nothing about whether any screen could have seen it, and an hour went
     * into "the D-pad does not work" before that distinction was drawn.
     *
     * Declared here as u16[] on purpose: that is the view the game's own
     * menu code takes of the same storage (see ovl18/code_239080.c, which
     * reads gPlayerControllers[1] for buttonPressed), and it needs no game
     * struct header in the platform layer. */
    if (sVerbose) {
        extern u16 gPlayerControllers[];
        static u16 sLastLatched;
        u16 latched = gPlayerControllers[1];

        if (latched != sLastLatched) {
            sLastLatched = latched;
            if (latched != 0) {
                fprintf(stderr,
                        "[input] %+7.2fs  game latched buttonPressed %04x\n",
                        (double)now / (double)PC_COUNTER_HZ,
                        (unsigned)latched);
                fflush(stderr);
            }
        }
    }
    sLast = mask;

    pads[0].button = (u16)(mask & ~STICK_MASK);
    pads[0].present = 1;
    /* THE STICK, NOT THE D-PAD, IS WHAT MOVES KIRBY. The D-pad in Kirby 64
     * drives menus; in gameplay the player is read from the analog stick, so
     * a walk script that only sets buttons produces a Kirby standing
     * perfectly still with the mask visibly latched. 0x50 is a firm push
     * without being the extreme the real hardware rarely reaches. */
    /* AND THE STICK DOES NOT ACTUALLY MOVE KIRBY IN THIS BUILD. The note
     * above is what the port believed, and `walk` appeared to confirm it --
     * but `walk` also sets CONT_RIGHT in its mask, and CONT_RIGHT is what
     * was doing the work. Measured with KIRBY_PC_PLAYERPOS (pc_progress.c),
     * three runs, world 1-1, stick held for 150 wall seconds:
     *
     *   KIRBY_PC_INPUT=g0:DRIGHT:60000   D-pad only, no stick
     *       x walks -2946.79 -> -1480.00
     *   KIRBY_PC_INPUT=g0:SR:60000       stick only, 0x50
     *       x stays -2959.92, the spawn point, for the whole run
     *   the same at full deflection 0x7F
     *       x stays -2959.92
     *
     * So it is not a deadzone. That note ended "the gap is inside the player
     * code that should read stickX". THERE IS NO SUCH CODE, and the search
     * for it is over.
     *
     * The stick arrives perfectly: probes in src/ovl1/util.c counted
     * utilSetPlayerContPad 8274 times over a 340 s stick-held run and
     * gPlayerControllers[0].stickX was non-zero on all 8274, with the
     * [playerpos] line reading stick=80 -- that field IS
     * gKirbyController.stickX. utilCorrectStickX, utilCorrectStickY and
     * utilGetStickDirection, the only three functions in the game that read
     * a raw stick axis, were entered ZERO times in the same run.
     *
     * A scan of the whole 32 MB ROM image agrees and finishes the argument:
     * gKirbyController.stickX has two `sb` writes (both in
     * utilSetPlayerContPad) and no reads at all, where
     * gKirbyController.buttonHeld has 154 `lhu` reads; and
     * utilGetStickDirection -- the sole caller of utilCorrectStickX/Y -- is
     * itself called zero times, by jal, by la, or through a data table. It
     * is dead code in the retail ROM.
     *
     * So the stick is copied three times and read by nothing, and movement
     * is gKirbyController.buttonHeld & 0x300 in ovl2/plylib.c. DRIVE
     * GAMEPLAY WITH THE D-PAD. The stick is still set here because it costs
     * nothing, but nothing will ever come of it without a deliberate port
     * feature -- synthesising the D-pad bits from stick_x in os_cont.c --
     * which would be a change to the game's behaviour and has to be labelled
     * as one. Full details in docs/PC_PORT_LIBULTRASHIP.md. */
    if (sMode == 4 && gGameState == 15) {
        pads[0].stick_x = (s8)STICK_PUSH;
        pads[0].stick_y = 0;
    } else {
        pads[0].stick_x = (s8)(((mask & STICK_R) ? STICK_PUSH : 0)
                               - ((mask & STICK_L) ? STICK_PUSH : 0));
        pads[0].stick_y = (s8)(((mask & STICK_U) ? STICK_PUSH : 0)
                               - ((mask & STICK_D) ? STICK_PUSH : 0));
    }
}
