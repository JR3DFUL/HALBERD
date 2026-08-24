#!/usr/bin/env python3
"""Link the native binary, or say exactly why it cannot be linked yet.

    make -f Makefile.pc link          this, with the headless backend
    python3 tools/pc/link.py --run    link and then run it

HOW THIS DIFFERS FROM tools/pc/link.sh, WHICH IS STILL HERE AND STILL USEFUL.
link.sh gives every missing symbol a weak abort-on-call stub, so it always
produces a binary and finds out at RUNTIME which absent function the game
reaches first. That is a good exploration tool and a bad build target: a weak
stub lets a function that was never decompiled be silently "ported", which the
project's governing rule forbids, and a stub that returns zero produces a
binary that is wrong rather than one that will not link.

So this tool never stubs anything. It links what exists and reports what does
not, using the same classification as tools/pc/gap.py so the two can never
disagree.

EXIT STATUS IS A DELIBERATE CHOICE.

  0   the binary linked, OR the only unresolved symbols are game functions the
      decompilation has not produced yet. The second case is the project's
      normal state, not a build failure -- `make -f Makefile.pc` on a clean
      checkout must not look broken because the decompilation is not finished.
      It prints the count and the names.
  1   something else is missing (a platform-layer regression, an unresolved
      data symbol, a --defsym whose base disappeared), or the link failed for
      a reason that is not a missing symbol. That is a real failure and it
      says which symbols and which category.

THE BACKEND. Only the two C backends are built by Makefile.pc, and this links
the one it built -- headless by default, SDL2 with PC_SDL=1. The libultraship
backend is C++, is compiled separately by tools/pc/build_lus_backend.sh, and
needs a checkout of that library that a clean tree does not have; point
LUS_ROOT/LUS_BUILD at it and use tools/pc/link.sh for that path.
"""
import glob, os, re, subprocess, sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.chdir(REPO)
sys.path.insert(0, os.path.join(REPO, 'tools', 'pc'))
import gap as gapmod

OUT = 'build/pc/kirby64'

# -no-pie is load-bearing, not a preference. See the low-memory note in
# src/pc/pc_mmio.c: game code stores host pointers in 32-bit fields
# (src/main/dma.c casts a void* to u32 before handing it to osEPiStartDma),
# and that truncation is only lossless while every game-visible address fits
# in 32 bits. A no-pie image loads at 0x400000, so all statics do, and
# pc_check_low_memory() aborts at startup if that ever stops being true.
CC = ['gcc', '-m64', '-no-pie', '-fno-pie']


def sh(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def defined_globals(objs):
    out = sh(['nm', '-g', '--defined-only'] + objs).stdout
    names = set()
    for line in out.split('\n'):
        p = line.split()
        if len(p) == 3 and p[1] not in 'Uu':
            names.add(p[2])
    return names


def drop_superseded(lu_objs, game_syms):
    """Reuse of libreultra is per translation unit, so decide per TU.

    The decompilation keeps landing functions that a reused libreultra object
    also defines. A static list of which ones to drop goes stale the moment
    another lane matches a function, so ask the linker's question instead.

      * every global superseded -> drop the object.
      * none superseded        -> link it as it is.
      * SOME superseded        -> the half-superseded case, which used to be
        reported as "needs a human" and stopped the link dead. It does not
        need a human. The two definitions are the same routine -- Kirby's copy
        of libnaudio and Nintendo's copy of libnaudio -- so the question is
        only which one wins, and the answer is always the game's: it is the
        one the ROM actually runs. Weakening libreultra's copy says exactly
        that to the linker and keeps the rest of the translation unit.

        n_event.o is the live example: the decompilation has landed
        n_alEvtqPostEvent, and the same object still supplies n_alEvtqNew,
        n_alEvtqNextEvent and n_alEvtqFlushType, which nothing else does.
        Dropping it would open three new gaps to close a duplicate.
    """
    keep, notes = [], []
    weakdir = 'build/pc/lu_weak'
    for o in lu_objs:
        syms = {s for s in defined_globals([o]) if not s.startswith('__x86')}
        hit = syms & game_syms
        if not hit:
            keep.append(o)
        elif hit == syms:
            notes.append(f'superseded by the decompilation, dropped: {o}')
        else:
            os.makedirs(weakdir, exist_ok=True)
            w = os.path.join(weakdir, os.path.basename(o))
            cmd = ['objcopy'] + [f'--weaken-symbol={s}' for s in sorted(hit)] \
                + [o, w]
            r = sh(cmd)
            if r.returncode:
                sys.stderr.write(r.stderr)
                notes.append(f'PARTIAL COLLISION and objcopy failed, dropped: '
                             f'{o} -- ' + ', '.join(sorted(hit)))
                continue
            keep.append(w)
            notes.append(f'half-superseded, the game\'s copy wins: {o} -- '
                         + ', '.join(sorted(hit)))
    return keep, notes


def main():
    argv = sys.argv[1:]

    game = sorted(glob.glob('build/pc/src/main/*.o')
                  + glob.glob('build/pc/src/ovl*/*.o')
                  + glob.glob('build/pc/src/pc/*.o')
                  + glob.glob('build/pc/data/*.o'))
    lu = sorted(glob.glob('build/pc/src/libreultra/*.o'))
    if not game:
        raise SystemExit('no objects -- run `make -f Makefile.pc objs` first')

    # pc_backend_null.c and pc_backend_sdl.c are #ifdef'd against each other,
    # so exactly one of them has any content; both are compiled and linking
    # both is harmless. pc_backend_lus.cpp is not built by Makefile.pc at all.
    game_syms = defined_globals(game)
    lu, notes = drop_superseded(lu, game_syms)
    for n in notes:
        print(n)

    host = 'build/pc/hostmain.o'
    r = sh(CC + ['-w', '-c', 'tools/pc/hostmain.c', '-o', host])
    if r.returncode:
        sys.stderr.write(r.stderr)
        raise SystemExit('link.py: could not compile tools/pc/hostmain.c')

    defsyms = []
    if os.path.exists('build/pc/defsyms.txt'):
        for line in open('build/pc/defsyms.txt'):
            line = line.strip()
            if line:
                defsyms.append('-Wl,' + line.replace(' ', ','))

    # -rdynamic puts every global into .dynsym, which is the only way
    # backtrace_symbols_fd() in src/pc/pc_progress.c can print a NAME instead
    # of a bare address. Without it a crash report is a column of hex that has
    # to be fed to addr2line by hand, which defeats the point of trapping the
    # signal in-process. It costs a larger symbol table and nothing else: the
    # image is still -no-pie and still loads at 0x400000, which the low-memory
    # note above depends on.
    cmd = CC + ['-rdynamic', '-o', OUT] + game + lu + [host] + defsyms + ['-lm']
    r = sh(cmd)
    if r.returncode == 0:
        print(f'linked {OUT} ({os.path.getsize(OUT)} bytes)')
        if '--run' in argv:
            print('--- running ---')
            subprocess.run(['timeout', '30', OUT])
        return 0

    return report(r.stderr)


def report(stderr):
    """Turn ld's output into the one fact that matters: what is missing."""
    missing = sorted(set(
        re.findall(r"undefined reference to [`']([^'\"]+)'", stderr)))
    # A --defsym whose right-hand side no longer exists fails differently.
    badsym = sorted(set(re.findall(
        r"undefined symbol [`']([^'\"]+)' referenced in expression", stderr)))

    # Anything ld said that is NOT one of those two shapes -- a multiple
    # definition, a relocation overflow, a missing library -- has to be shown
    # verbatim. An earlier version of this function extracted the undefined
    # references and printed only those, which hid a genuine "multiple
    # definition of n_alEvtqPostEvent" behind a tidy list of missing functions
    # and made a broken link look like a waiting one.
    BENIGN = ('undefined reference to',      # the message itself
              'referenced in expression',    # a --defsym with a dead base
              'in function `',               # its location line
              'more undefined references',   # ld collapsing repeats
              'ld returned 1 exit status')   # collect2's epilogue
    residue = [l for l in stderr.split('\n')
               if l.strip() and not any(b in l for b in BENIGN)]
    if residue:
        print('ld reported more than missing symbols:')
        for l in residue[:40]:
            print('  ' + l)
        if len(residue) > 40:
            print(f'  ... and {len(residue) - 40} more line(s)')

    if not missing and not badsym:
        sys.stderr.write(stderr)
        print('\nlink FAILED for a reason that is not a missing symbol; '
              'ld\'s own output is above.')
        return 1

    buckets, _host, _bad = gapmod.buckets_of(missing + badsym)
    named = {n: h for n, h in buckets}
    owed = named['un-decompiled functions']
    other = [(n, h) for n, h in buckets
             if h and n not in ('un-decompiled functions',
                                'supplied by the host link')]

    if badsym:
        print('DEFSYM BASE SYMBOL GONE -- tools/pc/gen_defsyms.py pointed a')
        print('--defsym at a symbol that no longer exists. Re-run it after the')
        print('data listings changed:')
        for s in badsym:
            print(f'    {s}')

    if other:
        print('\nNOT LINKED. Symbols are missing that are NOT un-decompiled '
              'game functions,')
        print('which means something in the platform layer or the data '
              'translation regressed:')
        for n, h in other:
            print(f'  {n} ({len(h)})')
            for s in h:
                print(f'      {s}')

    if owed:
        print(f'\nNOT LINKED YET: {len(owed)} game function(s) the '
              f'decompilation has not produced.')
        print('Nothing in the port can supply these -- a stub here would be a '
              'wrong binary,')
        print('and would let a function count as ported that was never '
              'decompiled. They are:')
        for s in owed:
            print(f'      {s}')

    if other or badsym or residue:
        return 1
    print('\nEverything else resolved. When the last of those lands, this '
          'links.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
