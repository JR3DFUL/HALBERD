#!/usr/bin/env bash
# kirby64_port one-command bootstrap (Linux / WSL2).
#
#   ./build.sh /path/to/baserom.us.z64
#
# Produces ./out/halberd plus staged assets, then prints the run command.
# Every stage is idempotent: rerun after a failure and it resumes.
#
# STATUS: beta. This encodes the exact chain the development container uses.
#
# The port reaches GAMEPLAY and renders. Driven by the scripted controller
# (KIRBY_PC_INPUT=walk) it runs logos -> opening movie -> title -> file select
# -> galaxy map -> planet map -> world 1-1, and draws the level and Kirby.
# See README and docs/PC_PORT_LIBULTRASHIP.md for what is proven by running
# rather than merely wired.
#
# DECOMP SOURCES. Step 4 fetches JR3DFUL/kirby64_decomp at the pinned commit
# DECOMP_REF and copies this repository's port overlay (src/pc, tools/pc,
# port/, Makefile.pc) into it. The decomp's published decomp-clean branch
# carries every PORT arm the port compiles, so no patch is applied. Knobs:
#
#   DECOMP_REF=<sha>   build against another decomp commit. Full 40-digit
#                      SHA: a shallow fetch by commit needs all of it.
#   DECOMP_DIR=/path   build inside an existing decomp checkout instead of
#                      cloning one. It is never reset, cleaned or checked out;
#                      only the overlay is copied in, and a checkout that
#                      already symlinks the overlay back at this tree is left
#                      exactly as it is.
set -euo pipefail

ROM=${1:-baserom.us.z64}
ROM_SHA=6cea2d46b929a3bb347b060a77fccc83526fb855
ROOT=$(cd "$(dirname "$0")" && pwd)
WORK=$ROOT/third_party
OUT=$ROOT/out
JOBS=$(nproc)

DECOMP_URL=https://github.com/JR3DFUL/kirby64_decomp
DECOMP_REF=${DECOMP_REF:-ea8acdc001187851a574135358843292117b7fe1}

msg() { printf '\n== %s ==\n' "$*"; }
# True when both paths resolve to the same file or directory (symlinks
# followed). A missing path resolves to itself and so never matches.
same_path() { [ "$(readlink -f "$1" 2>/dev/null)" = "$(readlink -f "$2" 2>/dev/null)" ]; }

msg "0/7 ROM check"
[ -f "$ROM" ] || { echo "ROM not found: $ROM (pass the path as arg 1)"; exit 1; }
got=$(sha1sum "$ROM" | cut -c1-40)
[ "$got" = "$ROM_SHA" ] || { echo "ROM sha1 $got != $ROM_SHA (need US 1.0 dump)"; exit 1; }

msg "1/7 host dependencies"
need="git cmake ninja-build build-essential pkg-config libudev-dev libgl1-mesa-dev libx11-dev libxext-dev libasound2-dev libpulse-dev libzip-dev nlohmann-json3-dev libtinyxml2-dev libspdlog-dev python3"
if command -v apt-get >/dev/null; then
    sudo apt-get install -y $need || apt-get install -y $need
else
    echo "install equivalents of: $need"; fi
# libzip's CMake package imports zipcmp/zipmerge/ziptool as executables and
# hard-errors at configure time when they are absent, although libultraship
# never runs them. Ubuntu's libzip-dev does not ship them (libzip-tools does,
# where it exists). An empty script at each path satisfies the import.
for t in zipcmp zipmerge ziptool; do
    [ -e "/usr/bin/$t" ] && continue
    printf '#!/bin/sh\nexit 0\n' | sudo tee "/usr/bin/$t" >/dev/null
    sudo chmod +x "/usr/bin/$t"
done

mkdir -p "$WORK" "$OUT"

msg "2/7 SDL2 (from source; the renderer fork is SDL2-only)"
if [ ! -f "$WORK/sdl2-install/lib/libSDL2.so" ]; then
    [ -d "$WORK/SDL2" ] || git clone --depth 1 -b release-2.30.11 https://github.com/libsdl-org/SDL "$WORK/SDL2"
    cmake -S "$WORK/SDL2" -B "$WORK/sdl2-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
          -DSDL_OPENGL=ON -DSDL_X11=ON -DSDL_SHARED=ON -DSDL_STATIC=ON \
          -DCMAKE_INSTALL_PREFIX="$WORK/sdl2-install"
    ninja -C "$WORK/sdl2-build" -j"$JOBS" install
fi

msg "3/7 libultraship (JRickey ssb64 fork + Kirby patch)"
if [ ! -d "$WORK/libultraship" ]; then
    git clone --depth 1 -b ssb64 https://github.com/JRickey/libultraship "$WORK/libultraship"
fi
# Re-apply the current patch every run (reset first) so a pulled patch update
# actually reaches the build; ninja then recompiles only what the patch
# touched.
git -C "$WORK/libultraship" checkout -- . 2>/dev/null || true
git -C "$WORK/libultraship" apply "$ROOT/patches/libultraship-jrickey-kirby.patch" || \
    { echo "libultraship-jrickey-kirby.patch failed to apply"; exit 1; }
if [ ! -f "$WORK/lus-build/build.ninja" ]; then
    cmake -S "$WORK/libultraship" -B "$WORK/lus-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
          -DGBI_UCODE=F3DEX_GBI_2 -DLUS_BUILD_TESTS=OFF \
          -DCMAKE_PREFIX_PATH="$WORK/sdl2-install"
fi
ninja -C "$WORK/lus-build" -j"$JOBS" libultraship

msg "4/7 decomp sources (game code)"
if [ -n "${DECOMP_DIR:-}" ]; then
    DECOMP=$(cd "$DECOMP_DIR" && pwd)
    [ -f "$DECOMP/include/ultra64.h" ] || \
        { echo "DECOMP_DIR=$DECOMP_DIR is not a kirby64_decomp checkout"; exit 1; }
    echo "using existing checkout $DECOMP (DECOMP_REF ignored)"
else
    DECOMP=$WORK/kirby64_decomp
    if [ ! -d "$DECOMP/.git" ]; then
        git init -q "$DECOMP"
        git -C "$DECOMP" remote add origin "$DECOMP_URL"
    fi
    # This clone belongs to the script, so moving it to DECOMP_REF may discard
    # (-f) whatever an earlier run left in tracked files. It never touches the
    # overlay: the decomp does not track src/pc, tools/pc, port/ or Makefile.pc.
    # The fetch is skipped when the commit is already here, so a rerun works
    # offline.
    if ! git -C "$DECOMP" rev-parse -q --verify "$DECOMP_REF^{commit}" >/dev/null 2>&1; then
        git -C "$DECOMP" fetch --depth 1 origin "$DECOMP_REF"
        git -C "$DECOMP" checkout -q -f --detach FETCH_HEAD
    else
        git -C "$DECOMP" checkout -q -f --detach "$DECOMP_REF"
    fi
    # libreultra is a submodule and the PC build compiles against its headers
    # (include/ultra64.h pulls PR/os_cont.h from libreultra/include/2.0I).
    git -C "$DECOMP" submodule update --init --depth 1 libreultra
fi
# The overlay. src/pc and tools/pc are replaced rather than merged: Makefile.pc
# compiles every file under src/pc, so a file deleted from the port would
# otherwise linger from an earlier run and still be built. port/ is merged,
# because it only carries Torch manifests and the staged shader, and a
# developer's checkout may hold extracted assets under port/o2r that this
# script did not put there.
for p in src/pc tools/pc Makefile.pc; do
    same_path "$ROOT/$p" "$DECOMP/$p" && continue
    rm -rf "${DECOMP:?}/$p"
    mkdir -p "$(dirname "$DECOMP/$p")"
    cp -r "$ROOT/$p" "$DECOMP/$p"
done
if ! same_path "$ROOT/port" "$DECOMP/port"; then
    mkdir -p "$DECOMP/port"
    cp -r "$ROOT/port/." "$DECOMP/port/"
fi
same_path "$ROM" "$DECOMP/baserom.us.z64" || cp "$ROM" "$DECOMP/baserom.us.z64"
same_path "$ROM" "$OUT/baserom.us.z64"    || cp "$ROM" "$OUT/baserom.us.z64"

msg "5/7 game build + link"
( cd "$DECOMP" && make -f Makefile.pc -j"$JOBS" )
( cd "$DECOMP" && \
  LUS_ROOT="$WORK/libultraship" LUS_BUILD="$WORK/lus-build" \
  SDL2_PREFIX="$WORK/sdl2-install" bash tools/pc/link.sh )
cp "$DECOMP/build/pc/kirby64" "$OUT/halberd"

msg "6/7 assets (Torch o2r + Fast3D shaders)"
if [ ! -f "$WORK/torch-build/torch" ]; then
    [ -d "$WORK/torch" ] || git clone --depth 1 -b ssb64 https://github.com/JRickey/Torch "$WORK/torch" \
        || git clone --depth 1 https://github.com/JR3DFUL/Torch "$WORK/torch"
    cmake -S "$WORK/torch" -B "$WORK/torch-build" -G Ninja -DCMAKE_BUILD_TYPE=Release
    ninja -C "$WORK/torch-build" -j"$JOBS" torch
fi
mkdir -p "$OUT/port/o2r" "$OUT/port/assets/shaders/opengl"
( cd "$DECOMP" && "$WORK/torch-build/torch" o2r baserom.us.z64 -s port/yamls -d "$OUT/port/o2r" ) || \
  echo "WARN: torch o2r failed -- game runs, textures may be limited"
cp "$WORK/libultraship/src/fast/shaders/opengl/default.shader.glsl" "$OUT/port/assets/shaders/opengl/"

msg "7/7 launcher"
cat > "$OUT/run.sh" <<LAUNCH
#!/bin/sh
# Launch HALBERD. Everything is baked in; no environment setup needed.
cd "\$(dirname "\$0")"
export LD_LIBRARY_PATH="$WORK/sdl2-install/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
export KIRBY_PC_TRACE=\${KIRBY_PC_TRACE:-1}
exec ./halberd "\$@"
LAUNCH
chmod +x "$OUT/run.sh"

cat <<EOT

Build complete.

Run the game:
    $OUT/run.sh

Headless/debug extras: KIRBY_PC_SCHEDDEBUG=1, KIRBY_PC_BGDEBUG=1 (stderr diagnostics).
EOT
