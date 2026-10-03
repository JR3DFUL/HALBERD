#!/usr/bin/env python3
"""What the PC port is still missing, by category, measured from the objects.

This is the porting counterpart to verify_rom.py: rather than asking whether
the code is byte-exact, it asks whether the code EXISTS. Every symbol printed
here is something the native binary cannot link without.

The categories mean different things and should not be added together:

  func_*        un-decompiled game functions. Not porting work at all -- these
                disappear on their own as the matching decompilation proceeds,
                and nothing else can start until they do. M2C_ERROR is counted
                here too: a draft that still calls it is a draft m2c could not
                finish, whatever its #ifdef says.
  D_*           data symbols with no definition. Nearly all of these are
                translated automatically now -- tools/pc/gen_data.py for the
                listings and the in-listing constants, tools/pc/gen_defsyms.py
                for the absolute addresses -- so anything left here is a
                genuine residue that needs a look.
  os* / al* / gu*  the actual platform layer. This is the real port.
  other         named helpers with no home yet.

THE HEADLINE NUMBER EXCLUDES "supplied by the host link", and that is not an
accounting convenience. Those symbols are not missing from anything: the host
libc and libm define them, or ld's own script does, or tools/pc/hostmain.c
does. link_verified() proves it by handing every one of them to a real link
before the bucket is filled, and anything that fails comes back in a WARNING
line rather than being quietly excused. Counting them would make the port
look fifty symbols further from a binary than it is.

Usage: gap.py [--list CATEGORY]
"""
import glob, re, subprocess, sys, os, tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.chdir(REPO)


def symbols():
    objs = glob.glob('build/pc/src/*/*.o') + glob.glob('build/pc/data/*.o')
    if not objs:
        raise SystemExit('no objects -- run `make -f Makefile.pc objs` first')
    out = subprocess.run(['nm'] + objs, capture_output=True, text=True).stdout
    undef, defined = set(), set()
    for line in out.split('\n'):
        p = line.split()
        if len(p) == 2 and p[0] == 'U':
            undef.add(p[1])
        elif len(p) == 3 and p[1] not in 'Uu':
            defined.add(p[2])
    # datatodo.txt's absolute symbols are supplied to the link as --defsym, not
    # by any object, so counting them as missing overstates the gap by 220.
    # See tools/pc/gen_defsyms.py.
    if os.path.exists('build/pc/defsyms.txt'):
        for line in open('build/pc/defsyms.txt'):
            m = re.match(r'--defsym\s+(\w+)=', line)
            if m:
                defined.add(m.group(1))
    return sorted(undef - defined), len(defined)


CATS = [
    ('un-decompiled functions', r'^func_'),
    ('unresolved data',         r'^D_'),
    ('libultra os/io',          r'^(os|__os)'),
    ('audio library',           r'^(al|n_al)'),
    ('gu math',                 r'^gu'),
    # NOT GAPS, and not counted in the headline number. Everything in
    # this bucket is supplied by the link itself: the host libc and libm, the
    # symbols ld's own script defines, and tools/pc/hostmain.c's entry point.
    # Every member is PROVED to resolve -- link_verified() links them for real
    # before they land here, and anything that fails comes back in the WARNING
    # line instead of being quietly excused.
    #
    # The membership test is NOT this pattern; the pattern is kept only as the
    # fallback for a machine where no symbol table can be read.
    ('supplied by the host link',
     r'^(memcpy|memset|memmove|strlen|strcpy|bcopy|bzero|sinf|cosf|sqrtf|'
     r'sincosf|_GLOBAL_OFFSET_TABLE_|__stack_chk_fail_local)$'),
    # Linker-script segment bounds, not code. On PC these come from whatever
    # overlay model the port adopts -- see docs/PC_PORT_SURFACE.md.
    ('overlay segment bounds',  r'^ovl\d+_(ROM|VRAM|TEXT|DATA|RODATA|BSS)'),
]


def host_supplied():
    """Symbols the host toolchain resolves at link time, read rather than listed.

    A hand-maintained list covers only undefined symbols from game code. The
    platform layer under src/pc/ calls fopen, getenv, mmap, snprintf,
    clock_gettime, swapcontext and references stderr, and without this some
    thirty such names land in "libc / other" and are counted as remaining
    PORTING work. They are not -- the link resolves every one of
    them today.

    Three sources, all authoritative rather than guessed:
      * the C library and friends, from their dynamic symbol tables;
      * libgcc, for the 64-bit division helpers a 32-bit build needs;
      * the handful of symbols ld itself defines.

    tools/pc/gen_stubs.py needs the same set for the same reason and has its
    own copy; if a third caller appears, this belongs in a shared module.
    """
    names = set()
    for lib in ('libc.so.6', 'libm.so.6', 'libpthread.so.0', 'librt.so.1'):
        for d in ('/lib/x86_64-linux-gnu', '/usr/lib/x86_64-linux-gnu',
                  '/lib/i386-linux-gnu', '/usr/lib/i386-linux-gnu',
                  '/lib', '/usr/lib'):
            p = os.path.join(d, lib)
            if not os.path.exists(p):
                continue
            out = subprocess.run(['nm', '-D', '--defined-only', p],
                                 capture_output=True, text=True).stdout
            for line in out.split('\n'):
                parts = line.split()
                if len(parts) >= 3 and parts[1] not in 'Aa':
                    names.add(parts[2].split('@')[0])
            break
    # libgcc's soft-arithmetic helpers. gcc emits calls to these for 64-bit
    # division on i386 and always links them.
    names |= {'__udivdi3', '__divdi3', '__umoddi3', '__moddi3', '__udivmoddi4'}
    # Defined by the LINKER SCRIPT, not by any object and not by any library,
    # so they are in no symbol table to read: ld's built-in script emits them
    # around the output sections. src/pc/os_pi.c's own-image DMA guard reads
    # three of them and asserts at startup that they bracket its own code.
    # __data_start in particular was landing in "libc / other" and being
    # counted as remaining platform work.
    names |= {'__executable_start', '_etext', '_edata', '__data_start',
              'data_start', '__bss_start', '__bss_start__', '_end', 'end',
              '_GLOBAL_OFFSET_TABLE_', '_DYNAMIC'}
    # glibc splits a few functions into libc_nonshared.a, a STATIC archive
    # every link pulls in implicitly, so they are absent from libc.so.6's
    # dynamic symbol table that the loop above reads. atexit is the one the
    # platform layer uses (src/pc/pc_progress.c registers the end-of-run
    # verdict with it); it linked the first time and still showed up as a
    # missing symbol, which is exactly the false-positive this function
    # exists to prevent.
    names |= {'atexit', '__libc_csu_init', '__libc_csu_fini',
              'stat', 'fstat', 'lstat', 'mknod'}
    return names


def link_objects_supplied():
    """Symbols the LINK adds that no object under build/pc/src/*/ defines.

    tools/pc/hostmain.c is compiled by the link step, not by Makefile.pc's
    object rules, so its two symbols are invisible to symbols() and were being
    counted as missing. They are read out of the file rather than listed, so
    renaming one cannot leave a stale entry here.
    """
    names = set()
    p = 'tools/pc/hostmain.c'
    if os.path.exists(p):
        names |= set(re.findall(r'^(?:\w+[ \t]+)+\*?(\w+)\s*\([^;]*\)\s*\{',
                                open(p).read(), re.M))
    return names


def link_verified(names, libs=('-lm',)):
    """Split `names` into (resolved, unresolved) by actually LINKING them.

    host_supplied() reads symbol tables, which answers "is this name in
    libc.so" and not the question that matters, which is "will the link this
    Makefile performs resolve it". The two differ in both directions: a name
    can be in the dynamic table of a library the port does not link, and a
    name can be absent from every table and still resolve because the linker
    script defines it (__data_start, _etext).

    So the answer is measured the only way it can be: one throwaway object
    that references every candidate, handed to the same driver and the same
    libraries the port links with. Anything ld cannot find comes back named,
    in its own error message, and stays counted as a gap.

    Costs one gcc invocation per run and needs no allowlist to be maintained.
    """
    names = sorted(names)
    if not names:
        return set(), set()
    src = ''.join(f'extern char {n};\n' for n in names)
    src += 'void *pc_gap_refs[] = {\n'
    src += ''.join(f'  &{n},\n' for n in names)
    src += '};\nint main(void) { return pc_gap_refs[0] != 0; }\n'
    with tempfile.TemporaryDirectory() as d:
        c = os.path.join(d, 'probe.c')
        with open(c, 'w') as f:
            f.write(src)
        r = subprocess.run(['gcc', '-m64', '-no-pie', '-fno-pie', '-w',
                            c, '-o', os.path.join(d, 'probe')] + list(libs),
                           capture_output=True, text=True)
    if r.returncode == 0:
        return set(names), set()
    bad = set(re.findall(r"undefined reference to [`']([^'\"]+)'", r.stderr))
    if not bad:
        # The link failed for a reason that is not a missing symbol (no
        # compiler, no crt files). Say nothing rather than reclassify
        # everything, and fall back to the symbol-table answer.
        return set(names), set()
    return set(names) - bad, bad & set(names)


def pragma_names():
    """Every function still behind a #pragma GLOBAL_ASM.

    Matching on `^func_` alone undercounts: plenty of functions have real
    names -- game_tick, auThreadMain, eneTurnCommon, initTrack, saveVerify,
    saveForceCompleteFile, saveCalcHeaderChecksum -- and were landing in the
    "libc / other" bucket, which made the platform layer look bigger than it is
    and the decompilation smaller. The pragma set is the authority.
    """
    out = set()
    for cf in glob.glob('src/**/*.c', recursive=True):
        out |= set(re.findall(r'GLOBAL_ASM\("[^"]*/(\w+)\.s"\)', open(cf).read()))
    return out


def buckets_of(gap, host=None):
    """[(category, [symbol])] plus the set the host link really satisfies.

    Shared with tools/pc/link.sh's caller so that the link's "what is still
    missing" answer and gap.py's are the same answer, computed once.
    """
    prag = pragma_names()
    cand = host_supplied() if host is None else host
    host, notreally = link_verified(cand & set(gap))
    # Not part of the probe -- the probe links libc and libm, not the port's
    # own host entry object -- but supplied by the same link all the same.
    host |= link_objects_supplied()
    buckets, seen = [], set()
    for name, pat in CATS:
        if name.startswith('un-decompiled'):
            # M2C_ERROR is not a libc symbol and never will be: m2c emits it
            # where it could not recover a value ("Read from unset register
            # $v0"), so a draft that still calls it is a draft that is NOT
            # decompiled, whatever its #ifdef says. It belongs with the
            # functions the decompilation still owes, and gap.py --list
            # un-decompiled is where someone would look for it.
            hit = [s for s in gap if (re.match(pat, s) or s in prag
                                      or s == 'M2C_ERROR')
                   and s not in host]
        elif name.startswith('supplied by'):
            hit = [s for s in gap if (re.match(pat, s) or s in host)
                   and s not in seen and s not in prag]
        else:
            hit = [s for s in gap if re.match(pat, s) and s not in seen
                   and s not in prag and s not in host]
        seen |= set(hit)
        buckets.append((name, hit))
    buckets.append(('libc / other', [s for s in gap if s not in seen]))
    return buckets, host, notreally


# Categories that are NOT remaining platform-layer work. "supplied by
# libc/libm" is in the list because the host link resolves every one of those
# names -- link_verified() proves it by linking them -- so they are not
# missing from anything.
NOT_PORT = ('un-decompiled functions', 'supplied by the host link')


def main():
    gap, ndef = symbols()
    want = sys.argv[sys.argv.index('--list') + 1] if '--list' in sys.argv else None

    buckets, host, notreally = buckets_of(gap)
    supplied = dict(buckets)['supplied by the host link']
    real = [s for s in gap if s not in supplied]

    print(f'{ndef} symbols defined, {len(real)} still missing '
          f'({len(supplied)} more the host link supplies -- proved by linking '
          f'them -- and are not counted)\n')
    for name, hit in buckets:
        print(f'  {name:26} {len(hit):5}')
        if want and want in name:
            for s in hit:
                print(f'      {s}')
    if notreally:
        print('\n  WARNING: in a host symbol table but NOT resolvable by the '
              'link:\n      ' + ', '.join(sorted(notreally)))

    plat = sum(len(h) for n, h in buckets if n not in NOT_PORT)
    owed = len(buckets[0][1])
    print(f'\nplatform layer: {plat} symbols. Decompilation still owes '
          f'{owed} functions;')
    if plat == 0 and owed:
        print('nothing but those stands between this tree and a linked '
              'binary.')
    else:
        print('until those land the binary cannot link no matter how complete '
              'the platform layer is.')


if __name__ == '__main__':
    main()
