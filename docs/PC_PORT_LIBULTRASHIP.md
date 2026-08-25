# libultraship in this port: what is wired, what is proven, what is not

This is a status document, not a plan. Everything asserted here was run.

    tools/pc/build_lus.sh            build libultraship out of tree
    tools/pc/stage_assets.sh         put Fast3D's shader where LUS can find it
    tools/pc/link.sh                 build and link the port (LUS is the default)
    tools/pc/link.sh --run           ... and run it

    KIRBY_PC_GFXTEST=1               submit a synthetic display list per frame
    KIRBY_PC_WINDOWED=1              force a window (see "fullscreen" below)
    KIRBY_PC_TRACE=1                 log missing symbols instead of stopping
    PC_TRACE=gfx,vi,sched,...        subsystem tracing
    PC_LUS=0                         link the headless backend instead

## The state in one paragraph

libultraship builds, the game links against it at `-m64`, and F3DEX2 display
lists submitted through `osSpTaskLoad`/`osSpTaskStartGo` are executed by Fast3D
and presented in an OpenGL window at the game's own 60 Hz. That path is proven
end to end with a synthetic display list built from the game's own `<PR/gbi.h>`
macros — 1198 frames in 20 s, colours matching the requested fill exactly when
the framebuffer is read back.

*(Updated 2026-08-12: the paragraph that stood here said the game never
reaches the renderer because `thread5_game` blocks on the undecompiled
`auThreadMain`. That was cured the same day by `src/pc/pc_audio_thread.c`, a
stand-in that posts the init message and consumes the audio flags. The game
now boots through the scheduler into `game_tick`; the current frontier is the
audio-library call surface (`auSetBGMVolume` reaching null sequence players)
and the boot-path stub set, tracked in the commit log. Audio itself is still
absent — the stand-in is not an implementation.)*

## How LUS's main loop and the game's scheduler were reconciled

This was the question the whole task turned on, and the answer is that they do
not conflict — for one specific reason that is worth stating precisely, because
it is a property of *this* port and not of ports in general.

`src/pc/os_thread.c` runs every N64 thread as a `ucontext` on **one host
thread**. That was chosen for the game's sake: Kirby 64 has no locks anywhere
and uses `osSetIntMask(OS_IM_NONE)` as an assertion that nothing else is
executing. But it is also exactly what makes libultraship embeddable, because
every constraint LUS has is a *thread affinity* constraint:

* an OpenGL context is bound to a thread — there is only one thread;
* SDL must be pumped on the thread that created the window — one thread;
* Fast3D's `Interpreter` keeps process-global state (`g_exec_stack`) — one
  thread.

So there is no ownership question to resolve. LUS calls happen wherever the
game reaches them and "wherever" is always the same OS thread. **The SSB64
method's collapse of the N64 threads into a single loop is the price of using
real threads; this port does not pay it because it does not have them.**

What replaces the main loop is a mapping of LUS's fixed per-frame order onto
events the game already emits:

| LUS main loop      | this port                                                    |
|--------------------|--------------------------------------------------------------|
| `HandleEvents()`   | `pcb_pump()` from `pc_pump_events()`, which every blocking libultra call goes through (rate-limited to 1 kHz) |
| game logic         | the game threads, dispatched by N64 priority                 |
| draw + present     | `pcb_gfx_run()` from `osSpTaskStartGo()` on an `M_GFXTASK`    |
| idle frame         | `pcb_frame_end()` at VI retrace runs the GUI alone if no display list arrived |

The frame boundary is therefore the *game's*, not a timer. That is strictly
better than a timer: `src/main/sched.c` recycles framebuffers off the same
event, so the two cannot disagree.

### The pacing consequence, which was not obvious

`GfxWindowBackendSDL::SwapBuffersBegin` calls `SyncFramerateWithTime`, which
sleeps until `1/targetFps` has elapsed. It runs inside `Interpreter::EndFrame`,
which this port calls from `osSpTaskStartGo` — so **LUS's frame limiter sleeps
inside the game's RSP execution, with the game's scheduler stopped behind it**,
while `src/pc/os_vi.c` is separately pacing the whole system from the clock the
game reads through `osGetCount`.

Two limiters on one frame beat against each other rather than averaging. The
game's VI wins, because sched.c's task state machine is driven by the retrace
and cannot be paced by anything else, so LUS's target is set far above the real
rate and its deadline is always already past. `KIRBY_PC_TARGET_FPS` overrides.

## What Fast3D needs that the game does not provide

**A resource archive, before it can draw anything at all.** This is the one
that will surprise people: `gfx_opengl.cpp` loads
`shaders/opengl/default.shader.glsl` *through the ResourceManager* and calls
`abort()` if it is missing, with the message "missing f3d.o2r?". A port that
mounts nothing does not merely lack textures — it cannot draw a triangle.
`tools/pc/stage_assets.sh` copies that shader out of the libultraship checkout
into `port/assets/`, which LUS mounts as a FolderArchive.

`port/assets/` and `port/o2r/` are **separate directories on purpose**.
`ArchiveManager::GetArchiveListInPaths` mounts a directory as a folder only if
it contains no `.o2r`/`.otr`/`.zip`/`.mpq`; dropping a Torch-built
`kirby64.o2r` into `port/assets` would silently unmount the shaders and turn a
working renderer into an abort.

**Segmented addresses are not resolved the way the game builds them.**
`Interpreter::SegAddr` treats `w1` as segmented only when bit 0 is set — the
OTR convention, where `DisplayListFactory` writes `seg | 1`. A raw N64 display
list carries even segmented addresses (`0x06001234`) and Fast3D will dereference
one as a host pointer. Nothing in this port hits it yet because the synthetic
list uses no segments and the game builds none yet, but **it is the first thing
that will break when real game display lists start flowing**, and the fix has
to be decided then: either mark segment references on the way in, or teach the
port's `G_MOVEWORD`/`G_MW_SEGMENT` handling to hand Fast3D pre-resolved
pointers.

**Nothing else.** The display-list format itself needs no adaptation: this
tree's `<PR/gbi.h>` already stores two `uintptr_t` per command, exactly like
libultraship's `F3DGfx`, so a `Gfx*` casts straight across at LP64.

## What the game provides that Fast3D does not want

**The XBUS split.** Kirby 64's scheduler runs graphics as an SP task followed
by a separate DP task: `func_80001FAC` calls
`osDpSetNextBuffer(task.t.output_buff, rdpBufSize)`. Under Fast3D that split
does not exist — Fast3D consumes the SP's *input* list and draws it, and
nothing ever writes an RDP command FIFO. `osDpSetNextBuffer` raising DP-done
and returning is the correct translation, not a stub.

**A framebuffer.** `osViSwapBuffer` still moves the pointers, and it must:
`scCheckGfxTaskDefault` picks a free buffer by comparing against
`osViGetCurrentFramebuffer` and `osViGetNextFramebuffer`. But no pixels cross
that boundary any more, so `pcb_video_present` is dead code under this backend
and `pcb_has_renderer()` selects between the two behaviours.

**Multiple graphics tasks per frame.** `Interpreter::Run` clears the
framebuffer on entry, so it is one-call-per-frame by construction. Kirby's
scheduler has a yield/resume path that can produce two tasks for one displayed
frame; if that happens the second erases the first. It is detected and warned
about once rather than silently flickering.

## LP64: what actually broke

The port had to move to `-m64` because libultraship is a 64-bit library. The
compile was the easy part (one `uintptr_t` typedef, already done). Three *data*
bugs were not, and all three were found by running:

1. **Truncated pointers.** `src/main/dma.c` does `dma_copy(..., (u32)vAddr, ...)`
   — correct on N64, a silent top-32-bit drop under a PIE at
   `0x555555554000`. The first 64-bit run memcpy'd to `0x5575b640`. The fix is
   not to edit the game: the port links **`-no-pie`** so the image loads at
   `0x400000` and the truncation is lossless. `pc_check_low_memory()` aborts at
   startup if that ever stops being true.

2. **bss sized for a 32-bit pointer.** `tools/pc/gen_data.py` emitted
   `.space N` blocks at their N64 size. `sched.c` declares
   `OSMesg D_80048C98[8]`; the listing says `.space 32`; at LP64 that needs 64,
   and the 32 bytes it overran were `scTaskMQ`, whose `mtqueue` became the
   message value `1`. Generated bss is now doubled — an exact upper bound,
   since only pointers grow and alignment never exceeds 8.

3. **`u32` is `unsigned long` in libreultra.** Its `<PR/ultratypes.h>` says
   `typedef unsigned long u32`, which is 8 bytes at `-m64`, so every struct it
   compiles has doubled fields while the game's headers disagree. `osViModeNtscLan1`
   read back as `ctrl = 0, width = 12574` — the port reported a 12574-pixel-wide
   display. `tools/pc/lu_lp64.h` is force-included ahead of it and claims its
   include guard; libreultra is a submodule and is also read by the matching
   build, so it could not be fixed in place.

4. **An 8-byte dereference of a 4-byte blob slot.** This is the fourth *data*
   class and the one that has cost the most, because nothing catches it. The
   game reads N64 data blobs whose slots are FOUR bytes. A `*(T **)`
   dereference reads EIGHT here. On hardware the two are the same instruction,
   so the ROM is byte-identical either way, every decomp gate passes, and
   `tools/pc/lp64_audit.py` does not see it — that tool audits call
   signatures, not dereference widths.

   Both of the port's in-game blockers were this, and each was one line:

   * `src/ovl1/ovl1_3.c` func_800AB0F4 declared `u32 **buf` where
     `gSegment4StartArray` is `u32 *[]`, so `buf[2]` read sixteen bytes in
     instead of eight. The value is the object's DRAW KIND, and every switch
     on it has no `default`, so affected objects were **silently not drawn**.
     The level rendered correctly around a player that was never emitted,
     which is why it read as a renderer problem for so long.

   * `src/ovl6/ovl6.c` func_80152EA8_ovl6 read `*(void **)(src + 4)` off a
     cursor advancing by an `src += 0x2C` N64 stride. The wide read swallowed
     the float behind the pointer and manufactured a non-NULL display list out
     of a position value; Fast3D took SIGSEGV walking into it. Slot 1 is NULL
     — that node has no display list at all.

   **The discriminator is the SLOT WIDTH of the thing being read, and it has
   to be checked per site rather than assumed**: this tree contains both
   conventions. A cursor with a hardcoded N64 stride (`+= 0x2C`, `* 0x10`,
   `* 4`) walks 4-byte slots and must be read as `((u32 *)p)[n]`; a table the
   port genuinely widens is indexed `base + idx * 8`, and `src/ovl1/ovl1_3.c`
   has an example of each. The two failure modes are opposite and equally
   quiet: read narrow where the port widened and you get a truncated pointer;
   read wide where it did not and you get a pointer made of the next field.

   **The 87 `*(T **)` sites under `src/` outside `src/pc` have now been swept,
   and the sweep was mostly a triage problem rather than a reading problem.**

   * **43 of the 87 are not compiled into the port at all** — they sit in
     `#ifdef MIPS_TO_C` factory drafts or in the `#else` arm of a construct
     whose `#ifdef PORT` arm is what the port builds. Decide this mechanically
     before reading anything: append a unique token to each candidate line, run
     the file through `gcc -E -P` with the Makefile.pc defines (`-DPORT
     -DNON_MATCHING -DAVOID_UB …`), and keep only the sites whose token
     survives. A line-marker heuristic over plain `gcc -E` output is *not* a
     substitute — gcc pads skipped regions with blank lines, so every dead site
     comes back live. The sweep collapses from 87 sites to 44.

   * Of the 44 live ones, most are **host-widened and correct**, and the
     giveaway is always a PORT-side declaration that fixed the layout on
     purpose: the twenty `D_8012BCA0 + 128 + i * 8` water-annex reads are at
     LP64 offsets by construction (`struct PcUnkBCA0Mirror` in
     `src/pc/pc_bss_whole.c`, with static asserts); `src/ovl2/ovl2_5.c` widened
     its particle record from the N64's `0x128` to `0x130` and the writer and
     both readers agree; and `src/ovl1/ovl1_3.c`'s six
     `*(struct GObj **)D_800DF850[objId]` derefs are **right**, because
     `func_800A94F4` hands back a block whose cells the port already widened to
     eight bytes — the same function's `temp_v0[2]`-under-PORT versus
     `temp_v0[1]` on N64 says so in one line.

   * **The index side of the class is bigger than the dereference side, and a
     `*(T **)` grep does not find it.** The failure is `(u8 *)table + idx * 4`
     where `table` is a pointer array the port widened to 8-byte elements: the
     bias addresses element `idx/2`, and for an odd index straddles two. It is
     invisible because the same source line usually reuses the same bias
     *correctly* on a neighbouring `s32[]` or `f32[]`. `src/ovl2/ovl2.c`'s
     `func_800F6350` PORT arm has carried this fix for `D_800DE350` for a
     while; four more instances over `D_800E1B50` (`ovl9_3.c`, `ovl9_9.c`,
     `ovl9_13.c`) and one more over `D_800DE350` (`ovl8.c`) were still
     standing. To find them: list every `extern T *NAME[]` in the tree and grep
     for `(u8 *) NAME +`.

   * **And the compiler introduces the class on its own, through struct
     padding.** `src/ovl19/helper.c` declared a sweep record as
     `{Vector; Vector; f32; void *d;}`. On LP64 that is `sizeof 40` with `d` at
     offset 32 behind four bytes of padding, while its only consumer
     (`func_8011BF4C`) reads the descriptor at byte 28 — the padding of an
     uninitialised stack local, dereferenced three times. Nothing in that
     struct is a cast, so no grep for one finds it. `src/ovl7/ovl7_3.c` had
     already written the rule down ("the hitbox-descriptor slot stays a u32
     host-address cell so the record keeps the N64's `f32[8]`/32-byte shape");
     this was the last of four callers still declaring a pointer. The same
     shape appears as a plain wrong constant in `src/ovl1/ovl1_2_2.c`, which
     wrote a `DObj *` at generator-node `+0x48` — the N64 offset of that slot,
     where LP64 puts `frame`, `dobj` having moved to `+0x50`.

   **The cheapest proof for all of these is `offsetof`/`sizeof`, not a
   debugger.** Copy the struct into a five-line host program and print the
   numbers; the port's own PORT-arm declarations then say which number the
   consumer expects. Several of these bugs are in code no route yet reaches, so
   a runtime probe prints nothing at all while the arithmetic is already
   conclusive.

A fourth bug was in the port's own scheduler and only showed up because LP64
work made the boot go further: `dispatch()` derived the *outgoing* ucontext
from `__osRunningThread`, which `pc_block_on` deliberately sets to NULL before
switching away. The blocked thread's registers were saved into the boot context
instead of its own slot, and the next thread to block overwrote them. It
survives with two alternating threads and crashes with eight.

## What does not work

* **The analog stick does not move the player, and only the D-pad does.**
  Measured with `KIRBY_PC_PLAYERPOS=<seconds>` (`src/pc/pc_progress.c`, which
  prints the player world position while `gGameState == 15`), world 1-1, each
  input held for ~150 wall seconds:

  | `KIRBY_PC_INPUT` | what is held | player X over the run |
  | --- | --- | --- |
  | `g0:DRIGHT:60000` | D-pad right only | walks `-2946.79` → `-1480.00` |
  | `g0:SR:60000` | stick only, `0x50` | `-2959.92`, never moves |
  | `g0:SR:60000` at `0x7F` | stick at full deflection | `-2959.92`, never moves |

  Full deflection ruling it out means this is not a deadzone: nothing
  downstream of `pads[0].stick_x` reaches the player. The plumbing as far as
  the game is intact — `src/pc/os_cont.c` copies `stick_x` into the
  `OSContPad`, `src/main/contpad.c` copies it on into
  `gControllers[i].stick_x` and then `gPlayerControllers[i].stickX` — so the
  gap is inside the player code that should read `stickX`. **This corrects a
  claim that stood in `src/pc/pc_input_script.c` for some time** ("THE STICK,
  NOT THE D-PAD, IS WHAT MOVES KIRBY"): the `walk` mode that appeared to
  confirm it also sets `CONT_RIGHT`, and `CONT_RIGHT` was doing all the work.

* **The player stops at world X = `-1480.00` and the level does not continue.**
  Walking right from the 1-1 spawn, X rises through a series of
  collision-corrected fractional values (`-2946.79`, `-2039.27`, `-1790.72`,
  …) and then stops at exactly `-1480.00` and stays there for the rest of the
  run. Walking *left* stops at `-2959.92`. The playable corridor is therefore
  about 1480 units wide and the run never leaves it.

  The **exactness** of the right-hand stop is the interesting half: every
  other resting value carries a fractional penetration correction, so
  `-1480.00` is a clamp against a datum, not a wall. It is **not** the
  track-parameter clamp in `func_800F8570` (`ovl2_2.c`) — instrumenting both
  of that function's `0.0001f`/`0.9999f` arms and its node transition showed
  it is never called during gameplay at all. That is where the next
  investigation starts.

* **The game reaches the renderer as of 2026-08-12** — the stand-in
  `src/pc/pc_audio_thread.c` posts the init message the real `auThreadMain`
  would. What remains on this path is the audio-library call surface: the
  first `au*` call dereferences the null sequence players that
  `auCreatePlayers` (still a pragma) would have built. Guarding that surface
  is porting work; implementing it is decompilation work.
* **Audio is wired but never exercised.** `pcb_audio_queue` calls
  `AudioPlayerPlayFrame`; nothing calls it, because `osAiSetNextBuffer`'s only
  caller is `auThreadMain`. There is also a **known rate mismatch**: the N64 AI
  runs at whatever `osAiSetFrequency` was asked for and LUS's `AudioSettings`
  defaults to 44100. It is deliberately not "fixed" by guessing.
* **Input is wired but never exercised**, for the same reason. Rumble is
  unimplemented rather than faked.
* **Controller remappings may not persist.** Ship::Window and Ship::ControlDeck
  both need the Config at *construction*, while `Context::CreateDefaultInstance`
  constructs the Config itself — so the Context is assembled by hand here
  (see `lus_init`) to thread one Config through. That part works; what is
  untested is the save path, because no controller has been remapped.
* **The crash handler is off by default** (`KIRBY_PC_CRASHHANDLER=1` enables
  it). `Ship::CrashHandler` installs SIGINT/SIGTERM handlers whose body is
  `exit(1)`, which runs static destructors — GL teardown, thread-pool joins —
  from a signal handler. Neither is async-signal-safe.
* **Nothing else notices SIGTERM either**, which is why `src/pc/os_time.c`
  installs its own handler. Without it the process ignores Ctrl-C and `timeout`
  entirely; SDL does not synthesise `SDL_EVENT_QUIT` for it here.
* **Fullscreen on a virtual display wedges.** LUS persists
  `Window.Fullscreen.Enabled`; once it is `true`, SDL waits for a mode switch a
  virtual display never completes, `IsFrameReady` stays false, and every frame
  is silently dropped. That looks exactly like a broken renderer and is not
  one. `KIRBY_PC_WINDOWED=1` forces it off.
* **No game assets are committed yet.** `port/o2r/` is empty in the tree.
  The mount itself is verified: a 13.9 MB Torch-built `kirby64.o2r` from the
  asset workstream was mounted alongside the shader folder and the renderer
  kept running unchanged (1195 frames in 20 s), so dropping the archive into
  `port/o2r/` really is the whole integration on this side.
* **The RDP quirks the BattleShip notes warn about are entirely unexplored.**
  Tile masks, `SetTileSize` extents, IA/I4 uploads and `gDPSetPrimDepth` 2D
  layering cannot be hit by fill rectangles. Expect them the day the game
  submits its first real list. `src/pc/gfx_trace.c` is the tool for that and
  now decodes correctly at LP64 (it walked the list as `u32*` before, reading
  every command's low half twice).

## Building libultraship here

Three things were needed and none is obvious:

1. **SDL3 from source.** Commit `6f42b9c` migrated LUS to SDL3 and SDL3 is not
   in apt on Ubuntu 24.04. Built to `/usr/local`.
2. **Three stub files.** Ubuntu ships `libzip-dev` without `libzip-tools`, but
   `libzip-targets.cmake` imports `libzip::zipcmp`/`zipmerge`/`ziptool` as
   IMPORTED executables and hard-errors when `/usr/bin/zipcmp` is absent. LUS
   never runs them; empty executables at those paths are enough.
3. **A one-line patch to the LUS checkout.** `cmake/dependencies/common.cmake`
   downloads `stb_image.h` from `github.com/nothings/stb/raw/...`, which this
   environment's proxy answers 403 while allowing `raw.githubusercontent.com`.
   `file(DOWNLOAD)` does not fail the configure — it writes a zero-byte file —
   so the failure surfaces minutes later as "stbi_uc was not declared". The
   patch switches the host and adds a status check.

Everything else (ImGui, prism, thread-pool, monocypher) FetchContent-clones
from github over git, which this environment allows.
