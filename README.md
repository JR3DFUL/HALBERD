# kirby64_port

Native PC port of **Kirby 64: The Crystal Shards** (N64), built on the
[JR3DFUL/kirby64_decomp](https://github.com/JR3DFUL/kirby64_decomp)
decompilation, using [libultraship](https://github.com/JRickey/libultraship) (JRickey `ssb64` fork, the one shipping under BattleShip; patched per `patches/`)
for rendering/window/input and [Torch](https://github.com/JR3DFUL/Torch) for
build-time asset extraction.

Architecture modeled on [JRickey/BattleShip](https://github.com/JRickey/BattleShip)
(the SSB64 port with the same decomp + libultraship + Torch pillars).

## No copyrighted assets

Nothing of Nintendo's ships in this repo. Every byte of game data is extracted
at build time from a ROM you supply
(`baserom.us.z64`, sha1 `6cea2d46b929a3bb347b060a77fccc83526fb855`).

## Layout

| Path | What |
|------|------|
| `Makefile.pc` | LP64 host build of the decomp game code (`-DPORT -DNON_MATCHING`) |
| `src/pc/` | Host platform layer: libultra shims (os_*), LUS backend bridge, arena/RAM window, MMIO |
| `tools/pc/` | Stub/data generators, linker driver (`link.sh`), Torch yaml generator, asset staging |
| `port/yamls/` | Torch extraction manifests (10,583 resources) |
| `port/assets/` | Fast3D shader staged for the ResourceManager FolderArchive |
| `patches/libultraship-jrickey-kirby.patch` | Kirby additions to the JRickey (BattleShip) libultraship fork, the adopted renderer: `G_OBJ_LOADTXTR` handler (TLUT/block/tile), format-general `BgCopy` (CI8), shared state ops under the S2DEX table |
| `patches/libultraship-native-rom.patch` | HISTORICAL: the equivalent patch set against the previous JR3DFUL fork, kept for reference |
| `docs/` | Port architecture, asset pipeline, LUS integration, surface inventory |

## Current state (honest)

Boots through scene creation, runs an indefinite game loop with zero
unimplemented symbols on the boot path, executes real F3DEX2/S2DEX display
lists (textures, palettes, draws all flowing to GL). Frame presentation in the
headless harness is the active frontier. Four game-side functions carry
behavioral (non-matching) PORT implementations pending genuine matches:
`func_80154A40_ovl6`, `func_800A09AC`, `func_8009E8F4`, `func_800A9864`,
plus `func_800A9250` (bank relocator, big-endian aware).

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
ROM, and produces `out/kirby64` with a printed run command. Idempotent:
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
