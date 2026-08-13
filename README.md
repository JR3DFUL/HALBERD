<p align="center">
  <img src="assets/halberd-logo.png" alt="HALBERD" width="640">
</p>

# HALBERD

**HALBERD** is a PC port of **Kirby 64: The Crystal Shards** (N64, US release)
-- built on top of the
[JR3DFUL/kirby64_decomp](https://github.com/JR3DFUL/kirby64_decomp)
decompilation, using
[libultraship](https://github.com/JRickey/libultraship) for PC-native
rendering / input and [Torch](https://github.com/HarbourMasters/Torch) for
extracting assets out of the ROM at build time. The name is a nod to
[BattleShip](https://github.com/JRickey/BattleShip), the SSB64 port whose
architecture this project follows.

Runs natively on Linux (and Windows via WSL2); further platforms planned.

**Early development**: the port currently boots through the intro screens and
into the menu scene. See *Current state* below.

## No copyrighted assets are included in this repository

**None of Nintendo's assets (code, textures, audio, models, text, ROM data)
are checked into this repo or distributed with builds.** The port is a pure
C source tree; every byte of Nintendo-owned data is extracted at build time
from a ROM that *you* supply. If you do not own a legal copy of Kirby 64:
The Crystal Shards for the Nintendo 64, you cannot build or run this project.

You supply your own ROM. The canonical, supported dump:

| Version | SHA-1 |
|---------|-------|
| **US** -- NTSC-U | `6cea2d46b929a3bb347b060a77fccc83526fb855` |

If your dump does not match the hash, it will not build.

## Layout

| Path | What |
|------|------|
| `Makefile.pc` | LP64 host build of the decomp game code (`-DPORT -DNON_MATCHING`) |
| `src/pc/` | Host platform layer: libultra shims (os_*), LUS backend bridge, arena/RAM window, MMIO |
| `tools/pc/` | Stub/data generators, linker driver (`link.sh`), Torch yaml generator, asset staging |
| `port/yamls/` | Torch extraction manifests (10,583 resources) |
| `port/assets/` | Fast3D shader staged for the ResourceManager FolderArchive |
| `patches/libultraship-jrickey-kirby.patch` | Kirby additions to the libultraship fork this port builds against |
| `docs/` | Port architecture, asset pipeline, LUS integration, surface inventory |

## Current state

Boots and renders the intro screens, then loads the menu scene; menu
rendering and game audio are in active development. A handful of game-side
functions carry behavioral (non-matching) PORT implementations pending
genuine matches -- see commit history for the running list.

## Relationship to the decomp

The game sources still live in `kirby64_decomp` and carry inert
`#ifdef PORT` arms where the port needs host-width/endianness variants
(the same convention BattleShip's decomp submodule uses). This repo holds
everything that is *only* about the PC build. Next structural step (per
BattleShip): decomp as a git submodule at `decomp/`, CMake superbuild,
libultraship + torch submodules with the patch applied on the fork.

## Building — one command

    ./build.sh /path/to/baserom.us.z64

Linux or WSL2. Fetches and builds every dependency (SDL2, the patched
libultraship fork, the decomp game code, Torch), extracts assets from your
ROM, and produces `out/halberd` with a printed run command. Idempotent:
rerun after any failure and it resumes. Beta -- mirrors the development
container's exact chain.

## Building (manual, transitional)

The build currently expects to run from a `kirby64_decomp` checkout with this
repo's files overlaid at the same relative paths (that is how they were
developed this session). After the submodule restructure this section will be
replaced by the standard `cmake` flow.

1. `make -f Makefile.pc` — compiles game + host objects, generates stubs/data
2. `tools/pc/link.sh` — links `build/pc/kirby64` against the patched LUS fork
3. `torch o2r baserom.us.z64 -s port/yamls -d build/pc` then
   `tools/pc/stage_assets.sh` — builds and stages `kirby64.o2r` + shaders
