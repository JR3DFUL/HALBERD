/* The libultraship backend: window, renderer, input and audio.
 *
 * TARGETS THE JRICKEY FORK of libultraship (the BattleShip/SSB64 port's LUS,
 * $LUS_ROOT, see tools/pc/lus_flags.sh), not upstream. Two things changed with the
 * fork and both simplified this file:
 *
 *   1. Context boot is the fork's STAGED INIT API. The old hand-assembled
 *      children-registry boot (CreateInstance + GetChildren().Add(...) in
 *      dependency order) is gone; the fork exposes
 *      Context::CreateUninitializedInstance() plus one InitX() per component,
 *      each of which constructs the component itself and returns false on
 *      failure. The sequence below mirrors BattleShip's PortInitImpl
 *      (port/port.cpp:644) minus its SSB-specific parts (hires packs, mods,
 *      cheats, widescreen, renderdoc, first-run wizard).
 *
 *   2. The C bridge (WindowIsRunning, AudioPlayerPlayFrame, ...) resolves
 *      through Context::GetInstance() in the fork, so the old cache-setter
 *      calls (ResourceSetResourceManager, WindowSetWindowComponent,
 *      GfxSetFast3dWindow, ...) no longer exist and are no longer needed.
 *
 * =====================================================================
 * HOW LUS'S MAIN LOOP AND THE GAME'S SCHEDULER WERE RECONCILED
 *
 * This is the design question the integration turns on, so it is answered
 * here rather than in a doc that will drift away from the code.
 * =====================================================================
 *
 * The premise everyone starts from is that these two things fight:
 *
 *   * libultraship expects to own the main loop. Every existing LUS port has
 *     a `while (WindowIsRunning()) { StartFrame(); GameFrame(); EndFrame(); }`
 *     somewhere, and the SSB64/BattleShip method is explicitly to *collapse*
 *     the N64 threads into that loop with graphics, audio and input called at
 *     fixed points.
 *   * src/pc/os_thread.c also owns it: cboot() starts the idle thread and
 *     never returns, and from then on the ucontext scheduler decides what
 *     runs.
 *
 * THEY DO NOT ACTUALLY FIGHT, and the reason is the single most useful
 * property of the platform layer that was already here: **the cooperative
 * scheduler runs every game thread on ONE host thread.** os_thread.c chose
 * ucontext over pthreads because the game has no locks anywhere and relies on
 * osSetIntMask as an assertion that nothing else is executing. That decision
 * was made for correctness of the game's own data, but it is also exactly
 * what makes LUS embeddable here:
 *
 *     - an OpenGL context is bound to a thread, and there is only one thread;
 *     - SDL requires event pumping on the thread that created the window, and
 *       there is only one thread;
 *     - Fast3D's Interpreter has process-global state (g_exec_stack), and
 *       there is only one thread.
 *
 * So no ownership question arises. LUS calls happen wherever the game reaches
 * them, and "wherever" is always the same OS thread.
 *
 * WHAT REPLACES THE MAIN LOOP. A LUS main loop is three things happening in a
 * fixed order once per frame. The game already emits all three, as events, at
 * points that mean the same thing:
 *
 *     LUS main loop            this port
 *     -----------------------  -------------------------------------------
 *     HandleEvents()           pcb_pump(), called from pc_pump_events(),
 *                              which every blocking libultra call goes
 *                              through. Rate-limited below; see sLastPumpNs.
 *     game logic               the game threads, dispatched by priority
 *     Draw + Present           pcb_gfx_run(), called from osSpTaskStartGo()
 *                              when the game hands the RSP a graphics task
 *     "no frame this tick"     pcb_frame_end() at VI retrace, which runs the
 *                              GUI alone if no display list arrived
 *
 * The frame boundary is therefore *the game's own*, not a wall-clock timer:
 * a frame exists exactly when the game submits a display list for it. That is
 * strictly better than a timer, because sched.c's framebuffer recycling is
 * driven by the same event and the two can never disagree.
 *
 * THE ONE PLACE THIS IS STILL WRONG, stated plainly: Fast3D's Interpreter::Run
 * clears the framebuffer on entry, so it is one-call-per-frame by
 * construction. If Kirby 64 ever submits two graphics tasks for a single
 * displayed frame -- and its scheduler can, it has a yield/resume path for
 * exactly that -- the second one erases the first and both get presented.
 * This is detected and warned about once (sMultiTaskFrame) rather than
 * silently producing a flickering game.
 *
 * =====================================================================
 * WHY THIS FILE DOES NOT #include "pc/pc_backend.h"
 * =====================================================================
 *
 * pc_backend.h includes <PR/ultratypes.h>, and libultraship ships its own
 * libultra headers: both define u8..f64, size_t, uintptr_t, OSMesgQueue,
 * OSContPad and Gfx, with different definitions. Including both in one
 * translation unit is not a warning, it is a wall of redefinition errors.
 *
 * So the C surface is re-declared here from <cstdint>, which is exact --
 * ultratypes' u16 IS uint16_t, its s8 IS int8_t -- and a compile-time check
 * on the PCPad layout is kept below so the two cannot drift apart silently.
 */
#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "libultraship/libultraship.h"
#include <SDL2/SDL.h>
#include "fast/Fast3dWindow.h"
#include "libultraship/controller/controldeck/ControlDeck.h"

/* --------------------------------------------------------- the C surface */

/* Mirror of PCPad in src/pc/pc_backend.h. Kept in step by the static_assert
 * below plus the field-by-field copy in pcb_input_poll. */
typedef struct {
    uint16_t button;
    int8_t stick_x;
    int8_t stick_y;
    uint8_t present;
} PCPad;

static_assert(sizeof(PCPad) == 6, "PCPad layout drifted from pc_backend.h");

extern "C" {
void pcb_video_init(int width, int height);
void pcb_video_present(const void* fb, int width, int height, int fmt);
void pcb_video_shutdown(void);
int pcb_has_renderer(void);
void pcb_frame_begin(void);
void pcb_frame_end(void);
void pcb_gfx_set_native_ucodes(const void* f3dex2, const void* s2dex);
void pcb_gfx_set_ucode(int s2dex);
void pcb_gfx_run(const void* displayList);
int pcb_gfx_stats(unsigned* framesDrawn, unsigned* framesSampled,
                  unsigned* framesNonBlank, unsigned* distinct,
                  unsigned long long* lastHash);
int pcb_alive(void);
void pcb_pump(void);
void pcb_input_poll(PCPad* pads, int n);
void pcb_input_rumble(int port, int on);
void pcb_audio_init(int freq);
void pcb_audio_set_freq(int freq);
void pcb_audio_queue(const void* samples, uint32_t bytes);
uint32_t pcb_audio_queued(void);

/* From src/pc/gfx_trace.c -- the GBI ring, armed here because this is the
 * only translation unit that knows the interpreter is up. */
void pc_gbi_ring_init(void);

/* From src/pc/os_time.c -- the platform layer's own trace switch, so LUS
 * diagnostics obey the same PC_TRACE= variable as everything else. */
void pc_trace(unsigned bit, const char* fmt, ...);
}

#define PC_TR_VI 0x02
#define PC_TR_GFX 0x08
#define PC_TR_AI 0x20

/* ------------------------------------------------------------------ state */

static std::shared_ptr<Ship::Context> sContext;
static std::shared_ptr<Fast::Fast3dWindow> sWindow;
static std::shared_ptr<LUS::ControlDeck> sControlDeck;
static bool sInitTried;
static bool sInitOk;

/* THE PAD BUFFER BELONGS TO THE CALLER, NOT TO THE CONTROL DECK.
 *
 * LUS::ControlDeck::GetPads() returns mPads, and mPads is only ever assigned
 * inside WriteToOSContPad(pad) -- it is a cached copy of whatever pointer the
 * game last handed in, not storage the deck owns. Asking for the pads before
 * ever writing to them therefore returns nullptr forever: GetPads() ->
 * nullptr -> skip the write -> GetPads() still nullptr. Every port then
 * reports "no controller", the game's boot check in func_800A3058 finds
 * contChannelMap all -1, and it enters scene 4 -- the "no controllers
 * connected" error screen -- instead of the title sequence.
 *
 * The buffer lives here. WriteToPad() fills it; GetPads() is not used at all.
 * NOTE the element type is the FORK's OSContPad (0x24 bytes, gyro and right
 * stick included), not the game's 6-byte one -- only the shared leading
 * fields are copied out. */
static OSContPad sLusPads[MAXCONTROLLERS];

/* ControlDeck::Init() takes a pointer to the game's connected-port bitmask
 * and ORs in bit 0 unconditionally (a keyboard is always a valid port 1
 * device). The port keeps its own byte because src/pc/os_cont.c derives
 * OSContStatus from PCPad.present rather than from a bitmask. */
static uint8_t sControllerBits;

static int sFramesDrawn;
static int sTasksThisFrame;
static bool sMultiTaskFrame;

/* ------------------------------------------------- did it draw anything?
 *
 * See the long note over pcb_gfx_stats in src/pc/pc_backend.h for why this
 * exists. Two independent facts are kept, because they answer two different
 * questions and only one of them is free:
 *
 *   sFramesDrawn    DrawAndRunGraphicsCommands returned true, i.e. Fast3D
 *                   ran the display list through the rasteriser and swapped.
 *                   That is the pixel evidence, and it costs nothing.
 *   the sample set  a readback of the presented image. This answers "was the
 *                   image blank" and "did it CHANGE", which framesDrawn
 *                   cannot -- a wedged game still swaps the same picture
 *                   forever. It stalls the GPU, so it is opt-in through
 *                   KIRBY_PC_FRAMEHASH=<every-nth-frame>.
 *
 * glReadPixels is resolved with dlsym rather than linked. The backend must
 * not grow a hard dependency on a GL loader for a diagnostic: on a build
 * where the symbol is not in the process (a future Metal/DirectX path, or a
 * GL loaded entirely through function pointers) the lookup simply fails and
 * the sample counters stay 0, which reports "not sampled" instead of lying.
 *
 * GL_FRONT, not GL_BACK: Fast3D's EndFrame has already swapped by the time
 * pcb_frame_end runs, so the finished image is in the front buffer and the
 * back buffer holds undefined contents. */
#define PCB_GL_FRONT 0x0404
#define PCB_GL_BACK 0x0405
#define PCB_GL_RGBA 0x1908
#define PCB_GL_UNSIGNED_BYTE 0x1401

typedef void (*pcb_glReadPixels_t)(int, int, int, int, unsigned, unsigned, void*);
typedef void (*pcb_glReadBuffer_t)(unsigned);

static unsigned sFramesSampled;
static unsigned sFramesNonBlank;
static unsigned sDistinct;
static unsigned long long sLastHash;
static int sSampleEvery;          /* 0 = sampling off */
static int sSampleResolved;
static unsigned sSampleBuf = PCB_GL_BACK;

static pcb_glReadPixels_t sGlReadPixels;
static pcb_glReadBuffer_t sGlReadBuffer;

/* A 64x64 corner is enough to tell blank from drawn and frame N from frame
 * N+1, and it is ~1000x less readback than a full 640x480. The origin is the
 * middle of the window rather than (0,0): the N64 image is letterboxed by
 * Fast3D on a resized window, so the corners can legitimately be the clear
 * colour on a frame that drew perfectly well. */
#define PCB_SAMPLE_DIM 64

static void pcb_sample_frame(void) {
    static unsigned char px[PCB_SAMPLE_DIM * PCB_SAMPLE_DIM * 4];
    unsigned long long h = 1469598103934665603ull;   /* FNV-1a 64 offset */
    size_t i;
    int nonblank = 0;
    int w = 0;
    int hgt = 0;

    if (!sSampleResolved) {
        sSampleResolved = 1;
        sGlReadPixels = (pcb_glReadPixels_t)dlsym(RTLD_DEFAULT, "glReadPixels");
        sGlReadBuffer = (pcb_glReadBuffer_t)dlsym(RTLD_DEFAULT, "glReadBuffer");
        if (sGlReadPixels == nullptr) {
            fprintf(stderr, "[lus] KIRBY_PC_FRAMEHASH set but glReadPixels is "
                            "not in this process; frames will not be sampled.\n");
        }
    }
    if (sGlReadPixels == nullptr) {
        return;
    }

    w = (int)sWindow->GetWidth();
    hgt = (int)sWindow->GetHeight();
    if (w < PCB_SAMPLE_DIM || hgt < PCB_SAMPLE_DIM) {
        w = 640;
        hgt = 480;
    }
    memset(px, 0, sizeof(px));
    if (sGlReadBuffer != nullptr) {
        sGlReadBuffer(sSampleBuf);
    }
    sGlReadPixels((w - PCB_SAMPLE_DIM) / 2, (hgt - PCB_SAMPLE_DIM) / 2,
                  PCB_SAMPLE_DIM, PCB_SAMPLE_DIM, PCB_GL_RGBA,
                  PCB_GL_UNSIGNED_BYTE, px);

    for (i = 0; i < sizeof(px); i++) {
        h ^= px[i];
        h *= 1099511628211ull;
    }
    /* "Not the clear colour" is spelled as "not all one value" so that this
     * keeps working if the clear colour is ever changed from black. A frame
     * of uniform anything is a frame with nothing on it. */
    for (i = 4; i < sizeof(px); i += 4) {
        if (px[i] != px[0] || px[i + 1] != px[1] || px[i + 2] != px[2]) {
            nonblank = 1;
            break;
        }
    }

    sFramesSampled++;
    if (nonblank) {
        sFramesNonBlank++;
    }
    if (sFramesSampled == 1 || h != sLastHash) {
        sDistinct++;
    }
    sLastHash = h;
}

/* pc_pump_events() is called at the top of every blocking libultra entry
 * point, which in this game is thousands of times a second. SDL_PollEvent is
 * cheap but not free, and more importantly LUS's HandleEvents can resize the
 * renderer. Once per millisecond is far finer than any input device and
 * removes the call from the hot path. */
static uint64_t sLastPumpNs;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* --------------------------------------------------------------- start-up */

/* Where resources come from. TWO paths, and the split is not arbitrary.
 *
 * LUS will not start without at least one archive: ArchiveManager marks
 * itself initialised only if it found one, and InitResourceManager reports
 * failure otherwise (see allowEmptyPaths below). It is not only the game's
 * assets that live there either -- FAST3D ITSELF LOADS ITS SHADERS AS
 * RESOURCES. gfx_opengl asks the ResourceManager for
 * "shaders/opengl/default.shader.glsl" and calls abort() if it is missing,
 * with the message "missing f3d.o2r?". A port that mounts nothing does not
 * merely lack textures; it cannot draw at all.
 *
 *   port/assets   a plain directory, mounted as a FolderArchive (the fork
 *                 kept FolderArchive). It holds libultraship's own shaders,
 *                 copied out of the LUS tree by tools/pc/stage_assets.sh, and
 *                 anything else the port wants to serve loose during
 *                 development. (BattleShip ships the same content as a packed
 *                 f3d.o2r instead; the folder form is friendlier while the
 *                 port is under construction.)
 *   port/o2r      where a Torch-built kirby64.o2r goes.
 *
 * They are SEPARATE DIRECTORIES because of a rule in
 * ArchiveManager::GetArchiveListInPaths: if a directory contains any .o2r,
 * .otr, .zip or .mpq, only those files are mounted and the directory itself
 * is NOT mounted as a folder. Dropping kirby64.o2r into port/assets would
 * therefore silently unmount the shaders and turn a working renderer into an
 * abort. Keeping archives in their own directory makes that impossible.
 *
 * KIRBY_ASSETS overrides the first, KIRBY_O2R the second. */
static std::vector<std::string> archive_paths(void) {
    const char* assetsEnv = getenv("KIRBY_ASSETS");
    const char* o2rEnv = getenv("KIRBY_O2R");
    std::vector<std::string> paths;
    std::error_code ec;

    paths.push_back(assetsEnv ? assetsEnv : "port/assets");
    paths.push_back(o2rEnv ? o2rEnv : "port/o2r");

    for (const auto& p : paths) {
        std::filesystem::create_directories(p, ec);
    }
    return paths;
}

static bool lus_init(void) {
    if (sInitTried) {
        return sInitOk;
    }
    sInitTried = true;

    /* THE FORK'S STAGED BOOT, in BattleShip's order (port/port.cpp:644,
     * PortInitImpl), minus the SSB-specific stages. Each InitX constructs its
     * component on first call and returns false on failure; the order is
     * load-bearing in exactly two places, both inherited from BattleShip:
     *
     *   - InitConfiguration before InitResourceManager (it reads Config for
     *     the default archive paths) and before the window-size sanity code
     *     below (which edits Config);
     *   - InitControlDeck before InitWindow: the window backends call
     *     ControllerUnblockGameInput from focus events during window
     *     creation. */
    try {
        sContext = Ship::Context::CreateUninitializedInstance(
            /* HALBERD: the port's name (a nod to BattleShip, the SSB64 port
             * whose architecture this follows). The name is the window title;
             * the shortName scopes the app-data directory. */
            "HALBERD - Kirby 64: The Crystal Shards", "halberd", "halberd.cfg.json");
        if (sContext == nullptr) {
            fprintf(stderr, "[lus] CreateUninitializedInstance failed\n");
            return false;
        }

        /* Logging first: everything after this can report failure. */
        if (!sContext->InitLogging()) {
            throw std::runtime_error("InitLogging failed");
        }
        if (!sContext->InitConfiguration()) {
            throw std::runtime_error("InitConfiguration failed");
        }
        if (!sContext->InitConsoleVariables()) {
            throw std::runtime_error("InitConsoleVariables failed");
        }

        /* THE CRASH HANDLER IS OPT-IN, and that is a considered choice.
         *
         * The fork's Ship::CrashHandler constructor installs sigaction
         * handlers for SIGINT and SIGTERM whose body is `exit(1)`. exit()
         * from a signal handler runs atexit handlers and static destructors
         * -- tearing down the GL context, joining threads, freeing the
         * resource cache -- none of which is async-signal-safe. Measured
         * behaviour in this port (against upstream LUS, same handler code):
         * Ctrl-C and `timeout` do not stop the process at all (it wedges in
         * teardown), and when it does get far enough it reports "free():
         * corrupted unsorted chunks".
         *
         * A development binary that cannot be interrupted is worse than one
         * without a crash log, so it is off unless asked for. Nothing in the
         * fork dereferences GetCrashHandler() unconditionally -- only the
         * CrashHandlerRegisterCallback bridge does, and this port never
         * calls it. */
        if (getenv("KIRBY_PC_CRASHHANDLER") != nullptr) {
            if (!sContext->InitCrashHandler()) {
                throw std::runtime_error("InitCrashHandler failed");
            }
        }

        if (!sContext->InitConsole()) {
            throw std::runtime_error("InitConsole failed");
        }

        /* LUS::ControlDeck is the concrete N64 deck (Ship::ControlDeck is
         * abstract; WriteToPad is pure virtual), and it is what turns host
         * gamepads into OSContPad. The fork's default constructor is safe
         * to call before the window exists -- it has no constructor-time
         * ConsoleVariable dereference -- and InitControlDeck only stores the
         * deck. Port-1 default bindings are
         * installed by Init(&bits), called after InitWindow below. */
        sControlDeck = std::make_shared<LUS::ControlDeck>();
        if (!sContext->InitControlDeck(sControlDeck)) {
            throw std::runtime_error("InitControlDeck failed");
        }

        /* allowEmptyPaths=false: if neither path yields an archive, fail HERE
         * with a diagnosable message rather than in gfx_opengl's abort() on
         * the first frame ("missing f3d.o2r?"). The headless fallback below
         * is this port's supported degraded mode. (BattleShip passes true at
         * this stage, but only because its first-run wizard needs a
         * partially-alive window to ask the user for a ROM; this port has no
         * wizard.) */
        if (!sContext->InitResourceManager(archive_paths(),
                                           std::unordered_set<uint32_t>{}, 1,
                                           /*allowEmptyPaths=*/false)) {
            throw std::runtime_error(
                "InitResourceManager failed (no archive found in port/assets "
                "or port/o2r -- run tools/pc/stage_assets.sh)");
        }

        /* The fork constructs FileDropMgr/EventSystem lazily too, but they
         * are DEREFERENCED WITHOUT NULL CHECKS elsewhere: gfx_sdl2's event
         * loop calls GetFileDropMgr()->SetDroppedFile on SDL_DROPFILE, and
         * the Gui's EventDebuggerWindow calls GetEventSystem()->... . Both
         * are constructed before the window so neither path can crash. */
        if (!sContext->InitEventSystem()) {
            throw std::runtime_error("InitEventSystem failed");
        }
        if (!sContext->InitFileDropMgr()) {
            throw std::runtime_error("InitFileDropMgr failed");
        }

        /* Window geometry fixups happen BEFORE InitWindow, which is what
         * reads them. */
        {
            auto config = sContext->GetConfig();

            /* KIRBY_PC_WINDOWED=1 forces a window. Worth having because LUS
             * persists Window.Fullscreen.Enabled and a headless or remote
             * session that once wrote `true` then hangs on every later
             * start: SDL waits for a fullscreen mode switch that a virtual
             * display never completes, IsFrameReady stays false, and
             * DrawAndRunGraphicsCommands silently draws nothing. That
             * failure looks exactly like a broken renderer and is not one. */
            if (getenv("KIRBY_PC_WINDOWED") != nullptr) {
                config->SetBool("Window.Fullscreen.Enabled", false);
            }

            /* A PERSISTED WINDOW SIZE CAN BE CORRUPT, AND THE COST IS NOT
             * COSMETIC.
             *
             * LUS saves the window geometry on exit and restores it on
             * start. Ask a virtual display for fullscreen and SDL can answer
             * with garbage; this port once wrote Window 35856x33278 into its
             * config that way, and every later start restored it. That is
             * 1.2 gigapixels of framebuffer: the software rasteriser crawled
             * and X_ShmPutImage failed with BadValue. It reads exactly like
             * a broken renderer and the renderer is fine.
             *
             * A size outside these bounds is not a user preference, it is
             * corruption -- no display is narrower than the N64's own
             * 320x240 or wider than 8K -- so it is discarded rather than
             * honoured. Fullscreen goes with it: whatever produced the bad
             * geometry is the thing that would produce it again. */
            const int w = config->GetInt("Window.Width", 640);
            const int h = config->GetInt("Window.Height", 480);
            if (w < 320 || w > 7680 || h < 240 || h > 4320) {
                fprintf(stderr,
                        "[lus] discarding corrupt saved window size %dx%d, "
                        "using 640x480\n",
                        w, h);
                config->SetInt("Window.Width", 640);
                config->SetInt("Window.Height", 480);
                config->SetBool("Window.Fullscreen.Enabled", false);
            }
        }

        sWindow = std::make_shared<Fast::Fast3dWindow>();
        if (!sContext->InitWindow(sWindow)) {
            throw std::runtime_error("InitWindow failed");
        }

        /* The Audio component is NOT created here. Its sample rate is fixed
         * when it is created (AudioSettings::SampleRate, default 44100), and
         * the N64 AI plays at whatever osAiSetFrequency asks for, so a sink
         * opened now at a guessed rate would play the game's samples at the
         * wrong speed. pcb_audio_init creates it at the game's rate on the
         * first osAiSetFrequency (src/pc/os_ai.c); nothing in LUS reaches the
         * Audio component before that. */

        if (!sContext->InitGfxDebugger()) {
            throw std::runtime_error("InitGfxDebugger failed");
        }

        /* Init() is what loads saved mappings from Config and installs the
         * default keyboard/mouse/gamepad bindings for port 1 when the config
         * has none, which is what makes a fresh checkout playable without
         * the user opening the binding UI first. Done after InitWindow
         * because the deck's input-blocked checks reach for the window's
         * Gui; the fork no longer hard-requires it at Init time, but the
         * ordering costs nothing. */
        sControlDeck->Init(&sControllerBits);
    } catch (const std::exception& e) {
        fprintf(stderr, "[lus] context creation threw: %s\n", e.what());
        sContext = nullptr;
    }

    if (sContext == nullptr) {
        fprintf(stderr,
                "[lus] libultraship failed to start. The window, the renderer "
                "and audio are unavailable;\n"
                "      the platform layer keeps running headless.\n");
        sWindow = nullptr;
        sControlDeck = nullptr;
        return false;
    }

    /* Kirby 64 is F3DEX2, the same microcode as SSB64. Fast3D defaults to
     * it, but stating it costs nothing and documents the assumption.
     * (SetRendererUCode is a one-line wrapper around gfx_set_target_ucode;
     * see pcb_gfx_set_ucode below for the per-task version.) */
    sWindow->SetRendererUCode(ucode_f3dex2);

    /* TWO PACERS IS ONE TOO MANY, and this is which one wins.
     *
     * The SDL window backend's SwapBuffersBegin calls SyncFramerateWithTime,
     * which nanosleeps until 1/mTargetFps has elapsed since the last frame.
     * That happens inside Interpreter::EndFrame, which this port calls from
     * pcb_gfx_run, which is called from osSpTaskStartGo -- so LUS's frame
     * limiter sleeps INSIDE the game's RSP execution, with the game's
     * scheduler stopped behind it. Meanwhile src/pc/os_vi.c is already
     * pacing the whole system from the same monotonic clock the game reads
     * through osGetCount, at exactly the rate the game expects.
     *
     * The game's VI wins, because sched.c's framebuffer recycling and its
     * whole task state machine are driven by the retrace and cannot be paced
     * by anything else. Setting a target far above the real rate makes
     * SyncFramerateWithTime's deadline always already past, so it never
     * sleeps. KIRBY_PC_TARGET_FPS overrides it for experiments. */
    {
        const char* fpsEnv = getenv("KIRBY_PC_TARGET_FPS");
        sWindow->SetTargetFps(fpsEnv ? atoi(fpsEnv) : 1000);
    }

    /* The N64 renders 320x240 and everything in the display list is in those
     * units -- scissors, texture rectangles, the viewport. Fast3D scales from
     * this to the window. */
    GfxSetNativeDimensions(320, 240);

    /* KIRBY_PC_FRAMEHASH=N samples every Nth drawn frame; bare "1" or any
     * non-numeric value means every frame. Off unless set -- the readback is
     * a synchronous stall and would be paid on every frame of every run. */
    {
        const char* hashEnv = getenv("KIRBY_PC_FRAMEHASH");
        const char* bufEnv = getenv("KIRBY_PC_FRAMEHASH_BUF");
        if (hashEnv != nullptr) {
            sSampleEvery = atoi(hashEnv);
            if (sSampleEvery < 1) {
                sSampleEvery = 1;
            }
        }
        /* GL_BACK BY DEFAULT, AND THE DEFAULT IS MEASURED. Fast3D's EndFrame
         * has already swapped by the time the sample is taken, so GL_FRONT
         * looks like the obvious choice -- and on this stack (Xvfb +
         * llvmpipe) it reads back uniformly blank while the X11 root window
         * plainly shows the rendered level. A run through world 1-1 reported
         * nonblank=0 for all 24659 frames while `import -window root`
         * captured grass, path, flowers and fence, and six captures 15
         * seconds apart all differed. GL_FRONT is simply not readable here.
         * KIRBY_PC_FRAMEHASH_BUF=front asks for it anyway on a stack where it
         * does work. */
        if (bufEnv != nullptr && bufEnv[0] == 'f') {
            sSampleBuf = PCB_GL_FRONT;
        }
    }

    /* Arm the GBI ring now: the interpreter exists from here on, and it has
     * to be recording before the first task or a crash on frame 1 dumps
     * nothing. No-op unless PC_TRACE=gbi. */
    pc_gbi_ring_init();

    sInitOk = true;
    return true;
}

/* --------------------------------------------------------------- video */

void pcb_video_init(int width, int height) {
    (void)width;
    (void)height;
    if (!lus_init()) {
        return;
    }
    pc_trace(PC_TR_VI, "[lus] window %ux%u\n", sWindow->GetWidth(),
             sWindow->GetHeight());
}

/* Never called with this backend -- os_vi.c asks pcb_has_renderer() first --
 * and it must not become a second presentation path if it ever is. */
void pcb_video_present(const void* fb, int width, int height, int fmt) {
    (void)fb;
    (void)width;
    (void)height;
    (void)fmt;
}

void pcb_video_shutdown(void) {
    if (sWindow != nullptr) {
        sWindow->Close();
    }
    sWindow = nullptr;
    sControlDeck = nullptr;
    sContext = nullptr;
}

int pcb_has_renderer(void) {
    return 1;
}

int pcb_alive(void) {
    if (!sInitOk) {
        return 1; /* headless fallback: nothing can ask us to quit */
    }
    return sWindow->IsRunning() ? 1 : 0;
}

void pcb_pump(void) {
    uint64_t t;

    if (!sInitOk) {
        return;
    }
    t = now_ns();
    if (t - sLastPumpNs < 1000000ull) {
        return;
    }
    sLastPumpNs = t;
    sWindow->HandleEvents();
}

/* --------------------------------------------------- the display-list path */

void pcb_frame_begin(void) {
    sTasksThisFrame = 0;
}

/* NO LONGER NEEDED UNDER THE FORK, kept only because os_sp.c (C, compiled
 * against the game's headers) still calls it and the exported C surface of
 * this file must not change.
 *
 * The old fork resolved a native gSPLoadUcode (a ucode TEXT POINTER in w1) by
 * comparing against these two globals (gfx_native_ucode_f3dex2/_s2dex,
 * gfx_native_ucode_mode). The JRickey fork deleted that scheme: its
 * interpreter switches microcode from the ENUM in G_LOAD_UCODE's w0 low 24
 * bits (interpreter.cpp: gfx_set_ucode_handler((UcodeHandlers)(w0 &
 * 0xFFFFFF))), and the per-task default is set through gfx_set_target_ucode.
 * The pointers os_sp.c hands in are therefore of no use to the renderer any
 * more; in-list ucode switches are the concern of the fork's interpreter
 * (being adapted separately), not of this backend. */
void pcb_gfx_set_native_ucodes(const void* f3dex2, const void* s2dex) {
    (void)f3dex2;
    (void)s2dex;
}

void pcb_gfx_set_ucode(int s2dex) {
    if (!sInitOk) {
        return;
    }
    /* gfx_set_target_ucode writes ucode_handler_index directly, which is the
     * same variable a G_LOAD_UCODE inside a display list writes. Setting it
     * per task is what makes each task start from its own microcode the way
     * the RSP does, instead of inheriting whatever the previous task's last
     * gSPLoadUcode left behind. (Under the old fork this went through
     * SetRendererUCode plus the native-ucode globals; the fork's
     * gfx_set_target_ucode is the exact replacement for the per-task half,
     * see pcb_gfx_set_native_ucodes above for the in-list half.) */
    static int sUcodeLogged = 0;
    if (sUcodeLogged < 8) {
        fprintf(stderr, "[lus] task ucode -> %s\n", s2dex ? "s2dex" : "f3dex2");
        sUcodeLogged++;
    }
    Fast::gfx_set_target_ucode(s2dex ? ucode_s2dex : ucode_f3dex2);
}

void pcb_gfx_run(const void* displayList) {
    if (!sInitOk || displayList == nullptr) {
        return;
    }

    if (++sTasksThisFrame > 1 && !sMultiTaskFrame) {
        sMultiTaskFrame = true;
        fprintf(stderr,
                "[lus] WARNING: %d graphics tasks in one frame. Fast3D's "
                "Interpreter::Run clears\n"
                "      the framebuffer on entry, so all but the last are "
                "erased. See the note at the\n"
                "      top of src/pc/pc_backend_lus.cpp.\n",
                sTasksThisFrame);
    }

    /* One call, one complete frame: StartDraw, Interpreter::StartFrame, Run,
     * EndDraw, Interpreter::EndFrame (which swaps buffers). Returns false when
     * LUS decided to drop the frame for pacing, which is not an error. */
    static const std::unordered_map<Mtx*, MtxF> kNoMtxReplacements;
    bool drew = sWindow->DrawAndRunGraphicsCommands((Gfx*)displayList, kNoMtxReplacements);
    if (getenv("KIRBY_PC_GUIDEBUG") != nullptr) {
        static int dbgN = 0;
        if (dbgN++ < 12) {
            fprintf(stderr, "[frame] drew=%d heldFb=%p\n", (int)drew,
                    (void*)sWindow->GetGfxFrameBuffer());
        }
    }
    if (drew) {
        sFramesDrawn++;
        pc_trace(PC_TR_GFX, "[lus] frame %d drawn from dl %p\n", sFramesDrawn,
                 displayList);
        /* Sample here and not in pcb_frame_end: the swap happened inside the
         * call above, so this is the first moment the finished image is in
         * the front buffer, and it is also the only place we know a frame was
         * actually produced. */
        if (sSampleEvery > 0 && (sFramesDrawn % sSampleEvery) == 0) {
            pcb_sample_frame();
        }
    }
}

int pcb_gfx_stats(unsigned* framesDrawn, unsigned* framesSampled,
                  unsigned* framesNonBlank, unsigned* distinct,
                  unsigned long long* lastHash) {
    if (framesDrawn != nullptr) {
        *framesDrawn = (unsigned)sFramesDrawn;
    }
    if (framesSampled != nullptr) {
        *framesSampled = sFramesSampled;
    }
    if (framesNonBlank != nullptr) {
        *framesNonBlank = sFramesNonBlank;
    }
    if (distinct != nullptr) {
        *distinct = sDistinct;
    }
    if (lastHash != nullptr) {
        *lastHash = sLastHash;
    }
    /* 1 unconditionally, and NOT `sInitOk`. The question this answers is
     * "does this backend rasterise", which is a property of the build, not of
     * how the run went. A LUS build whose context failed to come up drew zero
     * frames and must report a rasterising backend with framesDrawn=0 -- that
     * is a rendering FAILURE and has to be readable as one. Returning 0 there
     * would make it indistinguishable from a null-backend run, which is the
     * exact confusion this function exists to end. */
    return 1;
}

void pcb_frame_end(void) {
    if (!sInitOk) {
        return;
    }
    /* A retrace with no display list behind it. The game is alive but has not
     * produced a frame. RE-PRESENT THE HELD GAME FRAME rather than running
     * the GUI alone: RunGuiOnly clears the backbuffer every retrace, and at
     * ~120 retraces/s against a game that (under llvmpipe) lands a real frame
     * every couple of seconds, the window is black for 99% of wall time --
     * every capture caught a cleared frame. PresentCurrentFramebuffer is the
     * fork's held-VI-frame path (BattleShip uses it for the same purpose);
     * it re-draws the cached game FB through the normal composite. It
     * returns false until the first game frame exists -- fall back to
     * RunGuiOnly only then, so the window still repaints before boot. */
    if (sTasksThisFrame == 0 && sFramesDrawn == 0) {
        /* Measured on this stack: the game renders DIRECT to the backbuffer
         * (mRendersToFb false, GetGfxFrameBuffer()==0), so the held-frame
         * re-present has nothing to re-present and the RunGuiOnly fallback
         * CLEARS the freshly swapped game frame within one retrace -- the
         * game was visible for 1/120th of a second per real frame. Once the
         * first game frame has been drawn, do nothing on empty retraces: the
         * window simply keeps its last presented contents. Before the first
         * frame, keep repainting so the window doesn't look hung at boot.
         *
         * THE `sFramesDrawn == 0` GUARD IS ON THE WHOLE BLOCK, and it must
         * stay there. Guarding only the RunGuiOnly fallback makes
         * PresentCurrentFramebuffer run on EVERY empty retrace for the whole
         * life of the process -- and an empty retrace is the common case,
         * ~120 of them per second against a game frame every couple of
         * seconds. That call is not cheap: it enters Ship::Gui::StartDraw,
         * where ImGui's SDL2 backend re-enumerates monitors (an
         * XGetWindowProperty round trip), and ends in glXSwapBuffers ->
         * XSync, another round trip. Measured that way, the scheduler thread
         * was inside that path in two samples out of three and the game drew
         * 6 frames in 60 seconds; with the guard on the whole block the same
         * run boots through the logos and the opening movie. Nothing is lost:
         * X11 keeps the last presented contents without being told again. */
        if (!sWindow->PresentCurrentFramebuffer()) {
            sWindow->RunGuiOnly();
        }
    }
    sTasksThisFrame = 0;
}

/* --------------------------------------------------------------- input */

void pcb_input_poll(PCPad* pads, int n) {
    int i;

    for (i = 0; i < n; i++) {
        pads[i].button = 0;
        pads[i].stick_x = 0;
        pads[i].stick_y = 0;
        pads[i].present = 0;
    }
    if (!sInitOk) {
        pads[0].present = (n > 0);
        return;
    }

    if (sControlDeck == nullptr) {
        return;
    }
    /* ControlDeck already produces N64 pad state -- turning modern gamepads
     * into CONT_* bits and an 80-unit stick is the entire reason it exists --
     * so this is a field copy and src/pc/os_cont.c never learns where the bits
     * came from.
     *
     * WriteToPad refreshes sLusPads from the mapped devices; it is the call a
     * LUS main loop makes once per frame. Here the game asks whenever
     * osContStartReadData runs, which is the same cadence. (The fork widened
     * the parameter to void* -- it is still an OSContPad[MAXCONTROLLERS].)
     *
     * The pads MUST be zeroed first: LUS's Controller::ReadToOSContPad does
     * `pad->button |= ...` and only writes sticks when the current value is 0,
     * on the assumption that the host cleared the array this poll (a real LUS
     * game zeroes its OSContPad block in osContGetReadData). Without the
     * memset every button ever pressed stays latched in sLusPads forever, so
     * the game sees one buttonPressed edge per key per process lifetime. */
    memset(sLusPads, 0, sizeof(sLusPads));
    sControlDeck->WriteToPad(sLusPads);

    if (getenv("KIRBY_PC_INPUTDEBUG") != nullptr && sLusPads[0].button != 0) {
        static int inDbgN = 0;
        if (inDbgN++ < 40) {
            fprintf(stderr, "[input] pad0 buttons=%04x stick=(%d,%d)\n",
                    (unsigned)sLusPads[0].button, sLusPads[0].stick_x, sLusPads[0].stick_y);
        }
    }
    for (i = 0; i < n && i < MAXCONTROLLERS; i++) {
        pads[i].button = sLusPads[i].button;
        pads[i].stick_x = sLusPads[i].stick_x;
        pads[i].stick_y = sLusPads[i].stick_y;
        /* LUS has no "is a controller physically plugged in" concept that
         * matches the SI bus: a keyboard is always a valid port 1 device, and
         * ControlDeck::Init() says so by setting bit 0 of sControllerBits.
         * Reporting port 1 present keeps src/main/contpad.c on its normal
         * path rather than its no-controller path. */
        pads[i].present = (sControllerBits & (1u << i)) ? 1 : 0;
    }
}

void pcb_input_rumble(int port, int on) {
    (void)port;
    (void)on;
    /* Deliberately unimplemented rather than faked. LUS drives rumble through
     * per-device rumble mappings on the ControlDeck, and wiring it before the
     * game's own osMotorInit path is exercised would be untestable code. */
}

/* --------------------------------------------------------------- audio */

static int sAudioRate;

void pcb_audio_init(int freq) {
    if (!sInitOk || sContext == nullptr || sAudioRate != 0 || freq <= 0) {
        return;
    }
    Ship::AudioSettings settings{};
    settings.SampleRate = freq;
    if (!sContext->InitAudio(settings)) {
        fprintf(stderr, "[lus] audio: InitAudio(%d Hz) failed; no sound\n", freq);
        return;
    }
    sAudioRate = freq;
    auto player = sContext->GetAudio() != nullptr ? sContext->GetAudio()->GetAudioPlayer() : nullptr;
    fprintf(stderr, "[lus] audio: opened at %d Hz (%s, SDL driver %s)\n", freq,
            player != nullptr && player->IsInitialized() ? "device ready" : "no device",
            SDL_GetCurrentAudioDriver() != nullptr ? SDL_GetCurrentAudioDriver() : "none");
}

void pcb_audio_set_freq(int freq) {
    /* LUS fixes the device rate when the Audio component is created and has
     * no call that reopens it at another one. The game sets the rate once
     * (auCreatePlayers), so a later change is reported, not applied. */
    static int sWarned;
    if (sAudioRate != 0 && freq != sAudioRate && !sWarned) {
        sWarned = 1;
        fprintf(stderr, "[lus] audio: rate change %d -> %d Hz not applied\n", sAudioRate, freq);
    }
}

void pcb_audio_queue(const void* samples, uint32_t bytes) {
    if (!sInitOk || sAudioRate == 0) {
        return;
    }
    /* osAiSetNextBuffer in src/pc/os_ai.c has already byte-swapped the samples
     * into host order and paced on the queued-byte count, which is exactly the
     * contract AudioPlayerPlayFrame wants. */
    AudioPlayerPlayFrame((const uint8_t*)samples, (size_t)bytes);
}

uint32_t pcb_audio_queued(void) {
    if (!sInitOk || sAudioRate == 0) {
        return 0;
    }
    /* AudioPlayerBuffered counts sample FRAMES (SDLAudioPlayer::Buffered is
     * the queued byte count over 2 * channels); the AI's length register,
     * which this stands in for, counts bytes. */
    int32_t frames = AudioPlayerBuffered();
    int32_t channels = GetNumAudioChannels();
    return frames > 0 ? (uint32_t)frames * 2u * (uint32_t)(channels > 0 ? channels : 2) : 0u;
}
