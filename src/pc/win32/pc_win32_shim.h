/* Force-included (gcc -include) into src/pc platform TUs by the Windows
 * build. NOT included by any source file, and never used on Linux -- it
 * exists so the Windows makefile can compile the UNMODIFIED platform layer.
 *
 * It papers over exactly the three POSIX-isms the mingw sweep found
 * (scratchpad/win32test/plat_errors.txt, 2026-08-14):
 *
 * 1. `errno` AS A STRUCT FIELD NAME. OSContStatus/OSContPad (libultra ABI,
 *    not editable) have a field literally named errno. mingw's <stdlib.h>
 *    defines errno as the macro (*_errno()), so `status[i].errno` stops
 *    parsing. glibc only does that in <errno.h>, which os_cont.c does not
 *    include -- that is why Linux never saw this. Cure: include the real
 *    headers that would define it, then #undef. The TUs this header is
 *    forced into never read the errno VARIABLE (they use f == NULL checks),
 *    so losing the macro costs nothing. os_pfs.c/os_pi.c include <errno.h>
 *    themselves AFTER this header; its include guard is already set by the
 *    include below, so the macro stays gone there too, and their only use
 *    of it -- also struct fields -- is exactly what needs it gone.
 *
 * 2. sigaction/SIGTERM-SIGINT (os_time.c installs a set-a-flag quit
 *    handler). mingw has ANSI signal() with SIGINT and SIGTERM but no
 *    sigaction. A minimal struct sigaction + sigemptyset + sigaction() that
 *    forwards to signal() reproduces the behaviour: the handler only sets a
 *    volatile flag, so none of sigaction's extra semantics (masks, restart)
 *    are load-bearing. (Windows delivers SIGINT for Ctrl-C via the CRT;
 *    SIGTERM exists but nothing raises it externally -- SetConsoleCtrlHandler
 *    would widen coverage and can be added in the Windows main later.)
 *
 * 3. setitimer/ITIMER_REAL/SIGALRM (pc_dbg.c, debug counters behind
 *    KIRBY_PC_PUMPDBG). No SIGALRM on Windows. The shim accepts and ignores
 *    the arm request: the debug dump never fires, the game is unaffected.
 *    (<sys/time.h> also does not exist under mingw; src/pc/win32/sys/time.h
 *    provides an empty one so pc_dbg.c's include line survives.)
 */
#ifndef PC_WIN32_SHIM_H
#define PC_WIN32_SHIM_H

#ifndef _WIN32
#error "pc_win32_shim.h is for the mingw-w64 build only."
#endif

/* --- 1: errno-the-macro vs errno-the-field ------------------------------ */
#include <errno.h>   /* sets its guard so later includes are no-ops */
#include <stdlib.h>
#undef errno

/* --- 2: sigaction over signal() ----------------------------------------- */
#include <signal.h>

typedef unsigned long pc_sigset_t;
#define sigset_t pc_sigset_t

struct sigaction {
    void (*sa_handler)(int);
    pc_sigset_t sa_mask;
    int sa_flags;
};

#ifndef SA_RESTART
#define SA_RESTART 0x10000000
#endif

static int sigemptyset(pc_sigset_t *s) {
    *s = 0;
    return 0;
}

static int sigaction(int sig, const struct sigaction *act,
                     struct sigaction *old) {
    void (*prev)(int);

    if (act == 0) {
        return 0;
    }
    prev = signal(sig, act->sa_handler);
    if (old != 0) {
        old->sa_handler = prev;
        old->sa_mask = 0;
        old->sa_flags = 0;
    }
    return (prev == SIG_ERR) ? -1 : 0;
}

/* --- 3: setitimer -------------------------------------------------------- */
#ifndef SIGALRM
#define SIGALRM 14 /* never delivered on Windows; number is decorative */
#endif
#define ITIMER_REAL 0

struct pc_itimer_tv { long tv_sec; long tv_usec; };
struct itimerval {
    struct pc_itimer_tv it_interval;
    struct pc_itimer_tv it_value;
};

static int setitimer(int which, const struct itimerval *nv,
                     struct itimerval *ov) {
    (void)which; (void)nv; (void)ov;
    return 0; /* accepted, never fires: pc_dbg's periodic dump is debug-only */
}

#endif /* PC_WIN32_SHIM_H */
