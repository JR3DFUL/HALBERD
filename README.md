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

**Early development**: the port currently plays from the title screen
through the menus and into running levels. See *Current state* below.

## About

This project exists to preserve Kirby 64 and open it up for the larger
Kirby fanbase to enjoy and build on.

Claude (ew) was used as a tool to bring the project to where it is today,
but contributions from people passionate about this game are always
welcome, and will always be valued over those of AI.

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

Boots, renders the intro movie and title screen, and takes input through
the full menu flow: file select (saves persist to `kirby64.eep`), the
opening cutscene, the world map and the planet level-select. Levels load
and run -- stage pipeline (config, collision, tracks, entities), enemy
spawning and the scene loop are live. The player-spawn chain (controllable
Kirby) and in-level rendering are in active development, as is game audio.
Some sprites draw untextured and a number of game-side functions carry
behavioral (non-matching) PORT implementations pending genuine matches --
see commit history for the running list.

## Relationship to the decomp

The game sources still live in `kirby64_decomp` and carry inert
`#ifdef PORT` arms where the port needs host-width/endianness variants
(the same convention BattleShip's decomp submodule uses). This repo holds
everything that is *only* about the PC build. Next structural step (per
BattleShip): decomp as a git submodule at `decomp/`, CMake superbuild,
libultraship + torch submodules with the patch applied on the fork.

## Install and build

Works on Linux, or on Windows through WSL2 (Ubuntu).

**Windows, one-time setup** -- in a regular `cmd` window:

    wsl --install -d Ubuntu

Reboot if asked, open "Ubuntu" from the Start menu, and create a username
and password when prompted. That password is what `sudo` asks for during
the build.

**Everyone** -- get the repo (clone it, or use GitHub Desktop), put your
ROM in the repo folder named `baserom.us.z64`, then from a WSL/Linux
terminal inside the repo folder:

    cd /mnt/c/path/to/HALBERD       # your repo folder; drive C:\ is /mnt/c in WSL
    ./build.sh baserom.us.z64

The script installs the compiler packages itself (asks for your password
once), then fetches and builds every dependency (SDL2, the patched
libultraship fork, the decomp game code, Torch), extracts assets from your
ROM, and finishes by writing the launcher. First build takes 10-30
minutes; it is resumable -- if anything fails, rerun the same line and it
continues where it stopped.

## Run

    ./out/run.sh

Keyboard mapping (N64 pad):

| Key | N64 |
|-----|-----|
| Space | START |
| X / C / Z | A / B / Z |
| W A S D | control stick |
| T G F H | D-pad |
| arrow keys | C buttons |
| E / R | L / R |

Space at the title, X through the file menu (X twice on an empty slot:
first creates the save, second starts it), T/G/F/H + X on the maps.

On WSL the window appears on the Windows desktop automatically. If the
game is audibly/visibly running in the terminal but no window shows,
Windows' WSL display layer is wedged -- `wsl --shutdown` from cmd and
reopen, or reboot once.

## Updating

    git pull
    ./build.sh baserom.us.z64

(or Fetch/Pull in GitHub Desktop, then the build line). The build reuses
everything already compiled and re-stages only what changed. `third_party/`
and `out/` are yours -- git never touches them.

## Building (manual, transitional)

The build currently expects to run from a `kirby64_decomp` checkout with this
repo's files overlaid at the same relative paths (that is how they were
developed this session). After the submodule restructure this section will be
replaced by the standard `cmake` flow.

1. `make -f Makefile.pc` — compiles game + host objects, generates stubs/data
2. `tools/pc/link.sh` — links `build/pc/kirby64` against the patched LUS fork
3. `torch o2r baserom.us.z64 -s port/yamls -d build/pc` then
   `tools/pc/stage_assets.sh` — builds and stages `kirby64.o2r` + shaders
