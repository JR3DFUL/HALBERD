/* mmap/munmap over VirtualAlloc, for the mingw-w64 Windows build.
 *
 * WHY THIS FILE EXISTS. Two TUs include <sys/mman.h>:
 *
 *   src/pc/pc_mmio.c      MAP_FIXED at 0xA3F00000 (the RCP register window)
 *   src/ovl1/ovl1_3.c     MAP_32BIT for the widened-dlist command buffer
 *                         (game code reads its address through u32 slots),
 *                         and a >4 GiB hint mapping for vertex/texel copies
 *                         (the libultraship fork's low-VA guards demand it)
 *
 * Neither file may be edited for the Windows port (ovl1_3.c is game source;
 * pc_mmio.c is shared with the Linux build), so the Windows makefile puts
 * src/pc/win32 on the include path AHEAD of the system directories and this
 * header impersonates <sys/mman.h>. On any non-Windows compiler it refuses
 * loudly rather than silently shadowing the real one.
 *
 * ONLY the subset those two callers use is implemented:
 *
 *   - fd must be -1 and MAP_ANONYMOUS set (no file mappings);
 *   - PROT_READ|PROT_WRITE only (PAGE_READWRITE);
 *   - MAP_FIXED: VirtualAlloc at exactly that address or MAP_FAILED --
 *     same all-or-nothing contract the callers already handle;
 *   - MAP_32BIT: the result must sit wholly below 4 GiB. VirtualAlloc(NULL)
 *     allocates bottom-up when ASLR is off (this exe links with
 *     --disable-dynamicbase), so the first attempt usually qualifies; if it
 *     does not, a bounded scan of low 16 MiB-aligned bases finds a hole.
 *     Both callers re-check the <4GiB property themselves anyway;
 *   - a non-NULL addr WITHOUT MAP_FIXED is a hint: try there, then anywhere.
 *     ovl1_3.c hints 0x200000000 and then verifies >=4 GiB itself, so the
 *     fallback preserves its failure semantics;
 *   - MAP_NORESERVE is accepted and ignored: everything is committed up
 *     front. The one NORESERVE user (pc_mmio, 10 MiB) can afford that.
 *
 * Win32 declarations are hand-written instead of pulling in <windows.h>:
 * this header is included by GAME translation units (ovl1_3.c), and
 * windows.h's macro namespace (near, far, CONST, ...) is not something to
 * spray over decompiled Kirby source. On x64 the calling convention
 * annotations are moot; the four constants are ABI, fixed forever. */
#ifndef PC_WIN32_SYS_MMAN_H
#define PC_WIN32_SYS_MMAN_H

#ifndef _WIN32
#error "src/pc/win32/sys/mman.h is the Windows mmap shim; a non-Windows \
build must not have src/pc/win32 on its include path."
#endif

#include <stddef.h>
#include <stdint.h>

#define PROT_NONE      0x0
#define PROT_READ      0x1
#define PROT_WRITE     0x2

#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20
#define MAP_32BIT      0x40
#define MAP_NORESERVE  0x4000

#define MAP_FAILED     ((void *)-1)

/* kernel32 imports, minus <windows.h>. SIZE_T is ULONG_PTR is size_t. */
__declspec(dllimport) void *__stdcall VirtualAlloc(void *addr, size_t size,
                                                   unsigned long type,
                                                   unsigned long prot);
__declspec(dllimport) int __stdcall VirtualFree(void *addr, size_t size,
                                                unsigned long type);

#define PC_MEM_COMMIT      0x1000ul
#define PC_MEM_RESERVE     0x2000ul
#define PC_MEM_RELEASE     0x8000ul
#define PC_PAGE_READWRITE  0x04ul

static void *mmap(void *addr, size_t length, int prot, int flags, int fd,
                  long offset) {
    void *p;

    (void)prot;
    (void)offset;
    if (fd != -1 || !(flags & MAP_ANONYMOUS) || length == 0) {
        return MAP_FAILED;
    }

    if (flags & MAP_FIXED) {
        p = VirtualAlloc(addr, length, PC_MEM_RESERVE | PC_MEM_COMMIT,
                         PC_PAGE_READWRITE);
        return (p == addr && p != NULL) ? p : MAP_FAILED;
    }

    /* Hint (addr non-NULL, not fixed): try there, else fall through to
     * anywhere -- the caller re-validates the address range it needs. */
    if (addr != NULL) {
        p = VirtualAlloc(addr, length, PC_MEM_RESERVE | PC_MEM_COMMIT,
                         PC_PAGE_READWRITE);
        if (p != NULL) {
            return p;
        }
    }

    p = VirtualAlloc(NULL, length, PC_MEM_RESERVE | PC_MEM_COMMIT,
                     PC_PAGE_READWRITE);

    if (flags & MAP_32BIT) {
        if (p != NULL && (uintptr_t)p + length <= 0xFFFFFFFFull) {
            return p;
        }
        /* NULL landed high (or failed): scan low bases. 16 MiB steps from
         * 64 MiB up; 250-odd probes worst case, once per model load. */
        if (p != NULL) {
            VirtualFree(p, 0, PC_MEM_RELEASE);
        }
        {
            uintptr_t base;

            for (base = 0x04000000ull; base + length <= 0xFFFFFFFFull;
                 base += 0x01000000ull) {
                p = VirtualAlloc((void *)base, length,
                                 PC_MEM_RESERVE | PC_MEM_COMMIT,
                                 PC_PAGE_READWRITE);
                if (p != NULL) {
                    return p;
                }
            }
        }
        return MAP_FAILED;
    }

    return (p != NULL) ? p : MAP_FAILED;
}

static int munmap(void *addr, size_t length) {
    (void)length; /* VirtualFree(MEM_RELEASE) frees the whole reservation */
    return VirtualFree(addr, 0, PC_MEM_RELEASE) ? 0 : -1;
}

#endif /* PC_WIN32_SYS_MMAN_H */
