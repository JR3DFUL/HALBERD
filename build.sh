#!/usr/bin/env bash
# kirby64_port one-command bootstrap (Linux / WSL2).
#
#   ./build.sh /path/to/baserom.us.z64
#
# Produces ./out/halberd plus staged assets, then prints the run command.
# Every stage is idempotent: rerun after a failure and it resumes.
#
# STATUS: beta. This encodes the exact chain the development container uses.
# The port itself boots to the intro screens; see README for current state.
set -euo pipefail

ROM=${1:-baserom.us.z64}
ROM_SHA=6cea2d46b929a3bb347b060a77fccc83526fb855
ROOT=$(cd "$(dirname "$0")" && pwd)
WORK=$ROOT/third_party
OUT=$ROOT/out
JOBS=$(nproc)

msg() { printf '\n== %s ==\n' "$*"; }

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
# Re-apply the current patch every run (reset first, same as the decomp
# staging below) so a pulled patch update actually reaches the build; ninja
# then recompiles only what the patch touched.
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
if [ ! -d "$WORK/kirby64_decomp" ]; then
    git clone --depth 1 -b decomp-clean \
        https://github.com/JR3DFUL/kirby64_decomp "$WORK/kirby64_decomp"
    # libreultra is a submodule and the PC build compiles against its headers
    # (include/ultra64.h pulls PR/os_cont.h from libreultra/include/2.0I).
    git -C "$WORK/kirby64_decomp" submodule update --init --depth 1 libreultra
fi
# Overlay the port files onto the decomp checkout (they are gitignored there).
cp -r "$ROOT/src/pc"    "$WORK/kirby64_decomp/src/"
mkdir -p "$WORK/kirby64_decomp/tools"
cp -r "$ROOT/tools/pc"  "$WORK/kirby64_decomp/tools/"
cp    "$ROOT/Makefile.pc" "$WORK/kirby64_decomp/"
cp -r "$ROOT/port"      "$WORK/kirby64_decomp/"
# Reset the staged checkout's tracked files, then apply the current patch.
# The old apply-or-reverse-check dance broke the moment the patch was UPDATED:
# a tree carrying the previous patch can neither apply the new one nor
# reverse-check it. Resetting first makes any patch version apply cleanly.
git -C "$WORK/kirby64_decomp" checkout -- . 2>/dev/null || true
git -C "$WORK/kirby64_decomp" apply "$ROOT/patches/decomp-port.patch" || \
    { echo "decomp-port.patch failed to apply"; exit 1; }
cp "$ROM" "$WORK/kirby64_decomp/baserom.us.z64"
cp "$ROM" "$OUT/baserom.us.z64"

msg "5/7 game build + link"
( cd "$WORK/kirby64_decomp" && make -f Makefile.pc -j"$JOBS" )
( cd "$WORK/kirby64_decomp" && \
  LUS_ROOT="$WORK/libultraship" LUS_BUILD="$WORK/lus-build" \
  SDL2_PREFIX="$WORK/sdl2-install" bash tools/pc/link.sh )
cp "$WORK/kirby64_decomp/build/pc/kirby64" "$OUT/halberd"

msg "6/7 assets (Torch o2r + Fast3D shaders)"
if [ ! -f "$WORK/torch-build/torch" ]; then
    [ -d "$WORK/torch" ] || git clone --depth 1 -b ssb64 https://github.com/JRickey/Torch "$WORK/torch" \
        || git clone --depth 1 https://github.com/JR3DFUL/Torch "$WORK/torch"
    cmake -S "$WORK/torch" -B "$WORK/torch-build" -G Ninja -DCMAKE_BUILD_TYPE=Release
    ninja -C "$WORK/torch-build" -j"$JOBS" torch
fi
mkdir -p "$OUT/port/o2r" "$OUT/port/assets/shaders/opengl"
( cd "$WORK/kirby64_decomp" && "$WORK/torch-build/torch" o2r baserom.us.z64 -s port/yamls -d "$OUT/port/o2r" ) || \
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

cat <<EOF

Build complete.

Run the game:
    $OUT/run.sh

Headless/debug extras: KIRBY_PC_SCHEDDEBUG=1, KIRBY_PC_BGDEBUG=1 (stderr diagnostics).
EOF
