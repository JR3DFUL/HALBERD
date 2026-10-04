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

Measured 2026-10-04 (Mesa llvmpipe under Xvfb, no GPU, 4 cores shared with
other jobs at a load average of about 8). Driven by the scripted controller
(`KIRBY_PC_INPUT=walk`) the port boots through the logos, opening movie,
title screen, file select (saves persist to `kirby64.eep`), galaxy map and
planet map into world 1-1 and reaches gameplay at 52 s of a 240 s run; the
level, its sky, HUD, Kirby (with his face) and enemies render
(`render=raster`, 14468 frames drawn, 1791 of 1808 sampled frames
non-blank), the player walks to the first ledge, and the run ends without a
fault. Characters' faces draw in the opening movie and on the title screen.
Not working: audio (a stand-in thread; nothing plays); the analog stick does
not move the player (the ROM's own behaviour -- use the D-pad); and a number
of game-side functions carry behavioural (non-matching) PORT implementations
pending genuine matches. `docs/PC_PORT_LIBULTRASHIP.md` carries the verdict
lines and what is behind each item.

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

Go to your repo folder (drive C:\ is `/mnt/c` in WSL, D:\ is `/mnt/d`, ...):

    cd /mnt/c/path/to/HALBERD

then build:

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
| Return | START |
| X / C / Z | A / B / Z |
| arrow keys | **D-pad — this is what moves Kirby** |
| W A S D | control stick |
| I J K L | C buttons (up / left / down / right) |
| A / S | L / R (they double as stick left/down) |

**Move with the arrow keys, not WASD.** The analog stick genuinely does not
move the player: Kirby 64 copies the stick value into the player's controller
record and never reads it back — verified both at runtime and by scanning the
whole ROM image, see `docs/PC_PORT_LIBULTRASHIP.md`. Walking is the D-pad, on
hardware as much as here. WASD is still mapped because menus and other code
paths may use the stick, and because a real gamepad's stick lands in the same
place.

Return at the title, X through the file menu (X twice on an empty slot:
first creates the save, second starts it), arrow keys + X on the maps.

On WSL the window appears on the Windows desktop automatically. If the
game is audibly/visibly running in the terminal but no window shows,
Windows' WSL display layer is wedged -- `wsl --shutdown` from cmd and
reopen, or reboot once.

## Updating

Pull the latest (Fetch/Pull in GitHub Desktop, or):

    git pull

then rebuild:

    ./build.sh baserom.us.z64

The build reuses everything already compiled and re-stages only what
changed. `third_party/` and `out/` are yours -- git never touches them.

## Building against your own decomp checkout

`build.sh` fetches the decomp at the commit pinned in `DECOMP_REF` (a full
SHA; the default is set at the top of the script). No patch is applied: the
decomp's `decomp-clean` branch already carries every `#ifdef PORT` arm the
port compiles. Two overrides:

    DECOMP_REF=<full sha> ./build.sh baserom.us.z64
    DECOMP_DIR=/path/to/kirby64_decomp ./build.sh baserom.us.z64

`DECOMP_DIR` builds inside an existing checkout instead of cloning one. That
checkout is never reset, cleaned or checked out; only the port overlay
(`src/pc`, `tools/pc`, `port/`, `Makefile.pc`) is copied in, and a checkout
that already symlinks those paths back at this repo is left alone.
