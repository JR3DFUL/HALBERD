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
 *   KIRBY_PC_INPUT=12.0:START,14.5:A:20,18:DDOWN+A
 *       An explicit timeline. Each entry is
 *
 *           <seconds>:<buttons>[:<frames>]
 *
 *       <seconds> is elapsed game time; <buttons> is one or more of
 *       A B Z START L R DUP DDOWN DLEFT DRIGHT CUP CDOWN CLEFT CRIGHT joined
 *       with '+', or NONE; <frames> is how long to hold it, in 60ths, and
 *       defaults to PRESS_FRAMES below. Entries may be listed in any order.
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

struct Cue {
    u64 at;        /* count-register ticks from the first poll */
    u64 until;
    u16 button;
};

static struct Cue sCues[MAX_CUES];
static int sNumCues;
static int sMode;            /* 0 off, 1 scripted, 2 autostart */
static int sReady;
static u64 sEpoch;
static int sVerbose;

static u16 button_of(const char *name, size_t n) {
    static const struct { const char *name; u16 bit; } kNames[] = {
        { "A", CONT_A },        { "B", CONT_B },
        { "Z", CONT_G },        { "START", CONT_START },
        { "L", CONT_L },        { "R", CONT_R },
        { "DUP", CONT_UP },     { "DDOWN", CONT_DOWN },
        { "DLEFT", CONT_LEFT }, { "DRIGHT", CONT_RIGHT },
        { "CUP", CONT_E },      { "CDOWN", CONT_D },
        { "CLEFT", CONT_C },    { "CRIGHT", CONT_F },
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

static u16 buttons_of(const char *s, size_t n) {
    u16 mask = 0;
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

/* One `<seconds>:<buttons>[:<frames>]` entry. */
static void parse_cue(const char *s, size_t n) {
    const char *colon1 = NULL;
    const char *colon2 = NULL;
    size_t i;
    double secs;
    long frames = PRESS_FRAMES;
    u16 mask;

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
    sNumCues++;
}

void pc_input_script_init(void) {
    const char *spec;
    size_t start;
    size_t i, n;

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

    n = strlen(spec);
    start = 0;
    for (i = 0; i <= n; i++) {
        if (i == n || spec[i] == ',') {
            if (i > start) {
                parse_cue(spec + start, i - start);
            }
            start = i + 1;
        }
    }
    if (sNumCues == 0) {
        fprintf(stderr, "[input] KIRBY_PC_INPUT set but no usable cues\n");
        return;
    }
    sMode = 1;
    fprintf(stderr, "[input] %d scripted cue(s)\n", sNumCues);
}

/* Called from os_cont.c's snapshot(), after the backend has filled the pads.
 *
 * PORT 0 ONLY, and it always reports the pad as PRESENT. A script is
 * meaningless on a port the game is not reading, and the headless backend
 * reports no pad at all -- which is right for a normal run and would make
 * every cue invisible here. */
void pc_input_script_apply(PCPad *pads, int n) {
    u64 now;
    u16 mask = 0;
    int i;
    static u16 sLast;

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

    if (sMode == 2) {
        /* One second on the game's clock per slot; press for the first
         * PRESS_FRAMES of it and alternate which button. */
        u64 slot = now / PC_COUNTER_HZ;
        u64 into = now - slot * PC_COUNTER_HZ;

        if (into < (u64)PRESS_FRAMES * (PC_COUNTER_HZ / 60u)) {
            mask = (slot & 1) ? CONT_A : CONT_START;
        }
    } else {
        for (i = 0; i < sNumCues; i++) {
            if (now >= sCues[i].at && now < sCues[i].until) {
                mask |= sCues[i].button;
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
        fprintf(stderr, "[input] %+7.2fs  buttons %04x\n",
                (double)now / (double)PC_COUNTER_HZ, (unsigned)mask);
        fflush(stderr);
    }
    sLast = mask;

    pads[0].button = mask;
    pads[0].present = 1;
}
