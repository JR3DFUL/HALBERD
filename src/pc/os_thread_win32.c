/* Threads and the scheduler -- Windows arm.
 *
 * This is src/pc/os_thread.c with the context-switch primitive swapped:
 * ucontext (makecontext/swapcontext) does not exist on Windows, and Win32
 * fibers are the exact same shape -- cooperatively scheduled register sets
 * with their own stacks, switched only where the code says so. The design
 * note at the top of os_thread.c (strict priority, no parallelism, no
 * preemption between equal priorities; the idle thread is never dispatched)
 * applies to this file unchanged and is not repeated here.
 *
 * THE MAPPING, primitive by primitive:
 *
 *   ucontext                              fiber
 *   ------------------------------------  -----------------------------------
 *   ucontext_t uc (in PCThread)           void *fiber (in PCThread)
 *   sBootCtx (the context that first      sBootFiber =
 *     called osCreateThread, i.e.           ConvertThreadToFiber(NULL),
 *     cboot's)                              done lazily in pc_sched_init()
 *   getcontext + makecontext(trampoline)  CreateFiberEx(commit, reserve,
 *     onto a malloc'd stack                 0, trampoline, slot) -- the OS
 *                                           allocates the stack
 *   swapcontext(&from->uc, &np->uc)       SwitchToFiber(np->fiber) -- the
 *                                           outgoing register set is saved
 *                                           into the CURRENT fiber
 *                                           automatically, which is why
 *                                           sCurCtx/from bookkeeping gets
 *                                           simpler here (see below)
 *   uc_link = &sBootCtx                   nothing -- the trampoline never
 *                                           returns (a fiber proc that
 *                                           returns exits the whole thread,
 *                                           so trampoline ends in a dispatch
 *                                           loop, same as osSetThreadPri's
 *                                           park)
 *
 * WHY sCurCtx SURVIVES ANYWAY. os_thread.c's long comment explains that
 * "which context are we physically on" and "which thread is running" are
 * different questions, and that swapcontext must save the outgoing registers
 * into the slot of the context being LEFT, not into whatever
 * __osRunningThread says. Fibers make the save-side automatic
 * (SwitchToFiber saves into the fiber being left, by definition, and
 * GetCurrentFiber() always answers the physical question), so the
 * lost-context bug that comment documents CANNOT happen here. sCurCtx is
 * kept regardless, for the np == from short-circuit in dispatch() --
 * switching a fiber to itself is documented as undefined behaviour and must
 * be avoided, which is the same rule swapcontext had.
 *
 * STACKS. Same policy as os_thread.c: the game's 1 KB MIPS stacks are
 * ignored and every thread gets a host stack. Here the stack belongs to the
 * fiber (CreateFiberEx allocates it), so PCThread has no ->stack pointer to
 * malloc or reuse. PC_STACK_SIZE becomes the RESERVE size; commit starts at
 * 64 KB and the OS grows it, which is strictly better than committing 512 KB
 * up front times 64 slots.
 *
 * DESTRUCTION. DeleteFiber(GetCurrentFiber()) calls ExitThread -- fatal for
 * the whole process here, since every game thread lives on the one host
 * thread. So osDestroyThread never deletes the fiber of the thread it is
 * standing on (the inUse=1 keep-alive below, same as os_thread.c keeping the
 * stack), and fiber deletion happens only on REUSE: osCreateThread on an
 * OSThread whose slot still holds a fiber deletes the old one -- at that
 * point nothing can be standing on it -- and creates a fresh one, which is
 * exactly the makecontext-again path in the ucontext version.
 *
 * TIME. pc_idle()'s 0.5 ms nanosleep becomes Sleep(1) (there is no reliable
 * sub-millisecond sleep on Windows without messing with timeBeginPeriod;
 * 1 ms at 60 Hz retrace granularity is fine). The winpthreads flavour of
 * mingw-w64 does provide nanosleep, but this file deliberately does not
 * depend on which flavour builds it.
 */
#ifdef _WIN32

#include <ultra64.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "pc/pc_platform.h"
#include "pc/pc_backend.h"

#define PC_MAX_THREADS      64
#define PC_STACK_RESERVE    (512 * 1024)
#define PC_STACK_COMMIT     (64 * 1024)

/* Per-thread host state. Same side-table reasoning as os_thread.c: OSThread's
 * layout is fixed by <PR/os_thread.h> and shared with game data, so host
 * state cannot live inside it. */
typedef struct {
    OSThread *os;
    void *fiber;                /* CreateFiberEx result; owns its stack */
    void (*entry)(void *);
    void *arg;
    int inUse;
    int finished;
} PCThread;

static PCThread sThreads[PC_MAX_THREADS];

OSThread *__osRunningThread;
OSThread *__osActiveQueue;
OSThread *__osRunQueue;

struct __osThreadTail __osThreadTail = { NULL, -1 };

int pc_in_event_delivery;

/* The fiber to return to when every thread has exited: the one that first
 * called osCreateThread/osStartThread, i.e. cboot's. NULL until
 * pc_sched_init converts the host thread. */
static void *sBootFiber;
static int sSchedReady;

/* Which PCThread's fiber the CPU is physically standing on; NULL means the
 * boot fiber. Kept for the np == from short-circuit -- SwitchToFiber to the
 * current fiber is undefined behaviour, same as swapcontext to self. */
static PCThread *sCurCtx;

extern int pc_ints_enabled(void);

/* ------------------------------------------------------------- side table */

static PCThread *slot_of(OSThread *t) {
    int i;

    for (i = 0; i < PC_MAX_THREADS; i++) {
        if (sThreads[i].inUse && sThreads[i].os == t) {
            return &sThreads[i];
        }
    }
    return NULL;
}

static PCThread *slot_alloc(OSThread *t) {
    int i;

    for (i = 0; i < PC_MAX_THREADS; i++) {
        if (!sThreads[i].inUse) {
            /* NOTE: unlike os_thread.c this must NOT memset the slot --
             * a stale ->fiber from a destroyed-then-recreated thread is a
             * live kernel object that osCreateThread below reclaims. A
             * memset here would leak it. Fields are assigned singly. */
            sThreads[i].inUse = 1;
            sThreads[i].os = t;
            sThreads[i].finished = 0;
            return &sThreads[i];
        }
    }
    fprintf(stderr, "[pc] osCreateThread: more than %d threads\n",
            PC_MAX_THREADS);
    abort();
    return NULL;
}

/* --------------------------------------------------------- queue plumbing */

/* Identical to os_thread.c -- pure pointer surgery, nothing host-specific.
 * Duplicated rather than shared because this file replaces that one whole:
 * the Windows build compiles os_thread_win32.c and drops os_thread.c, the
 * Linux build does the reverse (this file is empty without _WIN32). */
void __osEnqueueThread(OSThread **queue, OSThread *t) {
    OSThread *cur = *queue;
    OSThread *prev = NULL;
    OSPri pri = t->priority;

    while (cur != NULL && pri <= cur->priority) {
        prev = cur;
        cur = cur->next;
    }
    if (prev == NULL) {
        t->next = *queue;
        *queue = t;
    } else {
        t->next = prev->next;
        prev->next = t;
    }
    t->queue = queue;
}

void __osDequeueThread(OSThread **queue, OSThread *t) {
    OSThread *cur = *queue;
    OSThread *prev = NULL;

    while (cur != NULL) {
        if (cur == t) {
            if (prev == NULL) {
                *queue = cur->next;
            } else {
                prev->next = cur->next;
            }
            t->next = NULL;
            t->queue = NULL;
            return;
        }
        prev = cur;
        cur = cur->next;
    }
}

OSThread *__osPopThread(OSThread **queue) {
    OSThread *t = *queue;

    if (t == NULL || t == (OSThread *)&__osThreadTail) {
        return NULL;
    }
    *queue = t->next;
    t->next = NULL;
    t->queue = NULL;
    return t;
}

OSThread *__osGetActiveQueue(void) {
    return __osActiveQueue;
}

OSThread *__osGetCurrFaultedThread(void) {
    return NULL;
}

/* --------------------------------------------------------------- dispatch */

static OSThread *runnable_head(void) {
    OSThread *t = __osRunQueue;

    if (t == (OSThread *)&__osThreadTail) {
        return NULL;
    }
    return t;
}

static int only_idle_runnable(void) {
    OSThread *t = runnable_head();

    return t == NULL || t->priority <= OS_PRIORITY_IDLE;
}

/* The platform's stand-in for thread1_idle's `while (1);`. */
void pc_idle(void) {
    pc_dbg_idle_call++;
    pc_pump_events();
    if (!only_idle_runnable()) {
        return;
    }
    if (pc_quit_requested()) {
        fprintf(stderr, "[pc] interrupted\n");
        fflush(NULL);
        _exit(0); /* not exit() -- see the termination note in os_time.c */
    }
    if (!pcb_alive()) {
        fprintf(stderr, "[pc] host asked to quit\n");
        fflush(NULL);
        pcb_video_shutdown();
        _exit(0);
    }
    /* 1 ms is the finest granularity Sleep gives without timeBeginPeriod
     * games; fine enough for a 60 Hz retrace, same reasoning as the 0.5 ms
     * nanosleep on Linux. */
    Sleep(1);
}

static void CALLBACK trampoline(void *param);

static void dispatch(void) {
    OSThread *next;
    PCThread *from = sCurCtx;
    PCThread *np;

    pc_dbg_dispatch_call++;
    for (;;) {
        next = runnable_head();
        if (next != NULL && next->priority > OS_PRIORITY_IDLE) {
            break;
        }
        pc_idle();
    }

    __osPopThread(&__osRunQueue);
    next->state = OS_STATE_RUNNING;
    __osRunningThread = next;

    np = slot_of(next);
    if (np == from) {
        /* Already standing on that fiber -- SwitchToFiber to the current
         * fiber is UB, and there is nothing to switch anyway. */
        return;
    }

    pc_trace(PC_TR_SCHED, "[sched] %s -> id %d pri %d\n",
             from ? "switch" : "enter", (int)next->id, (int)next->priority);

    sCurCtx = np;
    /* The outgoing register set is saved into the fiber being left --
     * automatically, which is the one place fibers are SIMPLER than
     * ucontext (see the header note on sCurCtx). */
    SwitchToFiber(np->fiber);

    /* Reached again when something switches back to `from`. Restore the
     * bookkeeping to match the physical truth. */
    sCurCtx = from;
}

void pc_make_runnable(OSThread *t) {
    if (t == NULL || t->state == OS_STATE_RUNNING) {
        return;
    }
    t->state = OS_STATE_RUNNABLE;
    __osEnqueueThread(&__osRunQueue, t);
}

void pc_yield(void) {
    OSThread *head;
    OSThread *me = __osRunningThread;

    if (me == NULL) {
        return;
    }
    head = runnable_head();
    if (head == NULL || head->priority <= me->priority) {
        return; /* still the best candidate -- property (3) */
    }
    me->state = OS_STATE_RUNNABLE;
    __osEnqueueThread(&__osRunQueue, me);
    dispatch();
}

void pc_block_on(OSThread **queue) {
    OSThread *me = __osRunningThread;

    if (me == NULL) {
        /* The boot fiber blocked before any thread exists. Spin the host
         * idle loop; there is nothing to switch to. */
        while (*queue != NULL) {
            pc_idle();
            return;
        }
        return;
    }
    me->state = OS_STATE_WAITING;
    __osEnqueueThread(queue, me);
    __osRunningThread = NULL;
    dispatch();
}

/* Called at the top of the blocking libultra entry points. Identical to
 * os_thread.c minus the Linux-only debug clock dump. */
void pc_pump_events(void) {
    static int reentrant;

    pc_dbg_pump_call++;
    if (reentrant || !sSchedReady) {
        if (reentrant) {
            pc_dbg_pump_reent++;
        } else {
            pc_dbg_pump_nosched++;
        }
        return;
    }
    /* osSetIntMask(OS_IM_NONE) means "nothing may run here". Honour it: the
     * audio library relies on it to keep its player list consistent. */
    if (!pc_ints_enabled()) {
        pc_dbg_pump_intsoff++;
        return;
    }
    pc_dbg_pump_body++;
    if (pc_quit_requested()) {
        fprintf(stderr, "[pc] interrupted\n");
        fflush(NULL);
        _exit(0);
    }
    reentrant = 1;
    pc_in_event_delivery = 1;
    pcb_pump();
    pc_vi_tick();
    pc_ai_tick();
    pc_pi_tick();
    pc_cont_tick();
    pc_sp_tick();
    pc_in_event_delivery = 0;
    reentrant = 0;
    pc_dbg_pump_done++;
}

/* -------------------------------------------------------------- lifecycle */

static void CALLBACK trampoline(void *param) {
    PCThread *me = (PCThread *)param;
    OSThread *os = me->os;

    me->entry(me->arg);

    /* A libultra thread entry that returns is undefined on N64. Treat it as
     * osDestroyThread(self). CRITICAL fiber difference: RETURNING from a
     * fiber procedure exits the host THREAD (and with it every game thread),
     * so this must never return -- it parks in a dispatch loop instead,
     * the same shape as osSetThreadPri's idle park. */
    me->finished = 1;
    os->state = OS_STATE_STOPPED;
    __osRunningThread = NULL;
    for (;;) {
        dispatch();
    }
}

void pc_sched_init(void) {
    if (sSchedReady) {
        return;
    }
    __osRunQueue = (OSThread *)&__osThreadTail;
    __osActiveQueue = (OSThread *)&__osThreadTail;
    __osRunningThread = NULL;

    /* Adopt the host thread as the boot fiber. Must happen before the first
     * SwitchToFiber anywhere; osCreateThread calls here first, so it does.
     * IsThreadAFiber guards the (harmless today, fatal under a future
     * embedder) double conversion. */
    if (!IsThreadAFiber()) {
        sBootFiber = ConvertThreadToFiber(NULL);
    } else {
        sBootFiber = GetCurrentFiber();
    }
    if (sBootFiber == NULL) {
        fprintf(stderr, "[pc] ConvertThreadToFiber failed (%lu)\n",
                (unsigned long)GetLastError());
        abort();
    }
    sSchedReady = 1;
}

void osCreateThread(OSThread *t, OSId id, void (*entry)(void *), void *arg,
                    void *sp, OSPri pri) {
    PCThread *p;

    pc_sched_init();
    (void)sp; /* deliberately ignored -- see the STACKS note in os_thread.c */

    p = slot_of(t);
    if (p == NULL) {
        p = slot_alloc(t);
    }
    p->entry = entry;
    p->arg = arg;
    p->finished = 0;

    /* Recreate-on-reuse: a slot that already holds a fiber is a thread the
     * game destroyed and is now recreating (its fixed set of static
     * OSThreads). Nothing can be standing on that fiber -- osDestroyThread
     * of the running thread keeps its slot inUse and dispatch never returns
     * to it -- so deleting it here is safe, and is the analogue of running
     * makecontext again over the reused stack. */
    if (p->fiber != NULL && p != sCurCtx) {
        DeleteFiber(p->fiber);
        p->fiber = NULL;
    }
    if (p->fiber == NULL) {
        p->fiber = CreateFiberEx(PC_STACK_COMMIT, PC_STACK_RESERVE, 0,
                                 trampoline, p);
        if (p->fiber == NULL) {
            fprintf(stderr, "[pc] CreateFiberEx failed (%lu)\n",
                    (unsigned long)GetLastError());
            abort();
        }
    }

    t->id = id;
    t->priority = pri;
    t->next = NULL;
    t->queue = NULL;
    t->state = OS_STATE_STOPPED;
    t->flags = 0;
    t->fp = 0;
    t->thprof = NULL;

    /* Link onto the active list, which fault.c walks. */
    t->tlnext = (__osActiveQueue == (OSThread *)&__osThreadTail)
                    ? NULL
                    : __osActiveQueue;
    __osActiveQueue = t;
}

void osStartThread(OSThread *t) {
    pc_sched_init();

    switch (t->state) {
        case OS_STATE_WAITING:
            if (t->queue != NULL) {
                __osDequeueThread(t->queue, t);
            }
            break;
        case OS_STATE_RUNNING:
            return;
        default:
            break;
    }
    t->state = OS_STATE_RUNNABLE;
    __osEnqueueThread(&__osRunQueue, t);

    /* Hand over immediately if the new thread outranks the caller. Kirby
     * 64's boot depends on this: cboot() starts the idle thread at priority
     * 127 from a context with no priority at all, and never comes back. */
    if (__osRunningThread == NULL || t->priority > __osRunningThread->priority) {
        if (__osRunningThread != NULL) {
            __osRunningThread->state = OS_STATE_RUNNABLE;
            __osEnqueueThread(&__osRunQueue, __osRunningThread);
        }
        dispatch();
    }
}

void osStopThread(OSThread *t) {
    if (t == NULL) {
        t = __osRunningThread;
    }
    if (t == NULL) {
        return;
    }
    if (t->queue != NULL) {
        __osDequeueThread(t->queue, t);
    }
    t->state = OS_STATE_STOPPED;
    if (t == __osRunningThread) {
        __osRunningThread = NULL;
        dispatch();
    }
}

void osDestroyThread(OSThread *t) {
    PCThread *p;
    OSThread **link;

    if (t == NULL) {
        t = __osRunningThread;
    }
    if (t == NULL) {
        return;
    }
    if (t->queue != NULL) {
        __osDequeueThread(t->queue, t);
    }

    /* Unlink from the active list. */
    if (__osActiveQueue == t) {
        __osActiveQueue = (t->tlnext != NULL) ? t->tlnext
                                              : (OSThread *)&__osThreadTail;
    } else {
        link = &__osActiveQueue;
        while (*link != NULL && *link != (OSThread *)&__osThreadTail) {
            if ((*link)->tlnext == t) {
                (*link)->tlnext = t->tlnext;
                break;
            }
            link = &(*link)->tlnext;
        }
    }

    p = slot_of(t);
    if (p != NULL) {
        /* The fiber is NOT deleted here: if the destroyed thread is the
         * caller we are standing on it (DeleteFiber(current) is ExitThread,
         * fatal), and even for another thread the slot-reuse path in
         * osCreateThread reclaims it more simply. Mirror of os_thread.c
         * keeping the malloc'd stack. */
        p->inUse = (t == __osRunningThread) ? 1 : 0;
    }
    t->state = OS_STATE_STOPPED;

    if (t == __osRunningThread) {
        __osRunningThread = NULL;
        dispatch();
    }
}

OSId osGetThreadId(OSThread *t) {
    if (t == NULL) {
        t = __osRunningThread;
    }
    return (t != NULL) ? t->id : 0;
}

OSPri osGetThreadPri(OSThread *t) {
    if (t == NULL) {
        t = __osRunningThread;
    }
    return (t != NULL) ? t->priority : -1;
}

void osSetThreadPri(OSThread *t, OSPri pri) {
    if (t == NULL) {
        t = __osRunningThread;
    }
    if (t == NULL) {
        return;
    }
    if (t->priority == pri) {
        return;
    }
    t->priority = pri;

    if (t->state != OS_STATE_RUNNING && t->queue != NULL) {
        OSThread **q = t->queue;

        __osDequeueThread(q, t);
        __osEnqueueThread(q, t);
    }

    /* Lowering our own priority may hand the CPU to someone else. This is
     * the path thread1_idle takes into its spin loop; at OS_PRIORITY_IDLE it
     * is parked here forever -- see the design note in os_thread.c. */
    if (t == __osRunningThread) {
        pc_yield();
        if (t->priority <= OS_PRIORITY_IDLE) {
            t->state = OS_STATE_WAITING;
            __osRunningThread = NULL;
            for (;;) {
                dispatch();
            }
        }
    }
}

void osYieldThread(void) {
    OSThread *me = __osRunningThread;

    pc_dbg_yield_call++;
    if (me == NULL) {
        return;
    }
    me->state = OS_STATE_RUNNABLE;
    __osEnqueueThread(&__osRunQueue, me);
    dispatch();
}

#endif /* _WIN32 */
