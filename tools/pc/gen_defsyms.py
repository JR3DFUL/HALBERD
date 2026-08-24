#!/usr/bin/env python3
"""Resolve datatodo.txt's absolute symbols for the native link.

`datatodo.txt` is fed to the N64 link with `-T`. It is 495 assignments of the
form `D_8012EB00 = 0x8012EB00;` -- a symbol pinned to a raw N64 VRAM address.
That works on N64 because the linker script puts every segment at its true
address, so an absolute assignment lands inside real data. It cannot work
natively: the host has no such address space, and these 220 symbols were the
ENTIRE unresolved-data residue after the data listings were translated.

The fix mirrors what datatodo.txt already does, one level down. Each address is
mapped to the data block that contains it, and re-expressed as that block's C
symbol plus a byte offset:

    D_8012EB00 = 0x8012EB00        ->    --defsym D_8012EB00=D_8012EAC0+0x40

`ld --defsym` takes exactly that form, so the native link gets the same
aliasing the N64 link gets, without the game sources knowing anything about it.

The residue does not resolve this way, but NOT for the reason first assumed. It
was written up as 77 "ROM file offsets ... asset pointers", and that claim was
carried into the DMA design brief before anyone checked it. Only 6 are ROM file
offsets. The other 71 are VRAM addresses in 0x8012E000-0x8013xxxx that fall
outside every asm/data block, so a cartridge-reading path could never have
satisfied them. They are reported rather than emitted as something plausible
but wrong.

Usage: gen_defsyms.py [-o build/pc/defsyms.txt]
"""
import bisect, glob, os, re, subprocess, sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.chdir(REPO)

ADDR = re.compile(r'/\*\s*(?:[0-9A-F]+\s+)?(8[0-9A-F]{7})\s')


def named_blocks(skip):
    """[(start, last, symbol)] from tools/symbol_addrs.txt.

    A dlabel only exists while a block is still assembly. Once a block is
    migrated into C the listing goes away, and any datatodo symbol pointing
    INTO it becomes unresolvable -- which is exactly what happened to the six
    entries hand-added for D_800D7178 and D_800D71E8. symbol_addrs.txt still
    carries the address and usually a `// size:0xNN`, so it covers the gap.

    `skip` is the set of names datatodo.txt itself defines; using one of those
    as a base would just move the problem.
    """
    out = []
    for line in open('tools/symbol_addrs.txt'):
        m = re.match(r'(\w+)\s*=\s*0x([0-9A-Fa-f]+)\s*;(.*)', line)
        if not m or m.group(1) in skip:
            continue
        addr = int(m.group(2), 16)
        sz = re.search(r'size:\s*(0x[0-9A-Fa-f]+|\d+)', m.group(3))
        out.append((addr, addr + (int(sz.group(1), 0) - 1 if sz else 0),
                    m.group(1)))
    return out


def _widened_blocks():
    """{symbol} for every block gen_data.py emits as an 8-byte-per-word
    void*[] (pointer blocks and FORCE_WIDEN tables). A defsym that lands
    inside one of these must scale its N64 byte offset by 2, or it points
    mid-cell: the ovl18 demo-setup records (D_8022AED8 et al) resolved to
    D_8022AE4C_ovl18+0x8C in a block whose host layout put that data at
    +0x118, and func_800BBC6C copied display-list halves into the stage
    globals."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import gen_data
    widened = set()
    for f in glob.glob('asm/data/**/*.s', recursive=True):
        for sym, _sec, entries in gen_data.parse(f):
            if sym in gen_data.FORCE_WORD32 or sym in gen_data.SUPPRESS_BSS:
                continue
            # One predicate, owned by gen_data, so the two tools can never
            # disagree about a block's host layout (mixed descriptor blocks
            # joined pointer blocks and FORCE_WIDEN when render_mixed_widened
            # landed).
            if gen_data.is_widened_block(sym, entries):
                widened.add(sym)
    return widened


def data_blocks():
    """[(start_vram, last_vram, symbol, widened)] for every dlabel, sorted."""
    widened = _widened_blocks()
    blocks = []
    for f in glob.glob('asm/data/**/*.s', recursive=True):
        cur, addrs = None, []
        for line in open(f):
            m = re.match(r'^dlabel (\w+)', line)
            if m:
                if cur and addrs:
                    blocks.append((addrs[0], addrs[-1], cur, cur in widened))
                cur, addrs = m.group(1), []
                continue
            m = ADDR.search(line)
            if m and cur:
                addrs.append(int(m.group(1), 16))
        if cur and addrs:
            blocks.append((addrs[0], addrs[-1], cur, cur in widened))
    blocks.sort()
    return blocks


# ---------------------------------------------------------------------------
# Spans the PORT lays out at true N64 offsets.
#
# Two files in src/pc/ reserve a block of .bss and place labels inside it at
# the offsets the N64 linker script uses, precisely so that address arithmetic
# between them survives. Their own headers explain why. What matters here is
# that each one turns a RANGE of N64 addresses into `symbol + offset`, which is
# exactly what a --defsym needs -- so an absolute address with no data block
# under it is still resolvable if it lands in one of these.
#
#     (first N64 address, last+1, span symbol, source file)
#
# THE NUMBERS ARE DUPLICATED FROM THOSE FILES and have to agree with them. They
# are checked, not trusted: check_port_spans() below re-derives each span's
# length from the .space directives in the source and fails the build if a
# span has been resized without this table following.
PORT_SPANS = [
    (0x800D69C0, 0x800F61A0, 'ovl1_BSS_START',     'src/pc/pc_overlay.c'),
    (0x80300160, 0x80300220, 'ovl20_BSS_START',    'src/pc/pc_overlay.c'),
    (0x8012C000, 0x80400000, 'pc_ram_window_base', 'src/pc/pc_ram_window.c'),
]


def check_port_spans():
    """Fail loudly if PORT_SPANS has drifted from the C it describes."""
    for lo, hi, sym, src in PORT_SPANS:
        text = open(src).read()
        m = re.search(re.escape('"' + sym + ':') + r'.*?(?=\.text)', text,
                      re.S)
        if not m:
            raise SystemExit(f'gen_defsyms: {sym} is no longer defined in '
                             f'{src}; PORT_SPANS is stale')
        total = sum(int(v, 0) for v in
                    re.findall(r'\.space\s+(0x[0-9A-Fa-f]+|\d+)', m.group(0)))
        # The trailing `.space 4` every span puts after its last label is a
        # guard byte, not part of the span.
        if total - 4 < hi - lo:
            raise SystemExit(
                f'gen_defsyms: {sym} in {src} reserves 0x{total - 4:X} bytes '
                f'but PORT_SPANS claims 0x{hi - lo:X}. One of the two moved.')


def segment_bounds():
    """[(name, base_symbol, host_offset)] for the overlay bss END markers.

    kirby.ld gives every overlay an `ovlN_BSS_END = .` at the close of its bss
    output section, and gOverlayTable's records -- assembly data, translated
    into build/pc/data -- store that symbol as the descriptor's bssEnd. A
    native link has no such script, and these were gap.py's "overlay segment
    bounds" bucket.

    Each one is the address one past the last bss block of its overlay, and
    every one of the nine lands EXACTLY on the end of a pure-`.space` block in
    a listing that gen_data.py already translates. So the honest definition is
    "the end of that block on the host":

        ovl10_BSS_END = D_801F4DA0_ovl10 + 0x20

    The offset is the HOST size, which for a bss block is twice the listing's
    -- see the doubling note in gen_data.render(). Using the host size rather
    than the N64 one is what keeps the symbol pointing past the end of real
    storage instead of into the middle of the last object.

    ovl1's and ovl20's bounds are NOT here: src/pc/pc_overlay.c defines those
    as real labels, and a --defsym on top of a defined symbol is a multiple
    definition.
    """
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import gen_data

    # Two indexes over the translated bss blocks: by the address they START
    # at, which is what an ovlN_BSS_START marker is, and by the address one
    # past their end, which is what an ovlN_BSS_END marker is.
    #
    # A BLOCK'S ADDRESS COMES OUT OF ITS OWN LISTING, not out of any symbol
    # map. The listing writes it on every line -- `/* 801F4D30 */ .space 0x18`
    # -- so this needs no matched build and cannot go stale. Going through
    # tools/pc/vram_syms.txt instead lost two bounds: that file is keyed by
    # ADDRESS and keeps one name per address, so D_8015A7C0_ovl6 is simply not
    # in it (something else at 0x8015A7C0 was written first) and ovl6_BSS_END
    # had nothing to hang off on a machine with no elf.
    line_addr = re.compile(r'/\*\s*([0-9A-F]{8})\s*\*/')
    ends = {}      # n64 address -> (symbol, host offset from that symbol)
    for f in sorted(glob.glob('asm/data/**/*.s', recursive=True)):
        text = open(f).read().split('\n')
        starts = {}
        cur = None
        for line in text:
            m = re.match(r'^dlabel (\w+)', line)
            if m:
                cur = m.group(1)
                continue
            m = line_addr.search(line)
            if m and cur and cur not in starts:
                starts[cur] = int(m.group(1), 16)
        for sym, section, entries in gen_data.parse(f):
            if section != '.bss' or sym not in starts:
                continue
            if {k for k, _ in entries} != {'space'}:
                continue
            n = sum(int(v, 0) for _, v in entries)
            ends.setdefault(starts[sym], (sym, 0))
            ends[starts[sym] + n] = (sym, n * 2)

    # {name: n64 address} for the BOUND MARKERS themselves, which exist only
    # in the linker script and so are in no listing. The matched build when
    # there is one, otherwise the map committed next to this script -- whose
    # tail carries these names for exactly this lookup.
    addr = {}
    if os.path.exists('build/kirby.us.elf'):
        out = subprocess.run(['nm', 'build/kirby.us.elf'],
                             capture_output=True, text=True).stdout
        for line in out.split('\n'):
            p = line.split()
            if len(p) == 3 and '.' not in p[2]:
                addr.setdefault(p[2], int(p[0], 16))
    else:
        for line in open('tools/pc/vram_syms.txt'):
            if line.startswith('#'):
                continue
            p = line.split()
            if len(p) == 2:
                addr.setdefault(p[1], int(p[0], 16))

    out, missed = [], []
    for name, a in sorted(addr.items()):
        if not re.match(r'ovl\d+_BSS_(START|END)$', name):
            continue
        if name.startswith('ovl1_') or name.startswith('ovl20_'):
            continue        # real labels in src/pc/pc_overlay.c
        if a in ends:
            out.append((name, ends[a][0], ends[a][1]))
        else:
            missed.append(name)
    return out, missed


# Symbols a decompiled draft invented, whose value is nevertheless exact.
#
# A draft may name something the listing has no label for. When the intended
# object is unambiguous the alias belongs here rather than in a stub: it is
# the same address, so nothing is approximated.
#
# D_8012BCA0_p -- src/ovl3/ovl3_1.c's func_80153668_ovl3 draft declares
#   `extern u8 *D_8012BCA0_p[]` and indexes it. Two other drafts in the SAME
#   file spell the identical table `(u8 **)(D_8012BCA0 + 0x40)` and
#   `*(u8 **)(D_8012BCA0 + 128 + wi * 8)`, i.e. D_8012BCA0 viewed as an array
#   of host pointers -- so `_p` is that view and its base is D_8012BCA0
#   itself. The real fix is to write it the way its two neighbours do; that is
#   src/ovl3's to make, and this keeps the link closed until it happens.
DRAFT_ALIASES = {
    'D_8012BCA0_p': 'D_8012BCA0',
}


def already_defined():
    """Every symbol some object in this build already defines.

    A --defsym for a symbol that an object also defines is not a harmless
    duplicate, it is `ld: multiple definition` and the link stops. That was
    latent while every emitted entry came from a data block (a block cannot be
    both translated and absolute), and it stopped being latent the moment the
    PORT_SPANS fallback started resolving addresses INTO src/pc's RAM window:
    five of datatodo.txt's absolutes -- D_8012EB00, D_8022FB50, gFrameBuffer,
    D_803D6900, D_803DA800 -- are the very labels that window defines.

    Two sources, because this script has to work both before and after the
    objects exist:
      * src/**/*.c, for C definitions and for the `.globl` lines inside the
        inline-assembly spans (that is how pc_ram_window.c and pc_overlay.c
        define theirs);
      * nm over build/pc, when there is a build to read.
    """
    names = set()
    for cf in glob.glob('src/**/*.c', recursive=True):
        txt = open(cf).read()
        names |= set(re.findall(r'\.globl\s+(\w+)', txt))
        for m in re.finditer(
                r'^(?!extern)(?:[\w\*]+[ \t]+)+?(\w+)\s*(?:\[[^\]]*\])?\s*=',
                txt, re.M):
            names.add(m.group(1))
    objs = glob.glob('build/pc/src/*/*.o') + glob.glob('build/pc/data/*.o')
    if objs:
        out = subprocess.run(['nm', '-g', '--defined-only'] + objs,
                             capture_output=True, text=True).stdout
        for line in out.split('\n'):
            p = line.split()
            if len(p) == 3 and p[1] not in 'Uu':
                names.add(p[2])
    return names


ORPHAN = re.compile(r'^D_(8[0-9A-F]{7})(?:_ovl\d+)?$')


def orphan_addresses(defined, in_datatodo, resolve):
    """datatodo entries the tree needs but datatodo.txt does not have.

    `D_800D723C` is the case that motivates this. src/ovl3/plyshot.c's draft of
    func_8015D7A0_ovl3 writes the pair of scratch globals the listing shows it
    writing, D_800D7238 and D_800D723C. datatodo.txt has the first and not the
    second -- it is hand-maintained, and it only ever grew entries for the
    addresses the MATCHING build happened to need, which is a different set
    from the addresses the drafts reference.

    The tree's naming convention makes the missing entries recoverable without
    guessing: a symbol called D_800D723C IS the address 0x800D723C. So every
    still-undefined symbol whose name is an address gets the same treatment
    datatodo's own entries get -- resolved against a data block or a port RAM
    span, or left alone. Nothing is invented; a name that does not resolve is
    reported, not stubbed.

    Reading the undefined set means this has to run AFTER the objects are
    compiled, which is why Makefile.pc's `defsyms` target depends on `objs`.
    With no objects to read it emits nothing, which is correct rather than
    merely safe: the defsyms it would have emitted are for symbols nothing
    references.
    """
    objs = glob.glob('build/pc/src/*/*.o') + glob.glob('build/pc/data/*.o')
    if not objs:
        return [], []
    out = subprocess.run(['nm'] + objs, capture_output=True, text=True).stdout
    undef = set()
    for line in out.split('\n'):
        p = line.split()
        if len(p) == 2 and p[0] == 'U':
            undef.add(p[1])
    undef -= defined
    lines, missed = [], []
    for name in sorted(undef):
        m = ORPHAN.match(name)
        if not m or name in in_datatodo:
            continue
        got = resolve(int(m.group(1), 16))
        if got:
            lines.append(f'--defsym {name}={got}')
        else:
            missed.append(name)
    return lines, missed


def main():
    out = 'build/pc/defsyms.txt'
    if '-o' in sys.argv:
        out = sys.argv[sys.argv.index('-o') + 1]
    os.makedirs(os.path.dirname(out), exist_ok=True)

    check_port_spans()
    defined = already_defined()

    text = open('datatodo.txt').read()
    own = set(re.findall(r'^(\w+)\s*=', text, re.M))
    blocks = sorted(data_blocks()
                    + [b + (False,) for b in named_blocks(own)])
    starts = [b[0] for b in blocks]

    counts = {'block': 0, 'rom': 0, 'span': 0}

    def resolve(a):
        """`BASE+0xOFF` (or a bare number) for an N64 address, or None."""
        i = bisect.bisect_right(starts, a) - 1
        if i >= 0 and blocks[i][0] <= a <= blocks[i][1] + 8:
            base, off = blocks[i][2], a - blocks[i][0]
            if blocks[i][3]:
                off *= 2    # widened void*[]: 8 host bytes per N64 word
            counts['block'] += 1
            return f'{base}+0x{off:X}' if off else base
        if a < 0x8000000:
            # A CARTRIDGE FILE OFFSET, not an address. gOverlayTable's records
            # carry two of these per overlay (startAddr and endAddr) and dma.c
            # hands them straight to osEPiStartDma as devAddr; src/pc/os_pi.c
            # reads the ROM image at that offset. So the correct native value
            # is the NUMBER, which is also exactly what kirby.ld emits for
            # ovl1_ROM_START. Resolving one of these against a data block would
            # have been the bug.
            counts['rom'] += 1
            return f'0x{a:X}'
        for lo, hi, sym, _src in PORT_SPANS:
            if lo <= a < hi:
                counts['span'] += 1
                return f'{sym}+0x{a - lo:X}' if a != lo else sym
        return None

    lines, unresolved, relative = [], [], 0
    nskip = 0
    for name, expr in re.findall(r'^(\w+)\s*=\s*([^;]+);', text, re.M):
        expr = expr.strip()
        if name in defined:
            nskip += 1
            continue
        if not re.fullmatch(r'0x[0-9A-Fa-f]+', expr):
            # already relative to another symbol -- pass it through untouched,
            # which is what makes the six hand-added entries keep working
            lines.append(f'--defsym {name}={expr.replace(" ", "")}')
            relative += 1
            continue
        got = resolve(int(expr, 16))
        if got:
            lines.append(f'--defsym {name}={got}')
        else:
            unresolved.append((name, expr))

    orphans, orphan_miss = orphan_addresses(defined, own, resolve)
    lines.extend(orphans)

    bounds, missed = segment_bounds()
    bounds = [b for b in bounds if b[0] not in defined]
    for name, base, off in bounds:
        lines.append(f'--defsym {name}={base}+0x{off:X}'
                     if off else f'--defsym {name}={base}')

    aliases = {k: v for k, v in DRAFT_ALIASES.items() if k not in defined}
    for name, base in sorted(aliases.items()):
        lines.append(f'--defsym {name}={base}')

    with open(out, 'w') as f:
        f.write('\n'.join(lines) + '\n')

    print(f'{len(lines)} --defsym entries -> {out} '
          f'({counts["block"]} into a data block, {relative} already '
          f'symbol-relative, {counts["rom"]} ROM file offset(s), '
          f'{counts["span"]} into a port RAM span, {len(bounds)} overlay bss '
          f'bound(s), {len(orphans)} address-named orphan(s), '
          f'{len(aliases)} draft alias(es), '
          f'{nskip} skipped as already defined by an object)')
    if missed:
        print(f'{len(missed)} overlay bound(s) with no bss block to hang off: '
              + ', '.join(missed))
    for label, items in (('datatodo', [n for n, _ in unresolved]),
                         ('address-named orphan', orphan_miss)):
        if not items:
            continue
        print(f'{len(items)} unresolvable {label} symbol(s) -- outside every '
              f'known data block and every port RAM span')
        for n in items[:8]:
            print(f'    {n}')
        if len(items) > 8:
            print(f'    ... and {len(items) - 8} more')


if __name__ == '__main__':
    main()
