# PC port architecture: the SSB64 method

The reference is **BattleShip**, the PC port of Super Smash Bros. 64, built on the
`ssb-decomp-re` decompilation. That pairing is unusually relevant here: SSB64
and Kirby 64 are both HAL Laboratory titles sharing the same GObj object-manager
engine and both use F3DEX2 microcode, so the port shape transfers almost
directly rather than by analogy.

## Why this method

Implemented directly, `osSpTaskLoad` / `osSpTaskStartGo` / `osDpSetNextBuffer`
would be an F3DEX2 interpreter plus a rasteriser backend -- comparable in
effort to everything else in the port combined. Under this method that work is a
**dependency, not a task**: libultraship's Fast3D intercepts the GBI display
lists and translates them to modern GPU calls. The same applies to audio output,
input mapping, and resource management.

That is the single largest reduction in the project's cost.

## The three layers

    decomp source  (src/, unmodified)   still compiles under IDO for the
                                        matching N64 build. Byte-exactness of
                                        build/kirby.us.z64 is never traded away.
    port layer     (port/)              glue translating N64-shaped APIs into
                                        libultraship calls
    libultraship                        Fast3D rendering (OpenGL/Metal/D3D),
                                        SDL2 input, miniaudio output, resource
                                        management, ImGui overlay

The discipline that makes this work is `#ifdef PORT`: game code stays intact and
PORT branches carry the adaptation. Behaviour accuracy takes precedence over
byte-for-byte fidelity **in the port**, while the N64 build stays byte-exact.

## Platform-layer pieces that fit

The platform layer already provides the right seam:

* `src/pc/pc_backend.h` is a host boundary with two interchangeable
  implementations (`pc_backend_null.c`, `pc_backend_sdl.c`). Adopting
  libultraship means adding a third implementation behind the same interface,
  not rewriting the callers.
* `src/pc/pc_overlay.c` already implements overlay interception, which is what
  the SSB port calls linearising the overlay model.
* `src/pc/gfx_trace.c` decodes display lists. Fast3D supersedes it for
  rendering, but a decoder is exactly the tool for diagnosing the SSB64-specific
  RDP quirks the BattleShip notes describe (tile masks, SetTileSize extents,
  IA/I4 uploads, `gDPSetPrimDepth` 2D layering) when Kirby 64 hits its own.

## Assets: build-time extraction, not runtime DMA

In this method the baserom is **never read at runtime**. Torch walks the ROM at
build time from YAML configs and emits a resource archive; libultraship's
resource manager mounts it and game code requests resources by path.

This replaces backing `osEPiStartDma` with reads from `baserom.us.z64` as the
long-term asset path, and it is where the 70 unresolved ROM-file-offset symbols
(`D_39E90`, `D_3B220`, ...) in `tools/pc/gen_defsyms.py` are meant to land.
Today the port still DMAs ROM data at runtime through `src/pc/os_pi.c`, and
nothing reads the archive yet (see docs/PC_PORT_LIBULTRASHIP.md).

## Pointer width: LP64

The port is LP64 because libultraship is a 64-bit build and 32-bit cannot link
against it:

    /usr/lib/x86_64-linux-gnu/libSDL2.so    exists
    /usr/lib/i386-linux-gnu/libSDL2.so      does not

SDL2 on an x86_64 distribution is x86_64 only, so `gcc -m32 ... -lSDL2` cannot
link. **32-bit and libultraship are mutually exclusive.**

Going LP64 took three small changes. Without them, three files fail to compile
at `-m64` (`gbi.h` casts a pointer into a 32-bit display-list word inside a
static initializer; `va_list` is an array type on x86-64). With them, all 151
game files compile at `-m64` under `-DPORT`, and the N64 build stays
byte-exact.

The SSB port needs `PORT_RESOLVE()` relocation tokens because it stores a `T*`
in a 32-bit word. Kirby 64's tree does not, because `gbi.h` here is already
64-bit aware -- `Gwords` holds two `uintptr_t`, so a display list can carry a
real pointer. The obstacle was `include/PR/ultratypes.h` hardwiring
`uintptr_t` to `u32`, which made those static display lists fail with
"initializer element is not constant": casting a 64-bit pointer to a 32-bit
integer is not something the linker can resolve. Widening it under PORT fixes
both files with no change to their source, and on MIPS32 a pointer is 4 bytes
so `u32` is correct there anyway.

The other two changes are `osVirtualToPhysical`, declared as returning `u32`
in one header and `uintptr_t` in another (they agree only while those are the
same type), and three `(va_list)` casts in `fault.c` -- an N64 idiom for
rounding the varargs pointer, invalid on x86-64 where `va_list` is an array
type and pointless where the ABI already aligns arguments.

Makefile.pc is `-m64 -DPORT` and tools/pc/link.sh adds `-no-pie`, which is what keeps the
places game code truncates a pointer to `u32` lossless. See
docs/PC_PORT_LIBULTRASHIP.md, "LP64: what actually broke".

## Dependencies: resolved

**docs/PC_PORT_LIBULTRASHIP.md** is the current state of the integration: the
port is LP64,
links against libultraship, and Fast3D executes the display lists that reach
`osSpTaskStartGo`. That page also records what is proven by running versus
merely wired, and the three LP64 *data* bugs the 64-bit move exposed.

    libultraship  third_party/libultraship   JRickey/libultraship at LUS_REF (cdb279c),
                                             plus patches/libultraship-jrickey-kirby.patch
    Torch         third_party/torch          JRickey/Torch at TORCH_REF (c3565f1)

Those paths are what `build.sh` uses, and they are relative to the repository
on purpose: an absolute path is wrong on every other machine and does not
belong in a repository. Override them with `LUS_ROOT`/`LUS_BUILD` (see
tools/pc/lus_flags.sh) if the clones live elsewhere.

Upstream libultraship lives at **Kenix3/libultraship**, not under
HarbourMasters -- HarbourMasters hosts the ports (Shipwright, Starship), not
the library. This port builds against the JRickey fork, not upstream.

Upstream libultraship migrated to SDL3 on 2026-08-04 (#1191). The JRickey fork
did not and is SDL2-only, which is why `build.sh` builds SDL2 2.30.11 from
source (see docs/PC_PORT_LIBULTRASHIP.md, "Building libultraship here").

Its other dependencies -- libzip, nlohmann_json, spdlog, tinyxml2, glew -- are
all in apt.

Torch has NAUDIO:V0 and NAUDIO:V1 factories (AUDIO_HEADER, BANK, SAMPLE,
SEQUENCE, SOUND_FONT, INSTRUMENT, ENVELOPE, ADPCM_BOOK, ADPCM_LOOP), but they
parse the SM64/Ocarina Audiobank format, not the SDK n_audio structures
Kirby 64 uses, so audio is extracted as BLOBs (see docs/PC_PORT_ASSETS.md).
Kirby 64's audio blocks are the `bin` subsegments in
kirby64.yaml (`sound/ctl_2A8CB0`, `sound/ctl_3E1400` and the `sound/sound_*`
banks).

