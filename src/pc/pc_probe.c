/* A counter and a bounded print, for questions the picture cannot answer.
 *
 * WHY A SEPARATE FILE. pc_progress.c samples globals from the OUTSIDE -- it
 * can say where the player is, not whether a particular line of game code
 * ran. Some questions are only about that. "Does anything read stickX?" and
 * "does the track-node hop ever fire?" are both of that shape: the value is
 * visible, the reader is not, and guessing which function owns it has
 * already cost this port more than one wrong conclusion.
 *
 * So the game calls in. A probe site is
 *
 *     #ifdef PORT
 *     { extern void pc_probe_hit(const char *); pc_probe_hit("tag"); }
 *     #endif
 *
 * inside the decomp, behind PORT so the ROM build is byte-identical, with the
 * prototype declared at the site so no decomp header has to learn about the
 * platform layer.
 *
 * TAGS ARE COMPARED BY POINTER FIRST, then by strcmp, so a site that always
 * passes the same string literal costs one compare. Nothing here allocates.
 *
 * OUTPUT. Two kinds, and the distinction matters:
 *
 *   pc_probe_hit(tag)              counts. Silent. Dumped as a table.
 *   pc_probe_say(tag, limit, ...)  counts AND prints the first `limit`
 *                                  occurrences with their operands.
 *
 * A counter answers "did it run"; the operands answer "with what". Printing
 * every occurrence of a per-frame site drowns the log and slows the run
 * enough to change it, which is why `say` is bounded rather than throttled.
 *
 * All of it is off unless KIRBY_PC_PROBE is set, and the off path is one
 * load and a branch.
 */
#include <ultra64.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "pc/pc_platform.h"

#define PC_PROBE_MAX_TAGS 48

static struct {
    const char *tag;
    unsigned long hits;
    int said;
} sTags[PC_PROBE_MAX_TAGS];

static int sNumTags;
static int sOn = -1;          /* -1 not yet decided, 0 off, 1 on */
static double sT0;
static double sNextDump;
static double sEvery;

static double probe_now(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int probe_on(void) {
    if (sOn < 0) {
        const char *s = getenv("KIRBY_PC_PROBE");

        sOn = (s != NULL && *s != '\0' && strcmp(s, "0") != 0);
        sT0 = probe_now();
        sEvery = 10.0;
        sNextDump = sEvery;
    }
    return sOn;
}

/* Slot for `tag`, creating it on first sight. Returns NULL when the table is
 * full, which is a probe-authoring bug rather than a runtime condition, so it
 * fails quietly rather than growing. */
static int tag_slot(const char *tag) {
    int i;

    for (i = 0; i < sNumTags; i++) {
        if (sTags[i].tag == tag || strcmp(sTags[i].tag, tag) == 0) {
            return i;
        }
    }
    if (sNumTags >= PC_PROBE_MAX_TAGS) {
        return -1;
    }
    sTags[sNumTags].tag = tag;
    sTags[sNumTags].hits = 0;
    sTags[sNumTags].said = 0;
    return sNumTags++;
}

void pc_probe_hit(const char *tag) {
    int i;

    if (!probe_on() || tag == NULL) {
        return;
    }
    i = tag_slot(tag);
    if (i >= 0) {
        sTags[i].hits++;
    }
}

void pc_probe_say(const char *tag, int limit, const char *fmt, ...) {
    va_list ap;
    int i;

    if (!probe_on() || tag == NULL) {
        return;
    }
    i = tag_slot(tag);
    if (i < 0) {
        return;
    }
    sTags[i].hits++;
    if (sTags[i].said >= limit) {
        return;
    }
    sTags[i].said++;
    fprintf(stderr, "[probe] %+7.2fs %-22s #%lu  ", probe_now() - sT0, tag,
            sTags[i].hits);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* THE THIRD KIND, and the one a stuck player needs.
 *
 * `say` prints the FIRST `limit` occurrences, which is the right shape for a
 * site that fires once or twice. It is the wrong shape for a per-frame site
 * whose interesting behaviour starts a minute into the run: the budget is
 * spent on the first second and the log is silent for exactly the part that
 * matters. `every` spends its budget on the clock instead -- at most one line
 * per `seconds` of wall time, for as long as the run lasts. */
void pc_probe_every(const char *tag, double seconds, const char *fmt, ...) {
    va_list ap;
    double t;
    int i;

    if (!probe_on() || tag == NULL) {
        return;
    }
    i = tag_slot(tag);
    if (i < 0) {
        return;
    }
    sTags[i].hits++;
    t = probe_now() - sT0;
    /* `said` doubles as the next-allowed second here; a tag is only ever used
     * with one of say/every, so the two never share a slot's meaning. */
    if (t < (double)sTags[i].said) {
        return;
    }
    sTags[i].said = (int)(t + seconds);
    fprintf(stderr, "[probe] %+7.2fs %-22s #%lu  ", t, tag, sTags[i].hits);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

void pc_probe_dump(void) {
    int i;

    if (!probe_on()) {
        return;
    }
    fprintf(stderr, "[probe] --- counters at %+7.2fs ---\n",
            probe_now() - sT0);
    for (i = 0; i < sNumTags; i++) {
        fprintf(stderr, "[probe]   %-24s %lu\n", sTags[i].tag, sTags[i].hits);
    }
    if (sNumTags == 0) {
        fprintf(stderr, "[probe]   (no site was ever reached)\n");
    }
    fflush(stderr);
}

/* Called from the same place pc_progress_playerpos is, so a run that is
 * killed rather than exited still leaves a table behind. */
void pc_probe_tick(void) {
    double t;

    if (!probe_on()) {
        return;
    }
    t = probe_now() - sT0;
    if (t < sNextDump) {
        return;
    }
    sNextDump = t + sEvery;
    pc_probe_dump();
}
