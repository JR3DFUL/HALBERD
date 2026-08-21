#!/bin/sh
# Cross-compile the PC port to a native Windows x86_64 exe with mingw-w64.
#
# ADDITIVE: this script owns build/pcw/ and touches nothing the Linux build
# owns (build/pc/ is read for the generated data C files and stubs.c, both
# products of `make -f Makefile.pc data` + tools/pc/gen_stubs.py, which must
# have run at least once). Makefile.pc and tools/pc/link.sh are the source of
# truth for flags and link logic; this file transcribes them for mingw and
# says so where it deviates.
#
# THE LOAD-BEARING LINK FLAGS. The game stores host pointers in 32-bit fields
# (src/main/dma.c), so every game-visible address must fit in 32 bits. On
# Linux that is -no-pie (image at 0x400000). The PE equivalent is BOTH of:
#
#     --image-base=0x400000     put the image where -no-pie would
#     --disable-dynamicbase     forbid ASLR from moving it anyway
#
# Verified (wine, 2026-08-14): statics, bss, and CRT-heap allocations up to
# 16 MB all land below 4 GiB under these flags; pc_check_low_memory() then
# guards it at every startup forever.
#
# WINDOWS SOURCE SUBSTITUTIONS (see scratchpad win32_inventory.md):
#     src/pc/os_thread.c    -> src/pc/os_thread_win32.c   (ucontext -> fibers)
#     src/pc/pc_setjmp.c    -> src/pc/win32/pc_setjmp_win64.c (SysV -> MS x64)
# and three TUs get -include src/pc/win32/pc_win32_shim.h (errno-field vs
# mingw's errno macro; sigaction; setitimer). src/pc/win32 is on the include
# path everywhere so <sys/mman.h> resolves to the VirtualAlloc shim.
#
# Usage: tools/pc/build_win32.sh [--run]
#     PC_SDL=1        link the SDL2 video backend instead of the null one
#     SDL2_MINGW=dir  SDL2 mingw dev tree (the x86_64-w64-mingw32 subdir of
#                     SDL2-devel-2.x-mingw.tar.gz); required for PC_SDL=1
#     JOBS=n          parallel compiles (default: nproc)
set -e
cd "$(dirname "$0")/../.."

CROSS=x86_64-w64-mingw32
CC=$CROSS-gcc
NM=$CROSS-nm
B=build/pcw
OUT=$B/halberd.exe
JOBS=${JOBS:-$(nproc)}
PC_SDL=${PC_SDL:-}

DEFS="-D_LANGUAGE_C -DTARGET_N64 -DPORT -DF3DEX_GBI_2 -DAVOID_UB -DNON_MATCHING"
# _SSIZE_T_DEFINED: include/PR/ultratypes.h typedefs ssize_t itself (as long,
# which IS 32/64-correct enough here -- nothing in the tree stores sizes
# through it); this define tells mingw's corecrt.h to skip its own conflicting
# typedef instead of erroring. The game header cannot be edited (matching).
DEFS="$DEFS -D_SSIZE_T_DEFINED"
ARCH="-m64 -std=gnu90 -fsigned-char -g -O1 -w"

GAME_INC="-Isrc/pc/win32 -Iinclude -Iinclude/libc -Ilibreultra/include/2.0I \
          -Ibuild -Ibuild/include -Ibuild/assets -Isrc -I."
PLAT_INC="-Isrc/pc/win32 -Iinclude -Isrc -I. \
          -idirafter libreultra/include/2.0I -idirafter include/libc \
          -idirafter build -idirafter build/include -idirafter build/assets"
LU_INC="-include tools/pc/lu_lp64.h \
        -Ilibreultra/include/2.0I -Ilibreultra/include/2.0I/PR -Ilibreultra/src \
        -Ilibreultra/src/gu -Ilibreultra/src/io -Ilibreultra/src/audio \
        -Ilibreultra/src/libnaudio -Iinclude -I."
SHIM="-include src/pc/win32/pc_win32_shim.h"

if [ "$PC_SDL" = "1" ]; then
    : "${SDL2_MINGW:?PC_SDL=1 needs SDL2_MINGW=<path to x86_64-w64-mingw32 tree>}"
    PLAT_DEFS="-DPC_SDL -I$SDL2_MINGW/include/SDL2"
else
    PLAT_DEFS=""
fi

mkdir -p $B/src $B/data $B/libreultra

# --------------------------------------------------------------- compile
# One job list, xargs-parallel. Entries are "flags|src|obj" with | as the
# separator (no | in any path here).
JOBLIST=$B/.jobs
: > $JOBLIST

for f in src/*/*.c; do
    d=$(dirname "$f")
    case "$f" in
        src/pc/os_thread.c|src/pc/pc_setjmp.c) continue ;; # substituted
        src/pc/win32/*) continue ;;                        # handled below
        src/pc/os_cont.c|src/pc/os_time.c|src/pc/pc_dbg.c)
            echo "$ARCH $DEFS $SHIM $PLAT_DEFS $PLAT_INC|$f|$B/$d/$(basename $f .c).o" ;;
        src/pc/pc_backend_sdl.c)
            echo "$ARCH $DEFS $SHIM $PLAT_DEFS $PLAT_INC|$f|$B/$d/$(basename $f .c).o" ;;
        src/pc/*)
            echo "$ARCH $DEFS $PLAT_DEFS $PLAT_INC|$f|$B/$d/$(basename $f .c).o" ;;
        *)  echo "$ARCH $DEFS $GAME_INC|$f|$B/$d/$(basename $f .c).o" ;;
    esac >> $JOBLIST
done
echo "$ARCH $DEFS $PLAT_INC|src/pc/win32/pc_setjmp_win64.c|$B/src/pc/pc_setjmp_win64.o" >> $JOBLIST

# The generated data files carry __asm__ blocks that use ELF-only
# .pushsection/.popsection (see render_word32_asm in tools/pc/gen_data.py and
# its comment about GCC's section tracking). COFF gas has neither. Two-part
# equivalent: rewrite to a plain .section switch in a build/pcw COPY (the
# Linux artifact is not touched), and compile with -fdata-sections, which
# makes GCC emit an explicit section directive for EVERY object -- so the
# next compiler-emitted object can never land in the section the asm left
# behind, which is the desync .popsection existed to prevent.
mkdir -p $B/datasrc
for f in build/pc/data/*.c; do
    [ -e "$f" ] || { echo "no build/pc/data/*.c -- run: make -f Makefile.pc data" >&2; exit 1; }
    bn=$(basename $f)
    sed -e 's/\.pushsection \.rodata/.section .rdata,\\"dr\\"/' \
        -e 's/\.pushsection \.data/.section .data/' \
        -e 's/"   \.popsection\\n"/""/' "$f" > $B/datasrc/$bn
    echo "-m64 -std=gnu90 -g -w -fdata-sections -Isrc -D_SSIZE_T_DEFINED|$B/datasrc/$bn|$B/data/$(basename $bn .c).o" >> $JOBLIST
done

for n in gu/mtxcatf gu/mtxutil gu/normalize gu/us2dex \
         io/vimodentsclan1 io/vimodempallan1 io/vimodefpallan1 \
         audio/cents2ratio audio/copy \
         libnaudio/n_cspsetfxmix libnaudio/n_cspsetpriority \
         libnaudio/n_event libnaudio/n_synsetvol; do
    o=$(echo "$n" | tr / _)
    echo "$ARCH $DEFS $LU_INC|libreultra/src/$n.c|$B/libreultra/$o.o" >> $JOBLIST
done
for n in xprintf xldtob xlitob; do
    echo "$ARCH $DEFS -include stddef.h -Ilibreultra/src/libc $LU_INC|libreultra/src/libc/$n.c|$B/libreultra/lc_$n.o" >> $JOBLIST
done

mkdir -p $(awk -F'|' '{print $3}' $JOBLIST | xargs -n1 dirname | sort -u)
cat > $B/.cc1.sh <<CC1
#!/bin/sh
line=\$1
flags=\${line%%|*}; rest=\${line#*|}
src=\${rest%%|*};   obj=\${rest#*|}
$CC \$flags -c "\$src" -o "\$obj" || { echo "FAILED: \$src" >&2; exit 255; }
CC1
if ! xargs -P "$JOBS" -d '\n' -I{} sh $B/.cc1.sh {} < $JOBLIST 2> $B/.compile_errs; then
    echo "--- compile failures ---"; cat $B/.compile_errs; exit 1
fi

[ -e build/pc/stubs.c ] || { echo "no build/pc/stubs.c -- run tools/pc/link.sh once" >&2; exit 1; }
$CC $ARCH -c tools/pc/hostmain.c -o $B/hostmain.o

python3 tools/pc/gen_defsyms.py -o $B/defsyms.txt >/dev/null
# The own-image DMA guard (src/pc/os_pi.c) reads three ELF linker symbols.
# PE ld does not provide them but provides equivalents: __ImageBase is the
# image start, and __data_start__ is the first writable byte -- which is also
# a safe (over-)estimate for _etext, since the guard only needs
# text < _etext <= first-writable-byte. Same --defsym channel as the data
# aliases so the stub generator's accounting sees them too.
cat >> $B/defsyms.txt <<'EOF'
--defsym __executable_start=__ImageBase
--defsym _etext=__data_start__-1
--defsym __data_start=__data_start__
EOF

# ------------------------------------------------- assemble the object list
GAME_OBJS=$(ls $B/src/main/*.o $B/src/ovl*/*.o $B/src/pc/*.o $B/data/*.o | tr '\n' ' ')
LU_OBJS=$(ls $B/libreultra/*.o | tr '\n' ' ')

# Same superseded-TU logic as tools/pc/link.sh, with the cross nm.
$NM -g --defined-only $GAME_OBJS 2>/dev/null | awk 'NF>=3 && $2!="U"{print $3}' | sort -u > $B/.gamesyms
KEEP=""
for o in $LU_OBJS; do
    $NM -g --defined-only "$o" 2>/dev/null | awk 'NF>=3 && $2!="U"{print $3}' \
        | grep -v '^__x86' | sort -u > $B/.lusyms
    n=$(wc -l < $B/.lusyms); c=$(comm -12 $B/.lusyms $B/.gamesyms | wc -l)
    if [ "$c" -eq 0 ]; then KEEP="$KEEP $o"
    elif [ "$c" -eq "$n" ]; then echo "superseded, dropped: $o"
    else echo "PARTIAL COLLISION (needs a human): $o -- $c of $n"; fi
done
LU_OBJS="$KEEP"

# ------------------------------------------------------------ win32 stubs
# The Linux flow's stubs.c makes every stub WEAK so a real definition wins.
# binutils PE ld cannot do that: a COFF weak external whose only definition
# is the weak one does NOT resolve references from other TUs (verified with
# a two-file test; the .refptr data case fails identically). So the Windows
# stubs are STRONG, and the missing set is computed exactly, here, against
# THIS build's objects: stubs.c's per-symbol lines are reused (they carry
# the function-vs-data distinction) but filtered to symbols that are still
# undefined, and de-weakened. Anything stubs.c does not cover (symbols
# glibc provides and mingw lacks) gets a strong abort-on-call residual stub.
$NM -u $GAME_OBJS $LU_OBJS $B/hostmain.o 2>/dev/null \
    | awk '$1=="U"{print $2}' | sort -u > $B/.undef
{ $NM -g --defined-only $GAME_OBJS $LU_OBJS $B/hostmain.o 2>/dev/null \
      | awk 'NF>=3 && $2!="U"{print $3}'
  awk '{sub(/^--defsym /,""); sub(/=.*/,""); print}' $B/defsyms.txt
  $NM -g --defined-only /usr/$CROSS/lib/*.a /usr/lib/gcc/$CROSS/*/libgcc*.a \
      /usr/lib/gcc/$CROSS/*/*.a 2>/dev/null | awk 'NF>=3 && $2!="U"{print $3}'
  [ "$PC_SDL" = "1" ] && $NM -g --defined-only "$SDL2_MINGW"/lib/*.a 2>/dev/null \
      | awk 'NF>=3 && $2!="U"{print $3}'
} | sort -u > $B/.def
comm -23 $B/.undef $B/.def | grep -v '^__imp_' > $B/.missing || true

python3 - "$B" <<'PYSTUB'
import re, sys
b = sys.argv[1]
missing = set(l.strip() for l in open(b + '/.missing') if l.strip())
out = ['/* GENERATED for the Windows link -- strong stubs (PE ld cannot',
       ' * resolve weak-only definitions across TUs), filtered to the',
       ' * symbols this exact object set leaves undefined. */']
covered = {'pc_stub_report'}  # defined by the preamble carried over below
line_re = re.compile(r'__attribute__\(\(weak\)\) (.*?\b(\w+)\s*[\[(].*)$')
for line in open('build/pc/stubs.c'):
    m = line_re.match(line.strip())
    if m is None:
        out.append(line.rstrip('\n'))     # preamble: reporter, helpers
        continue
    body, sym = m.group(1), m.group(2)
    if sym in missing:
        out.append(body)
        covered.add(sym)
residual = sorted(missing - covered)
if residual:
    out.append('static void pc_unimplemented2(const char *n) {')
    out.append('    fprintf(stderr, "\\n*** not implemented (win32): %s\\n", n);')
    out.append('    if (getenv("KIRBY_PC_TRACE") == NULL) exit(70);')
    out.append('}')
    for s in residual:
        out.append('long long %s(void) { pc_unimplemented2("%s"); return 0; }' % (s, s))
    print('win32 residual stubs: ' + ' '.join(residual))
open(b + '/stubs_win32.c', 'w').write('\n'.join(out) + '\n')
PYSTUB
$CC $ARCH -g0 -c $B/stubs_win32.c -o $B/stubs_win32.o

DEFSYMS=$(sed 's/^/-Wl,/; s/ /,/g' $B/defsyms.txt | tr '\n' ' ')

if [ "$PC_SDL" = "1" ]; then
    BACKEND_LIBS="-L$SDL2_MINGW/lib -lSDL2 -lwinmm -limm32 -lole32 -loleaut32 -lversion -lsetupapi -lgdi32"
else
    BACKEND_LIBS=""
fi

# shellcheck disable=SC2086
$CC -m64 -o "$OUT" $GAME_OBJS $LU_OBJS $B/stubs_win32.o $B/hostmain.o \
    $DEFSYMS \
    -Wl,--image-base=0x400000 -Wl,--disable-dynamicbase \
    -Wl,-Bstatic -lpthread -Wl,-Bdynamic \
    $BACKEND_LIBS -lm

echo "linked $OUT ($(stat -c%s "$OUT") bytes)"
$CROSS-objdump -x "$OUT" | grep -E 'ImageBase|DllCharacteristics'

if [ "$1" = "--run" ]; then
    echo "--- running (wine) ---"
    if [ "$PC_SDL" = "1" ] && [ -e "$SDL2_MINGW/bin/SDL2.dll" ]; then
        cp -n "$SDL2_MINGW/bin/SDL2.dll" $B/ 2>/dev/null || true
    fi
    (cd $B && WINEDEBUG=-all timeout 60 wine "$(basename $OUT)") || true
fi
