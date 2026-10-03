#!/usr/bin/env python3
"""Find the pointer truncations before they crash the port.

    python3 tools/pc/lp64_audit.py                 the ranked worklist
    python3 tools/pc/lp64_audit.py --file src/ovl2/ovl2_7.c
    python3 tools/pc/lp64_audit.py --casts         include the explicit-cast tiers

WHAT THIS IS FOR. The port's dominant failure mode is not a missing function,
it is a pointer that lost its top 32 bits. Host pointers are 8 bytes and the
game's decompiled signatures often say `s32` where the ROM had a 4-byte
address, so the value arrives sign-extended garbage and the first dereference
segfaults -- 8 seconds into a boot, in a function that is not the one at
fault. A representative case: src/ovl2/ovl2_7.c's CollisionState wrapper family forwards its trailing
arguments to callees whose parameters are pointers, and every one of those
arguments is declared `s32`.

THE COMPILER ALREADY KNOWS. Every one of those sites is a -Wint-conversion
diagnostic, and Makefile.pc compiles with -w, so nothing has ever printed
them. This runs the same compile with the pointer-width diagnostics turned
back on and nothing else, so it costs a syntax-only pass and adds no flags to
the build. -w stays in the build on purpose: the tree has thousands of
warnings that are noise for a decompilation, and turning them all on would
bury these.

WHY IT IS A WORKLIST AND NOT A LINT. The diagnostics come out per CALL SITE,
but the bug is in a DECLARATION -- one wrong signature produces a dozen
warnings, and fixing it clears all of them at once. So the report groups by
the function whose declaration is wrong and by the file that has to be edited,
which is also how work is assigned in this tree (by file, never by position in
a list -- see tools/decomp/REFOUND.md).

THE TIERS, in descending confidence:

  truncating   a POINTER is passed to a parameter declared as an integer.
               The callee's declaration is wrong; the value is truncated at
               the call. This is the crash.
  widening     an INTEGER is passed to a pointer parameter. Almost always the
               same bug seen from the other end: the caller took the value in
               as `s32` and is forwarding it, so the CALLER's own signature is
               what needs fixing.
  incompatible two different pointer types. Same width, so it does not
               truncate, but it is how a struct gets read at the wrong
               offsets, which is the second-commonest port failure.
  casts        explicit (s32)pointer / (pointer)s32. NOT reported by default,
               because the tree uses a DELIBERATE truncated sub-4GiB pointer
               where a struct field must stay 4 bytes wide (see the PORT arm
               of func_800A9250 in src/ovl1/ovl1_3.c). Those are correct and
               commented as such, so this tier needs reading rather than
               fixing, and --casts is how you read it.

WHAT IT CANNOT SEE, said plainly because an auditor that overstates its
coverage is worse than none. A call made through a CAST function pointer
type-checks against the cast, not against the real definition, so the compiler
has nothing to warn about and this reports nothing. That is not a corner case:
the CollisionState truncation above arrives this way. src/ovl2/ovl2_5.c casts
`castFn` to `void (*)(Vector *, Vector *, void *, void *, void *)` and calls
func_80104958 through it, whose definition takes three `s32`; the truncation
happens in silence and only the wrappers' OWN forwarding calls show up here.
So a clean file is not a proof. Treat the ranking as "where to look first",
and read the crash backtrace from tools/pc/smoke.py for the rest.

Nothing here edits anything. It names files and functions; whoever owns the
file makes the change, and the N64 build must stay byte-exact across it --
these are PORT-side call sites, so the sha1 gate is the acceptance test.
"""
import argparse
import collections
import concurrent.futures
import glob
import os
import re
import subprocess
import sys

# Derived from this file's own location. No path belonging to whoever runs it
# may appear in the repository.
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Exactly Makefile.pc's PC_CFLAGS, minus -w, plus the four diagnostics that
# describe a pointer changing width. Kept in this shape so a divergence from
# the real build is visible as a diff rather than hidden in a helper.
#
# -w IS NOT HERE AND MUST NOT BE. gcc's -w suppresses warnings enabled by
# LATER -W flags too, so `-w -Wint-conversion` prints nothing at all -- which
# looks exactly like a clean tree (163 files, zero findings). The default warning set comes along
# as a result; the regexes below select only the four shapes that matter.
BASE = [
    'gcc', '-m64', '-fno-pie', '-std=gnu90', '-fsigned-char', '-O1',
    '-fsyntax-only',
    # No caret art or source excerpts; one line per diagnostic. It does NOT
    # settle the quoting -- see the note on QUOTE below.
    '-fdiagnostics-plain-output',
    '-Wint-conversion', '-Wincompatible-pointer-types',
    '-Wpointer-to-int-cast', '-Wint-to-pointer-cast',
    '-D_LANGUAGE_C', '-DTARGET_N64', '-DPORT', '-DF3DEX_GBI_2', '-DAVOID_UB',
    '-DNON_MATCHING',
    '-Iinclude', '-Iinclude/libc', '-Ilibreultra/include/2.0I', '-Ibuild',
    '-Ibuild/include', '-Ibuild/assets', '-Isrc', '-I.',
]

# gcc quotes identifiers with U+2018/U+2019 under a UTF-8 locale and with
# ASCII apostrophes under LC_ALL=C, and -fdiagnostics-plain-output does not
# change that. compile_one() forces LC_ALL=C, and the pattern accepts both
# anyway: a pattern that accepts only one quoting silently misses every
# diagnostic and reports a clean tree, the worst possible failure for an
# auditor.
QUOTE = u"['‘’]"
ARG_RE = re.compile(
    r"^(?P<file>[^:]+):(?P<line>\d+):\d+: warning: "
    r"passing argument (?P<argno>\d+) of " + QUOTE +
    r"(?P<callee>.+?)" + QUOTE + r" "
    r"(?P<what>makes integer from pointer|makes pointer from integer|"
    r"from incompatible pointer type)")
CAST_RE = re.compile(
    r"^(?P<file>[^:]+):(?P<line>\d+):\d+: warning: "
    r"cast (?P<what>from pointer to integer|to pointer from integer) "
    r"of different size")

TIER = {
    'makes integer from pointer': 'truncating',
    'makes pointer from integer': 'widening',
    'from incompatible pointer type': 'incompatible',
    'from pointer to integer': 'cast-narrowing',
    'to pointer from integer': 'cast-widening',
}
ORDER = ['truncating', 'widening', 'incompatible',
         'cast-narrowing', 'cast-widening']


def compile_one(path):
    env = dict(os.environ)
    env['LC_ALL'] = 'C'
    r = subprocess.run(BASE + [path], capture_output=True, text=True,
                       cwd=REPO, env=env)
    return r.stderr


def scan(paths, jobs):
    findings = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for text in pool.map(compile_one, paths):
            for line in text.split('\n'):
                m = ARG_RE.match(line)
                if m:
                    findings.append({
                        'file': m.group('file'),
                        'line': int(m.group('line')),
                        'tier': TIER[m.group('what')],
                        'callee': m.group('callee'),
                        'argno': int(m.group('argno')),
                    })
                    continue
                m = CAST_RE.match(line)
                if m:
                    findings.append({
                        'file': m.group('file'),
                        'line': int(m.group('line')),
                        'tier': TIER[m.group('what')],
                        'callee': None,
                        'argno': None,
                    })
    return findings


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--file', action='append',
                    help='audit only this source file (repeatable)')
    ap.add_argument('--casts', action='store_true',
                    help='also report the explicit-cast tiers, which include '
                         'the deliberate sub-4GiB pointer convention')
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--top', type=int, default=20,
                    help='how many files and callees to list (default 20)')
    args = ap.parse_args()

    os.chdir(REPO)

    if args.file:
        paths = args.file
    else:
        # src/pc is the platform layer, written for this ABI on purpose, so it
        # is the one directory excluded.
        paths = sorted(p for p in glob.glob('src/*/*.c')
                       if not p.startswith('src/pc/'))
    paths = [p for p in paths if os.path.exists(p)]
    if not paths:
        print('lp64_audit: nothing to scan')
        return 1

    print(f'lp64_audit: {len(paths)} translation unit(s)')
    findings = scan(paths, args.jobs)

    tiers = [t for t in ORDER
             if args.casts or not t.startswith('cast-')]
    counts = collections.Counter(f['tier'] for f in findings)
    print()
    for t in ORDER:
        if t in tiers or counts[t]:
            note = '' if t in tiers else '   (not listed below; pass --casts)'
            print(f'  {counts[t]:5d}  {t}{note}')

    live = [f for f in findings if f['tier'] in tiers]
    if not live:
        print('\nlp64_audit: clean')
        return 0

    # BY CALLEE FIRST, because that is the unit of repair: one declaration
    # fixed clears every site that calls it.
    bycallee = collections.Counter(
        (f['callee'], f['tier']) for f in live if f['callee'])
    print(f'\nby callee -- fixing one declaration clears all of its sites:')
    for (callee, tier), n in bycallee.most_common(args.top):
        argnos = sorted({f['argno'] for f in live
                         if f['callee'] == callee and f['tier'] == tier})
        where = sorted({f['file'] for f in live if f['callee'] == callee
                        and f['tier'] == tier})
        arglist = ','.join(str(a) for a in argnos)
        print(f'  {n:4d}  {tier:12s} {callee}  arg {arglist}')
        print(f'        called from: {", ".join(where[:4])}'
              + (f' (+{len(where) - 4} more)' if len(where) > 4 else ''))

    # THEN BY FILE, because that is the unit of assignment.
    byfile = collections.Counter(f['file'] for f in live)
    print(f'\nby file -- the lane that owns the file makes the change:')
    for path, n in byfile.most_common(args.top):
        t = collections.Counter(f['tier'] for f in live if f['file'] == path)
        detail = ' '.join(f'{t[k]} {k}' for k in ORDER if t[k])
        print(f'  {n:4d}  {path}   ({detail})')

    if args.file:
        print('\nsites:')
        for f in sorted(live, key=lambda f: (f['file'], f['line'])):
            tail = (f"{f['callee']} arg {f['argno']}"
                    if f['callee'] else 'explicit cast')
            print(f"  {f['file']}:{f['line']}  {f['tier']:12s} {tail}")

    return 0


if __name__ == '__main__':
    sys.exit(main())
