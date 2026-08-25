/* F3DEX2 display-list tracer.
 *
 * Not a renderer, and specifically not the first half of one written badly.
 * It walks a display list, decodes each command's opcode and the fields that
 * are cheap and unambiguous to read, follows G_DL branches, and stops at
 * G_ENDDL. That is the part of "interpret a display list" that contains no
 * rendering decisions at all, so it can be written now, be correct now, and
 * be thrown away or kept independently of whatever renderer eventually
 * appears.
 *
 * It earns its place by answering questions that otherwise get guessed:
 * how deep do this game's lists nest, which combine modes does it actually
 * use, does it use G_LOAD_UCODE to switch between F3DEX2 and S2DEX mid-frame
 * (it does -- gspS2DEX2_fifo is in the ROM), how many primitives per frame.
 * Those answers shape a renderer's design and none of them require one.
 *
 * ONE THING IT CANNOT DO, and this is the honest limit: every pointer in a
 * display list is a SEGMENTED address -- a 4-bit segment number and a 24-bit
 * offset, resolved through a table the list itself loads with
 * G_MOVEWORD/G_MW_SEGMENT. The tracer tracks that table as it goes, so
 * addresses inside one list resolve, but a list that assumes a segment set up
 * by an earlier task will show unresolved pointers. They are printed as
 * `seg%X:%06X` rather than silently mapped to something wrong.
 *
 * AND IT CANNOT TELL THE TWO APART, which is worse and was measured. The
 * port's game memory is a static array in this binary's .bss, so a host
 * pointer to a display list is a small number -- 0x016F8E70 for the galaxy
 * map's DL head -- and 0x016F8E70 read as a segmented address is segment 1,
 * offset 0x6F8E70. Segment 1 is unbound, so resolve() returns NULL and the
 * walk stops. Every galaxy-map frame traced as "2 commands, 0 triangles"
 * while the renderer was walking thousands. The image spans 0x400000.._end,
 * which covers the whole N64 segmented range too, so no address test
 * separates them; only knowing which allocator produced the value would,
 * and the tracer does not.
 *
 * So: this walker is for lists that are WELL FORMED and whose segments are
 * bound in-list. When the question is "what did the renderer actually
 * execute, and what killed it", use the GBI ring at the bottom of this file
 * (PC_TRACE=gbi) -- it records from inside the interpreter and has no
 * opinion about what an address means.
 *
 * Enable with PC_TRACE=gfx.
 *
 * A NOTE ON POINTER WIDTH, because this file got it wrong once and the output
 * was plausible enough to be believed. A Gfx is TWO uintptr_t, not two u32:
 * include/PR/gbi.h declares Gwords that way and include/PR/ultratypes.h widens
 * uintptr_t under PORT, so a display-list command is 8 bytes on N64 and 16 in
 * this build. Walking it as `const u32 *` with `p += 2` therefore reads every
 * command's low half twice and shifts by one word per command -- which decoded
 * SETFILLCOLOR's colour argument as if it were the next opcode. It walks Gfx
 * now, which is the only correct thing at either width.
 */
#include <ultra64.h>
#include <PR/gbi.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "pc/pc_platform.h"

#define MAX_DEPTH 12
#define MAX_CMDS  200000

typedef struct {
    uintptr_t seg[16];
    int depth;
    unsigned count;
    unsigned tris;
    unsigned verts;
} TraceState;

static const char *op_name(u32 op) {
    switch (op) {
        case G_NOOP:            return "NOOP";
        case G_VTX:             return "VTX";
        case G_MODIFYVTX:       return "MODIFYVTX";
        case G_CULLDL:          return "CULLDL";
        case G_BRANCH_Z:        return "BRANCH_Z";
        case G_TRI1:            return "TRI1";
        case G_TRI2:            return "TRI2";
        case G_QUAD:            return "QUAD";
        case G_LINE3D:          return "LINE3D";
        case G_SPECIAL_3:       return "SPECIAL_3";
        case G_SPECIAL_2:       return "SPECIAL_2";
        case G_SPECIAL_1:       return "SPECIAL_1";
        case G_DMA_IO:          return "DMA_IO";
        case G_TEXTURE:         return "TEXTURE";
        case G_POPMTX:          return "POPMTX";
        case G_GEOMETRYMODE:    return "GEOMETRYMODE";
        case G_MTX:             return "MTX";
        case G_MOVEWORD:        return "MOVEWORD";
        case G_MOVEMEM:         return "MOVEMEM";
        case G_LOAD_UCODE:      return "LOAD_UCODE";
        case G_DL:              return "DL";
        case G_ENDDL:           return "ENDDL";
        case G_SPNOOP:          return "SPNOOP";
        case G_RDPHALF_1:       return "RDPHALF_1";
        case G_SETOTHERMODE_L:  return "SETOTHERMODE_L";
        case G_SETOTHERMODE_H:  return "SETOTHERMODE_H";
        case G_TEXRECT:         return "TEXRECT";
        case G_TEXRECTFLIP:     return "TEXRECTFLIP";
        case G_RDPLOADSYNC:     return "RDPLOADSYNC";
        case G_RDPPIPESYNC:     return "RDPPIPESYNC";
        case G_RDPTILESYNC:     return "RDPTILESYNC";
        case G_RDPFULLSYNC:     return "RDPFULLSYNC";
        case G_SETKEYGB:        return "SETKEYGB";
        case G_SETKEYR:         return "SETKEYR";
        case G_SETCONVERT:      return "SETCONVERT";
        case G_SETSCISSOR:      return "SETSCISSOR";
        case G_SETPRIMDEPTH:    return "SETPRIMDEPTH";
        case G_RDPSETOTHERMODE: return "RDPSETOTHERMODE";
        case G_LOADTLUT:        return "LOADTLUT";
        case G_RDPHALF_2:       return "RDPHALF_2";
        case G_SETTILESIZE:     return "SETTILESIZE";
        case G_LOADBLOCK:       return "LOADBLOCK";
        case G_LOADTILE:        return "LOADTILE";
        case G_SETTILE:         return "SETTILE";
        case G_FILLRECT:        return "FILLRECT";
        case G_SETFILLCOLOR:    return "SETFILLCOLOR";
        case G_SETFOGCOLOR:     return "SETFOGCOLOR";
        case G_SETBLENDCOLOR:   return "SETBLENDCOLOR";
        case G_SETPRIMCOLOR:    return "SETPRIMCOLOR";
        case G_SETENVCOLOR:     return "SETENVCOLOR";
        case G_SETCOMBINE:      return "SETCOMBINE";
        case G_SETTIMG:         return "SETTIMG";
        case G_SETZIMG:         return "SETZIMG";
        case G_SETCIMG:         return "SETCIMG";
        default:                return "???";
    }
}

/* Segmented address -> host pointer, or NULL if the segment is unknown.
 *
 * Segment 0 is the identity segment on N64 (a raw KSEG0 address). On PC the
 * "physical" address IS the host pointer -- see osVirtualToPhysical in
 * src/pc/os_time.c -- so segment 0 resolves to the value itself. */
static const void *resolve(TraceState *st, uintptr_t addr) {
    u32 seg;
    u32 off;

    /* A value that does not fit in 32 bits cannot be a segmented address: it
     * is a host pointer that the LP64 gbi.h macros stored directly. This is
     * the case for every list the port builds itself, and it will be the case
     * for game lists once assets stop being segmented ROM offsets. */
    if (addr > 0xFFFFFFFFu) {
        return (const void *)addr;
    }
    seg = (u32)((addr >> 24) & 0xF);
    off = (u32)(addr & 0x00FFFFFF);

    if (seg == 0) {
        return (const void *)addr;
    }
    if (st->seg[seg] == 0) {
        return NULL;
    }
    return (const void *)(st->seg[seg] + off);
}

static void trace_list(TraceState *st, const Gfx *p);

static void trace_indent(TraceState *st) {
    int i;

    for (i = 0; i < st->depth; i++) {
        fputs("  ", stderr);
    }
}

static void trace_list(TraceState *st, const Gfx *p) {
    if (p == NULL || st->depth >= MAX_DEPTH) {
        return;
    }
    st->depth++;

    for (;;) {
        uintptr_t full0;
        uintptr_t full1;
        u32 w0;
        u32 w1;
        u32 op;

        if (st->count++ >= MAX_CMDS) {
            trace_indent(st);
            fprintf(stderr, "... command limit reached, list truncated\n");
            break;
        }
        full0 = p->words.w0;
        full1 = p->words.w1;
        w0 = (u32)full0;
        w1 = (u32)full1;
        op = w0 >> 24;

        trace_indent(st);
        fprintf(stderr, "%016llX %016llX  %s", (unsigned long long)full0,
                (unsigned long long)full1, op_name(op));

        switch (op) {
            case G_VTX: {
                unsigned n = (w0 >> 12) & 0xFF;

                st->verts += n;
                fprintf(stderr, "  n=%u dst=%u src=%08X", n,
                        ((w0 >> 1) & 0x7F), w1);
                break;
            }
            case G_TRI1:
                st->tris += 1;
                break;
            case G_TRI2:
            case G_QUAD:
                st->tris += 2;
                break;
            case G_MOVEWORD: {
                unsigned index = (w0 >> 16) & 0xFF;
                unsigned offset = w0 & 0xFFFF;

                fprintf(stderr, "  index=%02X offset=%04X value=%08X", index,
                        offset, w1);
                if (index == G_MW_SEGMENT) {
                    /* offset is the segment number * 4 */
                    unsigned s = (offset >> 2) & 0xF;

                    st->seg[s] = full1;
                    fprintf(stderr, "   [segment %X = %016llX]", s,
                            (unsigned long long)full1);
                }
                break;
            }
            case G_SETTIMG: {
                const void *t = resolve(st, full1);

                if (t != NULL) {
                    fprintf(stderr, "  img=%p", t);
                } else {
                    fprintf(stderr, "  img=seg%X:%06X (unresolved)",
                            (w1 >> 24) & 0xF, w1 & 0xFFFFFF);
                }
                break;
            }
            case G_SETCIMG:
            case G_SETZIMG:
                fprintf(stderr, "  addr=%08X", w1);
                break;
            case G_SETCOMBINE:
                fprintf(stderr, "  %05X %08X", w0 & 0xFFFFFF, w1);
                break;
            case G_GEOMETRYMODE:
                fprintf(stderr, "  clear=%06X set=%08X", w0 & 0xFFFFFF, w1);
                break;
            case G_MTX:
                fprintf(stderr, "  params=%02X mtx=%08X", w0 & 0xFF, w1);
                break;
            case G_LOAD_UCODE:
                fprintf(stderr, "  <<< microcode switch >>> text=%08X", w1);
                break;
            case G_DL: {
                const void *sub = resolve(st, full1);
                int push = ((w0 >> 16) & 0xFF) == G_DL_PUSH;

                fprintf(stderr, "  %s %016llX\n", push ? "push" : "branch",
                        (unsigned long long)full1);
                if (sub != NULL) {
                    trace_list(st, (const Gfx *)sub);
                }
                if (!push) {
                    st->depth--;
                    return; /* branch does not return */
                }
                p++;
                continue;
            }
            case G_ENDDL:
                fputc('\n', stderr);
                st->depth--;
                return;
            default:
                break;
        }
        fputc('\n', stderr);
        p++;
    }
    st->depth--;
}

void pc_gfx_trace_task(OSTask *task) {
    TraceState st;

    if (!(pc_trace_mask & PC_TR_GFX) || task == NULL ||
        task->t.data_ptr == NULL) {
        return;
    }
    memset(&st, 0, sizeof(st));
    st.depth = -1; /* so the outermost list prints unindented */

    fprintf(stderr, "[gfx] ==== display list at %p (%u bytes declared) ====\n",
            (void *)task->t.data_ptr, (unsigned)task->t.data_size);
    trace_list(&st, (const Gfx *)task->t.data_ptr);
    fprintf(stderr, "[gfx] ==== %u commands, %u vertices, %u triangles ====\n",
            st.count, st.verts, st.tris);
}

/* ------------------------------------------------------------- GBI ring
 *
 * The tracer above walks a display list from the outside, which is exactly
 * what you want until the list is malformed -- then the tracer's own idea of
 * where the commands are diverges from the renderer's, and it reports on a
 * list nobody executed. (It did: the port's DL heads live in the low 32 bits
 * of the address space, so resolve() below reads 0x016F8E70 as segment 1 and
 * refuses to descend. Every galaxy-map frame traced as "2 commands".)
 *
 * This records the command stream from INSIDE the renderer instead.
 * libultraship's interpreter offers gfx_set_trace_callback(), invoked once
 * per command with the words it is about to decode and the current G_DL
 * depth. We keep the last RING_N of them and dump them when the process
 * dies, so a SIGSEGV in Fast::Interpreter::Run names the command that killed
 * it instead of a stack frame.
 *
 * The dump runs from a signal handler, so it uses write(2) and formats its
 * own hex: no stdio, no malloc, no locks.
 *
 * Enable with PC_TRACE=gbi. */

#define RING_N 128

typedef struct {
    uintptr_t w0;
    uintptr_t w1;
    int depth;
} GbiSlot;

static GbiSlot sRing[RING_N];
static unsigned sRingHead;      /* total commands seen, not an index */
static int sRingOn;

static void gbi_ring_record(uintptr_t w0, uintptr_t w1, int depth) {
    GbiSlot *s = &sRing[sRingHead % RING_N];

    s->w0 = w0;
    s->w1 = w1;
    s->depth = depth;
    sRingHead++;
}

/* Declared by libultraship's fast/interpreter.h as extern "C". Repeated here
 * so this stays a C file; the signature must match that header. Weak because
 * this file is also linked into the null-backend build, which has no
 * interpreter to hook -- there the ring simply stays silent. */
__attribute__((weak)) void gfx_set_trace_callback(void (*cb)(uintptr_t w0,
                                                             uintptr_t w1,
                                                             int depth));

void pc_gbi_ring_init(void) {
    if (!(pc_trace_mask & PC_TR_GBI) || gfx_set_trace_callback == NULL) {
        return;
    }
    sRingOn = 1;
    gfx_set_trace_callback(gbi_ring_record);
    fprintf(stderr, "[gbi] ring armed: last %d commands dumped on crash\n",
            RING_N);
}

static char *put_hex(char *p, unsigned long long v, int digits) {
    static const char kHex[] = "0123456789ABCDEF";
    int i;

    for (i = digits - 1; i >= 0; i--) {
        p[i] = kHex[v & 0xF];
        v >>= 4;
    }
    return p + digits;
}

static char *put_str(char *p, const char *s) {
    while (*s != '\0') {
        *p++ = *s++;
    }
    return p;
}

void pc_gbi_ring_dump(void) {
    char line[160];
    unsigned n;
    unsigned i;
    unsigned first;

    if (!sRingOn || sRingHead == 0) {
        return;
    }
    n = sRingHead < RING_N ? sRingHead : RING_N;
    first = sRingHead - n;

    {
        static const char hdr[] = "[gbi] last commands the interpreter decoded "
                                  "(oldest first); the one AFTER the last line "
                                  "is the one it died on:\n";
        ssize_t ignored = write(2, hdr, sizeof(hdr) - 1);
        (void)ignored;
    }
    for (i = 0; i < n; i++) {
        const GbiSlot *s = &sRing[(first + i) % RING_N];
        char *p = line;
        int d;
        ssize_t ignored;

        p = put_str(p, "[gbi] #");
        p = put_hex(p, first + i, 6);
        p = put_str(p, " d");
        d = s->depth < 0 ? 0 : (s->depth > 15 ? 15 : s->depth);
        p = put_hex(p, (unsigned)d, 1);
        p = put_str(p, " op");
        p = put_hex(p, (unsigned)((s->w0 >> 24) & 0xFF), 2);
        p = put_str(p, "  ");
        p = put_hex(p, (unsigned long long)s->w0, 16);
        *p++ = ' ';
        p = put_hex(p, (unsigned long long)s->w1, 16);
        *p++ = '\n';
        ignored = write(2, line, (size_t)(p - line));
        (void)ignored;
    }
}
