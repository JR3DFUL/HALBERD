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
    KIRBY_PC_INPUT=walk|play|...     scripted controller (src/pc/pc_input_script.c)
    KIRBY_PC_TIMESCALE=8             scale the count register (see "Speed" below)
    KIRBY_PC_FRAMEHASH=8             hash a sample of every 8th presented frame
    KIRBY_PC_PLAYERPOS=10            print the player's position every 10 s in-level
    KIRBY_PC_SKYDEBUG=1              skybox layers: placement, scale, camera parallax
    KIRBY_PC_BGDEBUG_FROM=<frame>    CI4/TLUT decode peeks from that task-frame on
    python3 tools/pc/smoke.py ...    run it and print one [verdict] line

## The state in one paragraph

libultraship builds, the game links against it at `-m64`, the game's own
F3DEX2 display lists submitted through `osSpTaskLoad`/`osSpTaskStartGo` are
executed by Fast3D, and driven by the scripted controller the port reaches
gameplay in world 1-1 and draws it. Measured 2026-09-02 in a 4-core container
with Mesa llvmpipe under Xvfb (no GPU), fork `JRickey/libultraship` branch
`ssb64` at cdb279c plus `patches/libultraship-jrickey-kirby.patch`, SDL2
2.30.11, with `port/o2r/kirby64.o2r` mounted:

    KIRBY_PC_WINDOWED=1 KIRBY_PC_FRAMEHASH=8 KIRBY_PC_PLAYERPOS=10 \
      python3 tools/pc/smoke.py --input walk --expect gameplay --timescale 8 \
                                --timeout 300 --verbose

    smoke: reached  gameplay  at 44.10s (gGameState route 0>1>2>3>10>11>12>15)
    smoke: outcome  interrupted, after 300.01s
    smoke: 15586 frame(s) rasterised, 1948 sampled, 1931 non-blank, 1877 distinct
    smoke: PASS -- reached gameplay, which is the recorded high-water mark

The route is logos, opening movie, title, file select, galaxy map (where
`walk` answers the world 1-1 cutscene prompt with D-LEFT then A, so state 14
is not on the route), planet map, and gameplay at 44 s wall. `[playerpos]`
put the player at `x = -1520.17 vel = 5.0000` ten seconds in and at
`x = -1480.00` — the ledge; `walk` holds D-RIGHT and never jumps — from +65 s
to the end of the run, with no fault (the `[playerpos]` figures are from the
2026-09-01 run of the same route). 1931 of the 1948 sampled frames were
non-blank and 1877 distinct. `smoke.py --deep` (the `advance` script, 150 s)
reaches gameplay at 51.48 s: 9185 frames, 1131 of 1148 sampled non-blank.

Unattended (no input), the same binary reaches `title-screen-2` at 59.41 s of
a 60 s run (5310 frames, 647 of 663 sampled non-blank); `smoke.py`'s default
ratchet is `attract-loop-complete`, which no 60 s run reaches on llvmpipe, so
the unattended check prints FAIL here both before and after this session.

**What the captures show** (root-window grabs of the 640x480 window during
those runs): the opening movie, the title screen, and world 1-1 with the HUD
(lives, health bar, shard counter), Kirby, a Waddle Dee, the checkered blocks,
fences and flowers, and (since 2026-09-02) the world 1-1 sky: the cyan
gradient, a cloud and the light-green hill band at the horizon. **What they
show wrong:** Ribbon's face in the opening movie is a noise-pattern texture.
Nothing else was checked against hardware.

**Speed.** `KIRBY_PC_TIMESCALE=8` scales the count register eightfold, but
nothing scales the rate at which frames are simulated, and on llvmpipe that
is bound by Fast3D: 17256 frames in 300 s is 57.5 frames/s, about real time.
The timescale does not shorten a run here; a scripted route does. It also
means any clock derived from the count register runs eight times faster than
the level — see the `play` entry under "What does not work".

Audio is absent: `src/pc/pc_audio_thread.c` stands in for `auThreadMain`,
posts the init message and consumes the audio flags, and plays nothing.

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

**Segmented addresses.** `Interpreter::SegAddr` treats `w1` as segmented
only when bit 0 is set — the OTR convention, where `DisplayListFactory` writes
`seg | 1`. The game's lists arrive carrying host pointers, all below 4 GB
because the image is linked `-no-pie`, and the fork's
`gfx_vtx_addr_is_unresolved` is patched to accept a low, mapped, 8-aligned
pointer as a real `Vtx` array (`patches/libultraship-jrickey-kirby.patch`). A
level, its objects and the player render through that path.
`src/pc/gfx_trace.c` describes what a stale segmented value looks like when
one does get through.

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

* **The analog stick does not move the player — and that is the ROM's own
  behaviour, not a gap in the port.** This entry used to end "the gap is
  inside the player code that should read `stickX`". There is no such code.

  Measured at runtime first. `KIRBY_PC_PROBE=1` with counters in
  `src/ovl1/util.c` (`#ifdef PORT`, ROM byte-identical), world 1-1, stick held
  at `0x50` for 340 s:

  | probe | count |
  | --- | --- |
  | `utilSetPlayerContPad` | 8274 |
  | `stickX.arrives` (that call saw `gPlayerControllers[0].stickX != 0`) | 8274 |
  | `utilCorrectStickX` | **0** |
  | `utilCorrectStickY` | **0** |
  | `utilGetStickDirection` | **0** |

  So the value arrives on *every* frame — the `[playerpos]` line reads
  `stick=80`, which is `gKirbyController.stickX` itself — and the only three
  functions in the game that read a raw stick axis are never entered. `vel`
  stays `0.0000`, the track parameter stays `0.001000`, X stays `-2959.92`.
  The same run with D-RIGHT held instead reads `held=0100`, `vel=5.0000`, and
  walks.

  Then confirmed over the whole 32 MB ROM image, which is the only way to
  make an absence claim honestly. A scan that tracks `lui`/`addiu`/`addu`
  bases through every instruction word and resolves each load/store's
  effective address reports:

  | field | writes | reads |
  | --- | --- | --- |
  | `sContPads[].stick_x` | (SI) | 1 `lb`, ROM `0x4D2C`, in `read_controller_input` |
  | `gControllers[].stick_x` | 1 | 1 `lb`, ROM `0x4DDC`, in `contSetPlayerPads` |
  | `gPlayerControllers[].stickX` | 6 | 2 `lb`: ROM `0x4D4EC` (`utilSetPlayerContPad`), ROM `0x4D7D0` (`utilCorrectStickX`) |
  | `gKirbyController.stickX` | 2 `sb`, both in `utilSetPlayerContPad` | **0** |
  | `gKirbyController.buttonHeld` (control) | — | 154 `lhu` |

  and a `jal` scan finishes it: `utilCorrectStickX` is called exactly once in
  the ROM (`0x4D8FC`) and `utilCorrectStickY` exactly once (`0x4D8EC`), both
  from inside `utilGetStickDirection` — and `utilGetStickDirection` is called
  **zero** times. Its address never appears as a `jal`, as an `la` pair, or as
  a data word, so it is not reached through a table either. It is dead code in
  the retail ROM.

  The stick is therefore copied three times and read by nothing. Movement is
  `gKirbyController.buttonHeld & 0x300` — D-pad left/right — in `ovl2/plylib.c`,
  and Kirby's walk speed is not analog in this engine anyway. **Drive gameplay
  with the D-pad. Making the stick work is a deliberate port feature (synthesise
  the D-pad bits from `stick_x` in `src/pc/os_cont.c`), not a bug fix, and it
  should be labelled as one if it is ever added.**

* **The player stops at world X = `-1480.00`, and `-1480.00` is a wall plane.**
  Walking right from the 1-1 spawn, X rises through a series of fractional
  values and then stops dead at exactly `-1480.00`, `blocked=0`, with the
  engine still asking for full speed: `node=3 t=0.517857 vel=5.0000
  acc=0.6250 held=0100`, unchanged for 275 s.

  The previous note here reasoned that the *exactness* made it "a clamp
  against a datum, not a wall". That is disproved. `-1480.00` is exact because
  it is a plane constant read straight out of the level's collision data:

  ```
  (gdb) print *(struct Normal *) <the blocking record's plane>
  $1 = {x = -1, y = 0, z = 0, originOffset = -1480}
  (gdb) print *(struct CollisionTriangle *) <its triangle>
  $2 = {vertex = {102, 97, 99}, polyCount = 30, normalType = 1,
        collisionIndex = 0, breakParticle = 0, Halt_Movement = 0,
        collisionParameter = 0, collisionType = 0}
  ```

  An ordinary solid face (`collisionType` 0 = default, `Halt_Movement` 0)
  whose plane is `x = -1480`, normal `(-1,0,0)`, i.e. facing back down the
  corridor. A wall resolve puts the body *exactly* on the plane; it is the
  values that are *not* on a plane that carry fractions.

  The mechanism, from a hardware watchpoint on `D_800E6BD0[0]` and the
  `8E6C.obj0` probe (`src/ovl2/ovl2_3.c`, `#ifdef PORT`):

  1. `func_800F8E6C` advances the track parameter `0.517857 -> 0.520089`
     (`vel 5.0 * 0.1 / len 224`), as it should.
  2. `func_80152828_ovl3` → `func_8010B11C` resolves the move and writes
     `gPositionState.kirbyFootPos[0] = -1480` while the proposed
     `gEntitiesNextPosXArray[0]` was `-1475`.
  3. `func_801529C0_ovl3` (`src/ovl3/ovl3_1.c:713-719`) takes
     `dx = -1480 - -1475 = -5.0000` and hands it to `func_800F8728`, which
     converts the world delta back into track progress and subtracts exactly
     what step 1 added — `0.520089 -> 0.517857`.

  So the probe prints `*UNDONE*` every frame, and the parameter is frozen
  while `vel` is 5.0. It is **not** the `[0,1]` clamp in `func_800F8A24` (its
  out-of-range arm never fires), **not** the node hop declining
  (`func_800F8B1C` is entered 8239 times and hops 0 more times), and **not**
  `func_800F8570`, which an earlier lane had already ruled out.

  **And the level wants a jump.** Same script plus an `A` pulse every two
  seconds
  (`g0:DRIGHT:60000` + `g20:A:12,g22:A:12,…`):

  | time | x | node | t |
  | --- | --- | --- | --- |
  | +72 s | `-2096.47` | 3 | 0.242648 |
  | +80 s | `-1480.00` | 3 | 0.517857 |
  | +88 s | `-1224.80` | 3 | 0.631785 |
  | +93 s | `-947.75` | 3 | 0.757700 |

  So `-1480.00` is an ordinary ledge, the corridor was never a corridor, and
  the port plays the level. What made it look like a wall for two lanes is
  that both the plain `DRIGHT` script and the built-in `play` program jump on
  a clock that does not happen to line up with arriving at it. `play` now
  presses `A` on a short repeating cycle for that reason.

* **Past the ledge was unrun ground, and three separate LP64 layout bugs
  lived there. All three are fixed and the level now runs.** A
  `g0:DRIGHT+SR` walk with a 2-second `A` cycle takes the player from the
  spawn at `x = -2096` across the `-1480` ledge, off the end of track node 3
  and onto node 4, for 330 wall seconds at `KIRBY_PC_TIMESCALE=8` with
  `outcome=interrupted` and no SIGSEGV. What was found, in the order it
  surfaced:

  1. **The spawn callback tables were emitted as scalar data.**
     `utilFuncTableJump (src/ovl1/util.c:151) <- func_800FCFF0
     (src/ovl2/spawn.c:201)`, SIGSEGV at `x = -947.75`.
     `utilFuncTableJump(idx, max, tbl)` does `tbl[idx](omCurrentObj)` with
     `tbl` an array of function pointers, and `D_801244A4` was not one here:
     the decomp's listing (`asm/data/ovl2/spawn.data.s:144`, ROM `0xACF14`)
     has all fourteen words as raw cross-overlay addresses, because splat
     writes `.word func_X` only for a target inside the segment it is
     disassembling. `tools/pc/gen_data.py` widened a `.word` block into
     `void *[]` only when at least one word was a relocation reference, so
     this one came out as `u32 D_801244A4[] = { 0x801BD7C4, … }` — dense
     4-byte slots, indexed at LP64 pointer stride.

     FIXED in `gen_data.py` (`is_all_function_words`), which now also treats
     an all-`.word` block as a pointer block when EVERY word is an exact hit
     on a `func_` symbol in the matching build's symbol table. That rule is
     measured, not curated: of the 2085 all-`.word` blocks in `asm/data` with
     no symbolic ref, exactly three satisfy it — `D_8012447C`, `D_801244A4`,
     `D_801244DC` — and all three are passed to `utilFuncTableJump` by
     `spawn.c` with a bound equal to their own length (3, 0xE, 0x2C against
     3, 14, 44 words).

     The two siblings this document previously listed with them, `D_801242D0`
     and `D_80124488`, were **already correct** and needed nothing:
     `D_801242D0` carries four symbolic refs (indices 42, 47, 62, 103) and
     `D_80124488` six, so both already satisfied the old rule. That claim is
     withdrawn.

  2. **`func_80218520_ovl9`'s draft cleared the wrong array element**, which
     showed up as a hard WEDGE at `x = -536.54` — not a crash: the process
     lives, keeps rendering, and stops advancing. Three `gdb` stack samples
     two seconds apart were identical:

     ```
     func_80218520_ovl9      src/ovl9/ovl9_15.c
     utilFuncTableJump(idx=1, max=3, D_8021CDA0_ovl9)
     func_802180D8_ovl9      src/ovl9/ovl9_15.c:1160   <- a `while (1)`
     func_8021817C_ovl9
     func_80218020_ovl9
     func_800FCF0C           src/ovl2/spawn.c:185
     ```

     The m2c draft kept m2c's byte offset (`var_v0 = objId * 4`) and used it
     as an ELEMENT index on `s32 gEntityFuncListIDArray[]`, so this object's
     state id was never cleared and the `while (1)` re-entered state 1 for
     ever; on the frame where the `func_800AF27C` yield is skipped it spins
     without yielding at all and starves the cooperative scheduler. FIXED in
     the decomp (`NON_MATCHING` arm only, ROM byte-identical).

     **This is a class.** Scanning for "a local assigned `X * 4` that is then
     dereferenced as `*(TypedArray + local)`" finds **91 sites in 16 files**,
     including `src/ovl9/ovl9_5.c:785` on this very array. Each needs its own
     listing check — some `* 4` locals really are element counts — but every
     true positive is a silent 4x out-of-bounds access in any port build.

  3. **`struct Ovl7AnimObj` is `struct CollSlot` behind an N64-offset
     filler.** `func_8019F410_ovl7 (src/ovl7/ovl7_2.c:112) <-
     func_80218248_ovl9 (src/ovl9/ovl9_15.c:1230)`, SIGSEGV on 3 runs of 3 at
     `x = -639 .. -931`. `src/ovl2/ovl2_9.c` declares `func_80111C88` as
     returning a `CollSlot *`; `src/ovl7/ovl7_2.c` declares the same function
     as returning an `Ovl7AnimObj *` and names its one field by the N64 byte
     offset. `CollSlot` leads with `void *unk0` and carries
     `struct Shape28 *unk1C`, so at LP64 everything after the first pointer
     slides:

     ```
     gdb -ex "ptype /o struct CollSlot"     ->  56 bytes, unk24 at offset 48
     gdb -ex "ptype /o struct Ovl7AnimObj"  ->  48 bytes, unk24 at offset 40
     ```

     Offset 40 in `CollSlot` is `s32 unk20`, the shape count. Instrumented,
     every one of 480 calls read `anim->unk24 == 0x1`, and the first one to
     take the `hdr->unk4 == 0 && arg0 != 0` branch stored through `0x1`:

     ```
     [f410] id=33 ent=0x12f2e48 unk8C=0x11ff340 anim=0x12e9f80
            hdr=0x11ff300 hdr4=0 arg0=0x17296d8 unk24=0x1
     ```

     FIXED in the decomp under `#ifdef PORT` (filler `48`, N64 arm untouched).
     `src/ovl7/ovl7_10.c:310` declares its own `struct Ovl7AnimObj` as
     `filler0[0x20] + unk20` over `func_80111A04`'s return value; if that is
     also a `CollSlot` the same slide applies. Not measured yet.

  **What stops the run now is the level, not a fault.** On node 4 the track
  parameter `t` oscillates — `0.4755, 0.3043, 0.2908, 0.3077, 0.3954, 0.2255,
  0.4984` at 5-second intervals, `vel` a steady `5.0000`, `face` `+1`
  throughout — so a held D-RIGHT loops the player around that node rather
  than leaving it. Getting further needs a route (whatever world 1-1 wants
  there — an inhale, a door, a switch) in the cue list, not a longer hold.

  A related trap worth knowing: `kPlayProgram`'s walk cue used to hold for
  `60000` frames, which is 1000 GAME seconds — only ~125 wall seconds at
  `KIRBY_PC_TIMESCALE=8`. A run that outlives its own hold freezes with
  `vel=0.0000` and looks exactly like a wedge. `held=0000 stick=0` in the
  `[playerpos]` line is the tell. The cue now holds for `360000`.

* **`play` keeps time in simulated frames now, and its route ends in
  player action 14.** The cue times in `src/pc/pc_input_script.c` are game
  seconds. The clock behind them used to be the count register, which
  `KIRBY_PC_TIMESCALE` scales while nothing scales the simulation; on
  llvmpipe at timescale 8 the register ran 8x and the game drew 57.5
  frames/s, so `play`'s cues fired eight times earlier in the level than
  written — the `g52` B cue landed at `x = -1943.47`, before the ledge, and
  Kirby stood in action 14 with `vel = 0.0000` for the remaining 190 s of a
  240 s run. The clock is now an accumulator over `gtlDrawnFrameCounter`
  (`src/main/gtl.c`: one bump per drawn frame, zeroed at scene setup, which
  the accumulator absorbs). Same program, same machine, after the change:

      [verdict] outcome=interrupted stage=gameplay stage_at=28.76 elapsed=240.01
                gamestate=15 frames=7614 route=0>1>2>3>10>11>12>15 render=raster
                drawn=14656 sampled=1832 nonblank=1768 distinct=792

  Kirby is over the ledge at +38.8 s (`x = 41.57 y = 98.10 node = 4
  action = 6`), falls at +58.8 s (`y = -3774.32`), respawns at node 0
  (`x = -2912.42`), and from +78.8 s to the end of the run reads
  `x = -2629.92 y = -1.90 vel = 0.0000 action = 14 held = 0100`. Action 14 is
  entered by `func_801727D8_ovl3` (`src/ovl3/kirby.c`), which sets it, stores
  an upward speed of 9.0 and a gravity of -0.980665 into the player's motion
  slots and parks its coroutine; its per-frame entry
  (`D_80196AE8_ovl3[14] = func_80172A3C_ovl3`) hops to action 6 as soon as
  `gKirbyState.unk30` is non-zero, which the start function has just made
  it. Neither the stored speed (y never leaves -1.90) nor the hop happens
  here, so the player's per-frame tick is not running in that state, or
  `set_kirby_action_1(6, 6)` does not take. The `[input]` log for the run
  above puts the B cue at frame-clock +113.38 s (g52 exactly), a sample
  with Kirby still walking (action 3) after it, then `DDOWN+SD` (g57) and
  the START pause/unpause pair (g62, g68), and action 14 at the next sample;
  which of those three enters it was not isolated. Not traced further;
  `src/ovl3/kirby.c` is being worked by a decompilation lane. The `walk`
  route presses none of them and never hits it.
* **Audio is absent.** `src/pc/pc_audio_thread.c` stands in for
  `auThreadMain`: it posts the init message and consumes the audio flags, and
  that is all. What remains on this path is the audio-library call surface:
  the first `au*` call dereferences the null sequence players that
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
* **`port/o2r/` is a build product, and nothing reads from it yet.**
  `build.sh` step 6 — by hand, `torch o2r baserom.us.z64 -s port/yamls -d
  port/o2r` from the decomp root — writes `kirby64.o2r`: 10,583 resources,
  13,987,129 bytes, 3.8 s with the JRickey `ssb64` Torch at c3565f1. The
  binary mounts it beside the shader folder on every run (`archive_paths()`
  in `src/pc/pc_backend_lus.cpp`; `KIRBY_O2R` overrides the path) and the
  runs above were made with it mounted. But no code under `src/pc` loads a
  resource out of it: every texture Fast3D drew came from ROM memory the
  port DMAs at runtime through `src/pc/os_pi.c`. The archive is the
  integration point for replacing that, not something the game depends on.
  (`audio.yml` extracts as plain BLOBs because the fork's Torch has no BK64
  factory — see docs/PC_PORT_ASSETS.md.)
* **The world 1-1 sky was black, and it took four bugs on two sides to be.**
  Fixed 2026-09-02; the frame now carries the cyan gradient, a cloud layer
  and the hill band. The camera clear really is black (`SETFILLCOLOR
  0x00010001` from colour record 127 of `D_800D478C`, which is zero in the
  ROM data), so the sky is the three skybox layers `src/ovl2/ovl2_6.c`
  emits as S2DEX `G_BG_1CYC` commands, and `KIRBY_PC_SKYDEBUG=1` shows they
  were emitted every frame. What kept them off the screen, in the order
  found:

  1. **`Gfxs2dexBg1cyc` took `frameW/frameH` as the far corner.** The fork
     (and upstream) passed them straight to `GfxDpTextureRectangle` as
     `lrx/lry`; they are the frame's size. Only a frame at the origin worked,
     and Kirby's layers sit inside the 3D view (`frameX=40 frameY=308
     frameH=192`), so each came out as an inverted rectangle above its own
     top edge — the "sprite fragments along the top edge". Now
     `frameX + frameW`, `frameY + frameH`, and `scaleW/scaleH` are the
     rectangle's `dsdx/dtdy` as in `guS2DEmuBgRect1Cyc` (the PORT arm sets
     them to `1024/scale + 0.5`, which is what the ROM's own emitter
     `func_800FF9B4` computes at `0x800FFCB4`). In the patch.
  2. **The skybox pitch parallax saw a camera eye at the origin.** The three
     layers are placed 50–85 px below the view top and scrolled up by
     `rect[1] * pitchFrac`, and `pitchFrac` comes from the at/eye snapshot
     `D_800D7B20`. The ROM writes its eye half by the interior name
     `D_800D7B2C`; `gen_data.py` emitted the two as separate objects, so the
     eye stayed `(0,0,0)`, `pitchFrac` read `+0.03` instead of `-0.38`, and
     the layers sat 60 px too low — under the ground. `D_800D7B20`
     (0x18) and `D_800D7B38` (0x30: the previous pair plus the six-float
     park block `func_800FC62C` writes at `+0x18`, which a 24-byte object
     overran) are now whole in `src/pc/pc_bss_whole.c` with `D_800D7B2C`
     aliased at `+0xC`. Measured: `[skycam] snap eye=(0.0,0.0,0.0)
     pitchFrac=0.0323` before, `eye=(-1357.0,180.0,476.8) pitchFrac=-0.3827`
     after, the layers at `y = -5.8 .. 42`.
  3. **A layer clipped at the view top showed its top rows, not the rows
     under the clip.** The PORT arm now offsets `imageX/imageY` (u10.5) by
     the clipped amount over the layer scale; `Gfxs2dexBg1cyc` passes them
     to the rectangle unshifted (both are u10.5; the old `<< 3` was harmless
     only while they were always 0).
  4. **Every 4-bit `LOADBLOCK` recorded 0 bytes.** `GfxDpLoadBlock` kept
     its texel-to-byte shift in a `uint32_t`; the 4-bit case's `-1` compared
     `> 0` and shifted left by 4294967295. The CI4 hill layer therefore
     decoded as 0 rows (`[dlog import] ... siz=0 ... size=0` every frame; the
     texture-upload census never saw its address). Signed now; the same
     `LOADBLOCK` reports `size=1024` and the census shows `CI4 64x32
     rgb_nz=1.00 a_nz=0.80`. The render tile's `line` for a 4-bit image was
     0 for the same reason (`width * siz` with `siz` an enum); it is
     `((texels << siz) / 2 + 7) / 8` now.

  Still wrong in the same family: Ribbon's face in the opening movie is a
  noise-pattern texture, and Kirby's own face texture is absent in-level
  (flat pink body). Neither is traced; `KIRBY_PC_TEXCENSUS=1` now also
  prints one `[dlog import]` line per `ImportTexture` call (format, size,
  TLUT mode, palette, tile, bytes), which is how the 0-byte load above was
  found.

## Building libultraship here

`build.sh` steps 2-3 are the record; this is what they do and why.

1. **SDL2 2.30.11 from source** into `third_party/sdl2-install`. The JRickey
   fork is an SDL2 codebase (`imgui_impl_sdl2`, `SDL_OpenAudio`); upstream
   libultraship moved to SDL3 and this fork did not.
2. **The fork, plus one patch.** `JRickey/libultraship` branch `ssb64`
   (cdb279c, 2026-08-11, at the time of writing) with
   `patches/libultraship-jrickey-kirby.patch` applied — Kirby's additions to
   `fast/interpreter.cpp` and `Gui.cpp`: the texture-census hooks, the
   low-pointer `Vtx` rule, S2DEX background and sprite handling, rectangle
   and viewport changes. The script resets the checkout and re-applies the
   patch on every run. Configure is `cmake -G Ninja
   -DCMAKE_BUILD_TYPE=Release -DGBI_UCODE=F3DEX_GBI_2 -DLUS_BUILD_TESTS=OFF
   -DCMAKE_PREFIX_PATH=<sdl2-install>`; `ninja libultraship` is 248 steps.
3. **Three stub executables.** Ubuntu's `libzip-dev` installs a
   `libzip-targets.cmake` that imports `libzip::zipcmp`/`zipmerge`/`ziptool`
   as executables and fails the configure when `/usr/bin/zipcmp` is absent,
   although LUS never runs them. `build.sh` writes `#!/bin/sh` / `exit 0` at
   each missing path; that is exactly what was on disk when the library
   built here.

The `stb_image.h` download in `cmake/dependencies/common.cmake` came through
this environment's proxy intact (284,733 bytes); `common.cmake` has a fallback
path (`torch/lib/n64graphics/stb_image.h` in the checkout) for when it does
not. Everything else (ImGui, prism, thread-pool, glslang, SPIRV-Cross, tinycc,
hidapi) is fetched by CMake from GitHub over git.
