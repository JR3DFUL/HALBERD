/* Internal interfaces of the PC platform layer.
 *
 * Nothing in the game includes this file. It exists so the pieces of the
 * libultra replacement under src/pc/ can talk to each other and to the host
 * backend without exporting anything the game could accidentally depend on.
 *
 * Layering:
 *
 *     game code  ->  libultra API (os*)  ->  pc_* internals  ->  backend
 *
 * The backend is the only part that knows about SDL. See pc_backend.h.
 */
#ifndef PC_PLATFORM_H
#define PC_PLATFORM_H

#include <ultra64.h>

/* -------------------------------------------------------------------------
 * Clock
 *
 * The N64 count register ticks at OS_CPU_COUNTER = 46,875,000 Hz (the 62.5 MHz
 * system clock * 3/4). osGetCount/osGetTime are the game's only notion of time,
 * and OS_CYCLES_TO_USEC in <PR/os_convert.h> assumes exactly that rate, so the
 * host monotonic clock is scaled into it rather than the other way round.
 * ------------------------------------------------------------------------- */
#define PC_COUNTER_HZ 46875000u

/* Free-running 64-bit count since osInitialize, in count-register units. */
u64 pc_count64(void);

/* -------------------------------------------------------------------------
 * Scheduler
 * ------------------------------------------------------------------------- */

/* libultra's own internal names, kept because src/main/fault.c already
 * declares __osGetActiveQueue() and walks the list through OSThread::tlnext. */
extern OSThread *__osRunningThread;
extern OSThread *__osActiveQueue;
extern OSThread *__osRunQueue;

/* Sentinel terminating every thread queue, priority -1 so an ordered insert
 * always finds a stopping point without a NULL test. Same trick, same name and
 * same layout as libultra's, because OSMesgQueue::mtqueue is compared against
 * it in src/pc/os_message.c and the two must agree. */
extern struct __osThreadTail {
    OSThread *next;
    OSPri priority;
} __osThreadTail;

void __osEnqueueThread(OSThread **queue, OSThread *t);
void __osDequeueThread(OSThread **queue, OSThread *t);
OSThread *__osPopThread(OSThread **queue);

/* Block the running thread on *queue and give the CPU up. Returns when the
 * thread is made runnable again. */
void pc_block_on(OSThread **queue);

/* Give the CPU to the highest-priority runnable thread, keeping the caller
 * runnable. No-op if the caller is still the highest priority. */
void pc_yield(void);

/* Make a waiting thread runnable. Does not switch; the caller decides. */
void pc_make_runnable(OSThread *t);

/* True while the platform layer is running host-event delivery, i.e. the
 * cooperative stand-in for "in an interrupt handler". Sends from here must
 * never context-switch, exactly as on N64 where the dispatch happens on the
 * way out of the exception handler rather than inside it. */
extern int pc_in_event_delivery;

/* Called from the idle path when nothing is runnable: pumps host events and
 * delivers any that are due. Must eventually make something runnable or the
 * process is wedged, which is the same property the N64 idle thread has. */
void pc_idle(void);

/* Called at the top of every blocking libultra entry point. Cheap. */
void pc_pump_events(void);

/* -------------------------------------------------------------------------
 * Event table (osSetEventMesg)
 * ------------------------------------------------------------------------- */
int pc_event_fire(OSEvent e); /* returns osSendMesg rc: -1 = queue full, event DROPPED */

/* -------------------------------------------------------------------------
 * Subsystem init, called in order from osInitialize().
 * ------------------------------------------------------------------------- */
void pc_time_init(void);
void pc_sched_init(void);
void pc_vi_init(void);
void pc_cont_init(void);
void pc_pi_init(void);
void pc_ai_init(void);

/* Reserve the RCP MMIO window so direct HW_REG() accesses in game code do not
 * fault. See src/pc/pc_mmio.c. Returns 0 on success. */
int pc_mmio_map(void);

/* Abort if any game-visible address needs more than 32 bits. See the
 * low-memory note in src/pc/pc_mmio.c -- this is what -no-pie buys. */
void pc_check_low_memory(void);

/* Non-zero once SIGINT or SIGTERM has been delivered. See the termination
 * note in src/pc/os_time.c -- nothing else in this process notices them. */
int pc_quit_requested(void);

/* The scripted controller (src/pc/pc_input_script.c) is declared in
 * pc_backend.h instead of here, next to pcb_input_poll: its argument is
 * PCPad, and PCPad is an anonymous-struct typedef that cannot be forward
 * declared. It is not a backend function and that header says so.
 *
 * Per-subsystem host pumping, called from pc_pump_events(). */
void pc_vi_tick(void);
void pc_ai_tick(void);
void pc_pi_tick(void);
void pc_cont_tick(void);
void pc_sp_tick(void);

/* -------------------------------------------------------------------------
 * Diagnostics. PC_TRACE=<subsystem list> in the environment turns these on.
 * ------------------------------------------------------------------------- */
extern unsigned pc_trace_mask;
#define PC_TR_DMA  0x01
#define PC_TR_VI   0x02
#define PC_TR_SP   0x04
#define PC_TR_GFX  0x08
#define PC_TR_CONT 0x10
#define PC_TR_AI   0x20
#define PC_TR_SCHED 0x40

void pc_trace(unsigned bit, const char *fmt, ...);

/* One-time "this is a stub" notice, so a stub can never be mistaken for a
 * working implementation at runtime. Prints once per call site. */
#define PC_STUB_ONCE(msg)                                                     \
    do {                                                                      \
        static int said_;                                                     \
        if (!said_) {                                                         \
            said_ = 1;                                                        \
            pc_stub_notice(__FILE__, __LINE__, msg);                          \
        }                                                                     \
    } while (0)

void pc_stub_notice(const char *file, int line, const char *what);

/* -------------------------------------------------------------------------
 * Boot progress, src/pc/pc_progress.c.
 *
 * Named milestones plus a fatal-signal trap, so "how far did it get" has an
 * answer that a make target can read. The stages are ordered and the recorded
 * one only ever moves forward; see that file for where each name comes from.
 * KIRBY_PC_PROGRESS=1 logs every transition, and the one-line [verdict] is
 * printed unconditionally however the process ends.
 * ------------------------------------------------------------------------- */
/* ONE RUNG PER gGameState THROUGH THE ATTRACT LOOP, and that granularity is
 * load-bearing rather than tidy. A single "attract-demo" rung covering states
 * 4, 6 and 8 reported PASS both before and after the fix that carried the
 * port from dying inside demo 2 to completing all three demos and wrapping
 * back to the opening movie -- a ratchet that cannot see the largest single
 * improvement the port has had is not a ratchet. */
#define PC_STAGE_START        0
#define PC_STAGE_OSINIT       1
#define PC_STAGE_THREAD       2
#define PC_STAGE_RETRACE      3
#define PC_STAGE_GFXTASK      4
#define PC_STAGE_LOGOS        5   /* gGameState 1  */
#define PC_STAGE_OPENING      6   /* gGameState 2  */
#define PC_STAGE_TITLE        7   /* gGameState 3  */
#define PC_STAGE_DEMO1        8   /* gGameState 4  */
#define PC_STAGE_TITLE2       9   /* gGameState 5  */
#define PC_STAGE_DEMO2       10  /* gGameState 6  */
#define PC_STAGE_TITLE3      11  /* gGameState 7  */
#define PC_STAGE_DEMO3       12  /* gGameState 8  */
#define PC_STAGE_TITLE4      13  /* gGameState 9  */
/* game_tick()'s case 9 sets gGameState back to 2, so the unattended cycle has
 * closed and the port will now repeat it forever. Without synthetic input
 * this is the ceiling: every state above needs a button. */
#define PC_STAGE_ATTRACT_LOOP 14
/* Everything from here needs a button. */
#define PC_STAGE_MENU        15  /* gGameState 10 -- file select */
#define PC_STAGE_WORLDSELECT 16  /* gGameState 11 */
#define PC_STAGE_LEVELSELECT 17  /* gGameState 12 */
/* gGameState 14 is not a menu. src/ovl1/game.c's func_800A3408 sets 0xE and
 * runs func_800A3150(4) in a loop for the world-1 level-1 opening cutscene
 * (overlay 18), so reaching it means the port has left the menus and is
 * running a real scene. */
#define PC_STAGE_CUTSCENE    18  /* gGameState 14 */
#define PC_STAGE_GAMEPLAY    19  /* gGameState 15 */

void pc_progress_init(void);
void pc_progress_mark(int stage);
void pc_progress_tick(void);
void pc_progress_report(const char *outcome);

/* Temporary hang instrumentation, src/pc/pc_dbg.c. KIRBY_PC_PUMPDBG=1. */
void pc_dbg_init(void);
extern unsigned long pc_dbg_pump_call, pc_dbg_pump_reent, pc_dbg_pump_nosched,
    pc_dbg_pump_intsoff, pc_dbg_pump_body, pc_dbg_pump_done, pc_dbg_vi_call,
    pc_dbg_vi_nostart, pc_dbg_vi_loop, pc_dbg_vi_retrace, pc_dbg_yield_call,
    pc_dbg_dispatch_call, pc_dbg_idle_call, pc_dbg_now_lo, pc_dbg_next_lo;

/* -------------------------------------------------------------------------
 * Display list tracing (src/pc/gfx_trace.c)
 * ------------------------------------------------------------------------- */
void pc_gfx_trace_task(OSTask *task);

/* -------------------------------------------------------------------------
 * Synthetic display lists (src/pc/gfx_selftest.c). KIRBY_PC_GFXTEST=1.
 * The renderer path has no other exercise until auThreadMain is decompiled;
 * see the header of that file.
 * ------------------------------------------------------------------------- */
int pc_gfx_selftest_enabled(void);
void pc_gfx_selftest_frame(void);

/* -------------------------------------------------------------------------
 * Cartridge / overlay interception (src/pc/os_pi.c, src/pc/pc_overlay.c)
 * ------------------------------------------------------------------------- */

/* Open baserom.us.z64 (or $KIRBY_ROM). Returns 0 on success. */
int pc_rom_open(void);

/* Read from the cartridge image. Returns bytes read, 0 if no ROM is loaded. */
u32 pc_rom_read(u32 romOffset, void *dst, u32 size);

/* Base pointer of the mapped cartridge image, or NULL. Byte order is exactly
 * the file's, i.e. big-endian; callers wanting scalars must swap. */
const u8 *pc_rom_base(u32 *sizeOut);

/* True if [ram, ram+size) lands inside a resident overlay's code/data image.
 * A DMA into such a range is a MIPS overlay load and must not be performed --
 * see docs/PC_PORT_SURFACE.md, "Overlays cannot be emulated, only
 * intercepted". Returns the overlay index, or -1. */
int pc_overlay_covering(const void *ram, u32 size);

#endif /* PC_PLATFORM_H */
