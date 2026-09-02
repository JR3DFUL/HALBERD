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

/* Level-settings-block load diagnostics (func_800F78E4's PORT arm). */
void pc_levelload_debug(unsigned int id, unsigned int bytes, unsigned int nodes,
                        unsigned int ents, const void *base) {
    if (!pc_sched_debug_enabled()) {
        return;
    }
    fprintf(stderr, "[level] setup=%08x bytes=%06x nodes=%u ents=%u base=%p\n",
            id, bytes, nodes, ents, base);
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

/* Skybox-layer placement (func_80100790's PORT arm, src/ovl2/ovl2_6.c).
 * KIRBY_PC_SKYDEBUG=1 prints every layer of the first four draw calls, then
 * one call in 512: the camera rect the layer is placed in, its span before
 * clipping, texel size, per-texel pixel scale, the derived out flags, the
 * image format and whether the rect test culled it. */
void pc_sky_debug(int objId, const float *rect, float x0, float y0, float x1, float y1,
                  int w, int h, float sx, float sy, unsigned out, int fmt, int cull,
                  const void *img, const unsigned short *tlut) {
    const unsigned char *ib = (const unsigned char *)img;
    static int on = -1;
    static unsigned n;
    if (on < 0) {
        on = getenv("KIRBY_PC_SKYDEBUG") != NULL;
    }
    if (!on) {
        return;
    }
    n++;
    if (n > 4 * 8 && (n & 511) != 0) {
        return;
    }
    fprintf(stderr,
            "[sky] obj=%d rect=(%.1f,%.1f)-(%.1f,%.1f) layer=(%.2f,%.2f)-(%.2f,%.2f) "
            "tex=%dx%d scale=%.3fx%.3f out=%#x fmt=%d cull=%d img=%p "
            "[%02x%02x%02x%02x] tlut=[%04x %04x %04x %04x]\n",
            objId, rect[2], rect[3], rect[4], rect[5], x0, y0, x1, y1, w, h, sx, sy, out, fmt,
            cull, img, ib ? ib[0] : 0, ib ? ib[1] : 0, ib ? ib[2] : 0, ib ? ib[3] : 0,
            tlut[0], tlut[1], tlut[2], tlut[3]);
}

/* The camera numbers the skybox scroll (func_80100EE4's PORT arm) derives
 * its parallax from: the D_800D7B20 at/eye snapshot ovl2_3.c keeps, the
 * scene camera's live lookAt, and the resulting yaw/pitch screen fractions.
 * Same KIRBY_PC_SKYDEBUG switch, one line in 256 calls. */
void pc_sky_cam_debug(const float *snap, const float *at, const float *eye, float yawFrac,
                      float pitchFrac, float fovy, float aspect) {
    static int on = -1;
    static unsigned n;
    if (on < 0) {
        on = getenv("KIRBY_PC_SKYDEBUG") != NULL;
    }
    if (!on || ((n++ & 255) != 0)) {
        return;
    }
    fprintf(stderr,
            "[skycam] snap at=(%.1f,%.1f,%.1f) eye=(%.1f,%.1f,%.1f) live at=(%.1f,%.1f,%.1f) "
            "eye=(%.1f,%.1f,%.1f) yawFrac=%.4f pitchFrac=%.4f fovy=%.1f aspect=%.3f\n",
            snap[0], snap[1], snap[2], snap[3], snap[4], snap[5], at[0], at[1], at[2], eye[0],
            eye[1], eye[2], yawFrac, pitchFrac, fovy, aspect);
}

/* Transient bring-up tap: file-select flow tracing (KIRBY_PC_FSDEBUG). */
#include <stdio.h>
#include <stdlib.h>
void pc_fsel_debug(const char *tag, int a, int b, int c) {
    static int on = -1;
    if (on < 0) {
        on = getenv("KIRBY_PC_FSDEBUG") != NULL;
    }
    if (on) {
        fprintf(stderr, "[fsel] %s a=%d b=%d c=%d\n", tag, a, b, c);
    }
}
