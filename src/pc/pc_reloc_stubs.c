/* No-op host hooks for the JRickey libultraship fork's reloc scheme.
 *
 * The fork's Fast3D interpreter (src/fast/interpreter.cpp:63-68) declares
 * five REQUIRED extern "C" host symbols. They exist because BattleShip/SSB64
 * serves its display lists out of relocatable resource files ("reloc files"):
 * a DL there can carry a 32-bit reloc TOKEN instead of a pointer, vertices
 * and textures inside a reloc file may still be byte-swapped on first touch,
 * and the interpreter asks the HOST to resolve tokens, name the containing
 * file, and perform those at-first-use fixups.
 *
 * Kirby 64 has none of that. This port hands Fast3D real display lists in
 * host memory: every pointer is either already a host pointer or a classic
 * segmented address that the interpreter's own segment table (fed by
 * G_MOVEWORD/G_MW_SEGMENT in the list) resolves. So all five hooks are
 * honest no-ops, and returning NULL/false is not a degraded mode -- it is
 * the documented fallback: when portRelocTryResolvePointer returns NULL and
 * portRelocFindContainingFile returns false, SegAddr falls back to classic
 * segmented addressing, and the describe/fixup hooks are only reached for
 * pointers a reloc file claimed (none ever will be claimed here).
 *
 * Signatures mirror interpreter.cpp:63-68 exactly; this is a C file, so
 * <stdbool.h> supplies the _Bool that matches C++ bool in the SysV ABI.
 * Linked unconditionally (it sits with the other src/pc objects in
 * build/pc/src/pc/), harmless when
 * libultraship is not (PC_LUS=0): nothing else references these symbols. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* A reloc token in w1 of a DL command -> host pointer. NULL = "not a token,
 * treat it as an address" (interpreter.cpp:4838). */
void* portRelocTryResolvePointer(uint32_t token) {
    (void)token;
    return NULL;
}

/* Which reloc file contains ptr? false = "no reloc file does", which routes
 * SegAddr and the DL bounds checks down the classic path. */
bool portRelocFindContainingFile(const void* ptr, uintptr_t* out_base, size_t* out_size) {
    (void)ptr;
    (void)out_base;
    (void)out_size;
    return false;
}

/* Diagnostic naming of a pointer for crash/trace output. false = "unknown to
 * the reloc scheme"; the interpreter then prints the raw pointer instead. */
bool portRelocDescribePointer(const void* ptr, uintptr_t* out_base, size_t* out_size,
                              uint32_t* out_file_id, const char** out_path) {
    (void)ptr;
    (void)out_base;
    (void)out_size;
    (void)out_file_id;
    (void)out_path;
    return false;
}

/* First-touch byte-swap of vertices/textures inside a reloc file. Kirby's
 * data is already in host byte order by the time Fast3D sees it (src/pc/
 * os_ai.c and the asset pipeline own any swapping), so these do nothing. */
void portRelocFixupVertexAtRuntime(const void* addr, unsigned int num_vtx) {
    (void)addr;
    (void)num_vtx;
}

void portRelocFixupTextureAtRuntime(const void* addr, unsigned int num_bytes) {
    (void)addr;
    (void)num_bytes;
}

/* The fork's SDL2 backend fires CALL_EVENT(WindowFocusEvent, ...) whose ID
 * variable the HOST must define (BattleShip registers it via REGISTER_EVENT
 * in port/hooks/Events.cpp). -1 = never registered, so the call is a no-op
 * until this port grows an event system of its own. */
unsigned int WindowFocusEventID = (unsigned int)-1;

/* BattleShip's GBI trace hook (debug_tools/gbi_trace). Interpreter calls it
 * on every vertex-buffer flush; a no-op loses only the trace/cost metrics. */
void gbi_trace_note_flush(int num_tris) {
    (void)num_tris;
}

/* Boot-stall diagnostics for the scheduler loop (sched.c PORT block). */
#include <stdio.h>
#include <stdlib.h>
int pc_sched_debug_enabled(void) {
    static int on = -1;
    if (on < 0) {
        on = getenv("KIRBY_PC_SCHEDDEBUG") != 0;
    }
    return on;
}
void pc_sched_debug_print(int msg, int n) {
    fprintf(stderr, "[sched] #%d msg=%d\n", n, msg);
}

/* Geo/model bank load diagnostics (func_800A9250's PORT arm). */
void pc_geoload_debug(unsigned int id, unsigned int mode, unsigned int size,
                      const void *blob, unsigned int layoutOff) {
    if (!pc_sched_debug_enabled()) {
        return;
    }
    fprintf(stderr, "[geo] id=%08x mode=%02x size=%06x blob=%p layout=+%05x\n",
            id, mode, size, blob, layoutOff);
}

/* Anim bank load diagnostics (func_800A94F4's PORT arm). */
void pc_animload_debug(unsigned int id, unsigned int bytes, unsigned int kind,
                       unsigned int relocs, const void *blk) {
    if (!pc_sched_debug_enabled()) {
        return;
    }
    fprintf(stderr, "[anim] id=%08x bytes=%06x kind=%u relocs=%u blk=%p\n",
            id, bytes, kind, relocs, blk);
}

void pc_bgload_debug(unsigned int id, const void *raw, const void *img, const void *pal) {
    const unsigned char *r = (const unsigned char *)raw;
    const unsigned char *i = (const unsigned char *)img;
    if (!pc_sched_debug_enabled()) {
        return;
    }
    fprintf(stderr,
            "[bgload] id=%08x raw=%p fmt=%u siz=%u w=%u h=%u img=%p pal=%p "
            "imgbytes=%02x%02x%02x%02x%02x%02x%02x%02x\n",
            id, raw, r[0], r[1], *(const unsigned short *)(r + 4), *(const unsigned short *)(r + 6),
            img, pal, i[0], i[1], i[2], i[3], i[4], i[5], i[6], i[7]);
}
