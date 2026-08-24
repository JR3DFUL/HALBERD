/* How far did the port get?
 *
 * The port boots. That changed the useful question from "does it link" to
 * "where does it stop", and answering that from a pumpdbg counter dump or a
 * gdb session is not something a make target can do. This file turns the boot
 * into a sequence of NAMED MILESTONES, records the furthest one reached, and
 * prints a one-line verdict however the process ends -- normal exit, SIGINT,
 * or a fatal signal. tools/pc/smoke.py parses that line.
 *
 * WHERE THE MILESTONES COME FROM, so they are evidence and not decoration:
 *
 *   * the early ones (main, osInitialize, first thread, first retrace, first
 *     graphics task) are events this directory already generates, so the
 *     platform layer can observe them honestly without touching game code;
 *
 *   * the later ones are gGameState, which src/ovl1/game.c's game_tick()
 *     switches on. That switch IS the boot sequence and the names below are
 *     read straight off it:
 *
 *         1   HAL and Nintendo logos      utilLoadOverlay(2), ovl4(0)
 *         2   opening movie               ovl6 func_80154D60_ovl6(0, 1)
 *         3   title screen                ovl4 func_80151CEC_ovl4(1)
 *         4   attract demo 1              func_800A3150(5)
 *         5   title screen                (between demos)
 *         6   attract demo 2              func_800A3150(6)
 *         7   title screen
 *         8   attract demo 3              func_800A3150(3)
 *         9   title screen, then back to 2
 *         10  file select menu            func_80158048_ovl4
 *         11  world select                func_80159A54_ovl4
 *         12  level select                func_8015531C_ovl4
 *         15  gameplay                    func_800F6AD4(0)
 *
 *     States 10 and up need a controller, so a headless run with no input is
 *     expected to cycle 1 -> 2 -> 3 -> 4 -> 5 ... through the attract loop
 *     forever. "Reached the attract demos" is therefore the honest ceiling of
 *     an unattended run, and anything short of it is a real stop.
 *
 * THE SIGNAL HANDLER IS THE POINT OF THE FILE. A port that crashes 66 seconds
 * into a boot is only debuggable if the crash says where it was, and a
 * backtrace from inside the process costs nothing and needs no gdb. It is
 * deliberately async-signal-safe-ish: backtrace_symbols_fd writes straight to
 * the fd rather than allocating, and the verdict line is assembled with
 * snprintf into a static buffer and written with write(2). Enabled by default
 * because a crash with no location is worse than a slightly unsafe handler;
 * KIRBY_PC_NOCRASHTRAP=1 turns it off if it ever gets in the way of gdb.
 */
#include <ultra64.h>

#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "pc/pc_platform.h"

/* game_tick()'s state variable and the frame counter gtlDraw bumps. Both are
 * plain globals in the game's bss; reading them is the same thing pc_dbg.c
 * already does for its counter dump. */
extern u32 gGameState;
extern s32 gtlDrawnFrameCounter;

/* The furthest milestone reached, and the wall-clock second it happened. */
static int sStage = PC_STAGE_START;
static double sStageAt;
static double sT0;
static int sVerbose;
static int sReported;

/* First occurrence of EVERY milestone, not only of the furthest one.
 *
 * The ordered ceiling on its own hides the two that matter most when a boot
 * stops early. game_tick() sets gGameState = 1 before a single frame is
 * drawn, so "logos" is claimed ahead of the first retrace and the first
 * graphics task -- both of which then score lower than the current ceiling
 * and are never mentioned. Whether the VI ever ticked and whether the game
 * ever submitted a display list are exactly the questions asked of a port
 * that stalled, so they are recorded independently and printed in the
 * verdict. 0.0 means "never happened". */
static double sFirstAt[PC_STAGE_GAMEPLAY + 1];

/* gGameState transitions seen, oldest first, so the verdict can show the route
 * taken rather than only the endpoint. 32 is more than the attract loop needs
 * and the array is written once per transition. */
#define PC_ROUTE_MAX 32
static unsigned sRoute[PC_ROUTE_MAX];
static int sRouteLen;
static u32 sLastGameState = 0xFFFFFFFFu;

static const char *stage_name(int s) {
    switch (s) {
    case PC_STAGE_START:      return "process-start";
    case PC_STAGE_OSINIT:     return "osInitialize";
    case PC_STAGE_THREAD:     return "first-thread-dispatched";
    case PC_STAGE_RETRACE:    return "first-vi-retrace";
    case PC_STAGE_GFXTASK:    return "first-graphics-task";
    case PC_STAGE_LOGOS:      return "logos";
    case PC_STAGE_OPENING:    return "opening-movie";
    case PC_STAGE_TITLE:      return "title-screen";
    case PC_STAGE_DEMO:       return "attract-demo";
    case PC_STAGE_MENU:       return "file-select-menu";
    case PC_STAGE_LEVELSELECT:return "level-select";
    case PC_STAGE_GAMEPLAY:   return "gameplay";
    }
    return "unknown";
}

/* gGameState -> milestone. Unlisted states (0xA is the pause/soft-reset
 * shunt, 0x11 is game over) do not move the ceiling; they are still recorded
 * in the route. */
static int stage_of_gamestate(u32 s) {
    switch (s) {
    case 1:  return PC_STAGE_LOGOS;
    case 2:  return PC_STAGE_OPENING;
    case 3:
    case 5:
    case 7:
    case 9:  return PC_STAGE_TITLE;
    case 4:
    case 6:
    case 8:  return PC_STAGE_DEMO;
    case 10: return PC_STAGE_MENU;
    case 11:
    case 12:
    case 14: return PC_STAGE_LEVELSELECT;
    case 15: return PC_STAGE_GAMEPLAY;
    }
    return -1;
}

static double now_s(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void pc_progress_mark(int stage) {
    double t;

    if (stage < 0 || stage > PC_STAGE_GAMEPLAY) {
        return;
    }
    if (sFirstAt[stage] != 0.0 && stage <= sStage) {
        return; /* seen, and not a new ceiling -- the common path */
    }
    t = now_s() - sT0;
    if (sFirstAt[stage] == 0.0) {
        sFirstAt[stage] = t;
        if (sVerbose) {
            fprintf(stderr, "[progress] %+7.2fs  %s%s\n", t, stage_name(stage),
                    stage <= sStage ? "  (out of order)" : "");
            fflush(stderr);
        }
    }
    if (stage > sStage) {
        sStage = stage;
        sStageAt = t;
    }
}

/* Sampled from pc_pump_events(), which every blocking libultra call goes
 * through, so a state change is noticed within one scheduler hop. Cheap: two
 * loads and a compare on the common path. */
void pc_progress_tick(void) {
    u32 gs = gGameState;
    int stage;

    if (gs == sLastGameState) {
        return;
    }
    sLastGameState = gs;
    if (sRouteLen < PC_ROUTE_MAX) {
        sRoute[sRouteLen++] = (unsigned)gs;
    }
    if (sVerbose) {
        fprintf(stderr, "[progress] %+7.2fs  gGameState=%u frames=%d\n",
                now_s() - sT0, (unsigned)gs, (int)gtlDrawnFrameCounter);
        fflush(stderr);
    }
    stage = stage_of_gamestate(gs);
    if (stage >= 0) {
        pc_progress_mark(stage);
    }
}

/* ------------------------------------------------------------- the verdict
 *
 * ONE LINE, KEY=VALUE, on stderr. Both a human and tools/pc/smoke.py read it,
 * which is why it is neither prose nor JSON. */
static void write_verdict(const char *outcome, const char *detail) {
    char buf[512];
    char route[192];
    int n = 0;
    int i;

    if (sReported) {
        return;
    }
    sReported = 1;

    route[0] = '\0';
    for (i = 0; i < sRouteLen; i++) {
        int k = (int)strlen(route);
        if (k > (int)sizeof(route) - 8) {
            break;
        }
        snprintf(route + k, sizeof(route) - (size_t)k, "%s%u",
                 i ? ">" : "", sRoute[i]);
    }

    n = snprintf(buf, sizeof(buf),
                 "[verdict] outcome=%s stage=%s stage_at=%.2f elapsed=%.2f "
                 "gamestate=%u frames=%d first_retrace=%.2f first_gfxtask=%.2f "
                 "route=%s%s%s\n",
                 outcome, stage_name(sStage), sStageAt, now_s() - sT0,
                 (unsigned)gGameState, (int)gtlDrawnFrameCounter,
                 sFirstAt[PC_STAGE_RETRACE], sFirstAt[PC_STAGE_GFXTASK],
                 route[0] ? route : "-",
                 detail ? " detail=" : "", detail ? detail : "");
    if (n > 0) {
        ssize_t ignored = write(2, buf, (size_t)n);
        (void)ignored;
    }
}

void pc_progress_report(const char *outcome) {
    write_verdict(outcome ? outcome : "exit", NULL);
}

static void at_exit_report(void) {
    write_verdict("exit", NULL);
}

/* ------------------------------------------------------------ crash handler */

static void fatal(int sig) {
    static const char *names[] = { "?", "SIGSEGV", "SIGBUS", "SIGFPE",
                                   "SIGILL", "SIGABRT" };
    const char *name = "signal";
    void *frames[48];
    int n;

    switch (sig) {
    case SIGSEGV: name = names[1]; break;
    case SIGBUS:  name = names[2]; break;
    case SIGFPE:  name = names[3]; break;
    case SIGILL:  name = names[4]; break;
    case SIGABRT: name = names[5]; break;
    default: break;
    }

    write_verdict("crash", name);

    {
        static const char hdr[] = "[verdict] backtrace:\n";
        ssize_t ignored = write(2, hdr, sizeof(hdr) - 1);
        (void)ignored;
    }
    n = backtrace(frames, (int)(sizeof(frames) / sizeof(frames[0])));
    backtrace_symbols_fd(frames, n, 2);

    /* Restore the default disposition and re-raise, so the shell still sees a
     * real signal exit status and a core file is still produced if enabled. */
    signal(sig, SIG_DFL);
    raise(sig);
}

void pc_progress_init(void) {
    static int done;
    struct sigaction sa;

    if (done) {
        return;
    }
    done = 1;

    sT0 = now_s();
    sStageAt = 0.0;
    sVerbose = getenv("KIRBY_PC_PROGRESS") != NULL;
    atexit(at_exit_report);
    pc_progress_mark(PC_STAGE_OSINIT);

    if (getenv("KIRBY_PC_NOCRASHTRAP") != NULL) {
        return;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fatal;
    /* SA_NODEFER off, SA_RESETHAND off: fatal() restores SIG_DFL itself, which
     * is the same effect and works for a signal raised inside the handler. */
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}
