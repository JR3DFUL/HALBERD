#!/bin/sh
# Compiler and linker flags for the libultraship backend, in one place so the
# build script and any future Makefile agree.
#
#   lus_flags.sh --cflags
#   lus_flags.sh --libs
#
# TARGETS THE JRICKEY FORK (the BattleShip/SSB64 port's libultraship) built
# out of tree with CMake+Ninja. Its static library pulls in ImGui, prism and
# stb (fetched into $LUS_BUILD/_deps), the post-process transpiler stack
# (glslang + SPIRV-Cross), tinycc (libtcc/libtcc1, the mod-scripting
# compiler), hidapi-hidraw (raphnet N64 adapters; needs libudev on Linux),
# plus SDL2 (NOT SDL3 -- the fork is an SDL2 codebase), system spdlog, fmt,
# tinyxml2, libzip, nlohmann-json and OpenGL.
#
# The include list and the SPDLOG/FMT defines mirror what the fork's own TUs
# were compiled with (see DEFINES/INCLUDES for ship/Context.cpp.o in
# $LUS_BUILD/build.ninja); the ABI-relevant ones are SPDLOG_SHARED_LIB /
# FMT_SHARED (system spdlog+fmt are shared libraries) and F3DEX_GBI_2 (Gfx
# opcode encodings in the fork's gbi.h -- also what the game side defines).
set -e

LUS_ROOT=${LUS_ROOT:-/workspace/jrickey/libultraship}
LUS_BUILD=${LUS_BUILD:-/workspace/lus2-build}
SDL2_CONFIG=${SDL2_CONFIG:-${SDL2_PREFIX:-/workspace/sdl2-install}/bin/sdl2-config}
DEPS="$LUS_BUILD/_deps"

case "$1" in
--cflags)
    # ENABLE_OPENGL is harmless-but-honest (the library was built with it);
    # none of the fork's public headers change layout on it, but stating the
    # backend we link against costs nothing.
    echo "-std=gnu++20 -DENABLE_OPENGL -DF3DEX_GBI_2" \
         "-DSPDLOG_COMPILED_LIB -DSPDLOG_SHARED_LIB -DSPDLOG_FMT_EXTERNAL -DFMT_SHARED" \
         "-DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE" \
         "-I$LUS_ROOT/include -I$LUS_ROOT/src -I$LUS_BUILD/src -I$LUS_BUILD/include" \
         "-I$DEPS/imgui-src -I$DEPS/imgui-src/backends" \
         "-I$DEPS/stb -I$DEPS/threadpool-src/include" \
         "-I$DEPS/prism-src -I$DEPS/prism-src/lib -I$DEPS/prism-src/src" \
         "-I$DEPS/hidapi-src/hidapi" \
         "-I$DEPS/glslang-src -I$DEPS/spirv_cross-src" \
         "-I$DEPS/tinycc-build/safe_include" \
         "$($SDL2_CONFIG --cflags)" \
         "-I$("$SDL2_CONFIG" --prefix)/include"
    ;;
--libs)
    # --start-group because libultraship.a references ImGui, prism, stb, the
    # glslang/SPIRV-Cross stack and libtcc, and some of those reference each
    # other; a single pass leaves undefined symbols. The find covers every .a
    # the fork's build produces (glslang and SPIRV-Cross live under
    # _deps/*-build), so a fork update that adds a library is picked up
    # without editing this list. hidapi-hidraw is the Linux HID transport and
    # drags in libudev.
    echo "-Wl,--start-group" \
         "$LUS_BUILD/src/libultraship.a" \
         "$(find "$LUS_BUILD" -name '*.a' ! -name 'libultraship.a' | tr '\n' ' ')" \
         "-Wl,--end-group" \
         "$($SDL2_CONFIG --libs)" \
         "-ludev -lspdlog -lfmt -ltinyxml2 -lzip -lGL -ldl -lpthread"
    ;;
*)
    echo "usage: $0 --cflags|--libs" >&2
    exit 1
    ;;
esac
