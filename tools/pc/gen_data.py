#!/usr/bin/env python3
"""Translate the MIPS data listings into C, for the PC build only.

The N64 build assembles `asm/data/**/*.s` directly. A native build cannot: the
listings are MIPS assembly. But they are also almost entirely `.word`, and
splat has already resolved every pointer word to a symbol NAME rather than a
raw address -- 7161 of them. So the translation is mechanical.

This is deliberately PC-ONLY and writes nothing the N64 build reads. Migrating
data into C for the N64 build means reproducing the ROM's byte layout exactly
and is the same class of problem as rodata migration; here byte layout does not
matter, only semantics, so the two jobs are completely separate. Nothing in
this file can affect the matching build.

Emitted arrays are untyped `u32` (or `void *` for pointer words). The generated
translation units deliberately include NO game headers: many of these symbols
are declared elsewhere with real types, and a `u32 D_800E1B50[]` definition
would conflict with an `extern struct Foo *D_800E1B50[]` declaration. C has no
cross-TU type checking at link time, so keeping them in isolation is what makes
the whole set compile.

Words are byte-swapped: the listings are big-endian ROM data and the host is
little-endian, so a `.word 0x3F800000` must become the u32 0x3F800000 (a value,
not a byte sequence) for float reinterpretation to work on the host.

Usage: gen_data.py [-o outdir]      default outdir: build/pc/data
"""
import os, re, sys, glob

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.chdir(REPO)

# Directives can be preceded by an address comment, so anchoring on the start
# of the line silently loses ALL of them. The first version of this tool did
# exactly that and dropped 15000+ entries -- .short, .float, .byte, .asciz and
# .double -- producing blocks that were correct-looking but short.
DIRECTIVE = re.compile(r'(?:/\*[^*]*\*/)?\s*'
                       r'\.(word|short|byte|float|double|asciz|space)\s+(.+?)\s*$')
INCBIN = re.compile(r'^\s*\.incbin\s+"([^"]+)"')
DLABEL = re.compile(r'^dlabel\s+(\w+)')
ENDLABEL = re.compile(r'^enddlabel\s+(\w+)')
SECTION = re.compile(r'^\.section\s+(\S+)')


def c_ident(sym):
    return sym


def parse(path):
    """[(symbol, section, entries)] where an entry is ('w', text) or ('z', n)."""
    out, cur, entries, section = [], None, [], '.data'
    for line in open(path):
        m = SECTION.match(line)
        if m:
            section = m.group(1).rstrip(',')
            continue
        m = DLABEL.match(line)
        if m:
            if cur:
                out.append((cur, section, entries))
            cur, entries = m.group(1), []
            continue
        m = ENDLABEL.match(line)
        if m:
            if cur:
                out.append((cur, section, entries))
            cur, entries = None, []
            continue
        if cur is None:
            continue
        m = INCBIN.match(line)
        if m:
            entries.append(('incbin', m.group(1)))
            continue
        m = DIRECTIVE.search(line)
        if m:
            kind, val = m.group(1), m.group(2).strip()
            entries.append(('space' if kind == 'space' else kind, val))
    if cur:
        out.append((cur, section, entries))
    return out


SCALAR = re.compile(r'0x[0-9A-Fa-f]+|-?\d+')
CTYPE = {'word': 'u32', 'short': 'u16', 'byte': 'u8',
         'float': 'f32', 'double': 'f64'}
WIDTH = {'word': 4, 'short': 2, 'byte': 1, 'float': 4, 'double': 8}


def _is_ref(val):
    return not SCALAR.fullmatch(val)


# ---------------------------------------------------------------------------
# Raw VRAM addresses sitting inside pointer tables.
#
# splat resolves a .word to a symbol NAME only when it can attribute it to a
# segment it is currently disassembling. Cross-overlay references it cannot,
# so a table of function pointers comes out half-named:
#
#     .word func_800BDD88          -> &func_800BDD88
#     .word 0x80151338             -> (void *)(u32)(0x80151338)
#
# The second form is a plain integer here, and the port jumped to it: the
# address is a gtl process entry, so osCreateThread got 0x80151338 as an entry
# point and the trampoline called it. SIGSEGV at 0x80151338 with an empty
# backtrace -- an N64 address executed as a host address.
#
# build/kirby.us.elf is the authority that closes this: it is the matching
# build, so every symbol in it sits at its true N64 address. An exact hit
# becomes `&name` and the host linker supplies the real address. Only EXACT
# hits are rewritten -- a near miss is far more likely to be a number that
# happens to look like an address than a pointer into the middle of something.
# ---------------------------------------------------------------------------
_VRAM_SYMS = None


def vram_symbols():
    """{n64_address: symbol} from the matching build, or its shipped map."""
    global _VRAM_SYMS
    if _VRAM_SYMS is not None:
        return _VRAM_SYMS
    _VRAM_SYMS = {}
    elf = 'build/kirby.us.elf'
    if os.path.exists(elf):
        import subprocess
        out = subprocess.run(['nm', elf], capture_output=True, text=True).stdout
        for line in out.split('\n'):
            p = line.split()
            if len(p) != 3 or p[1] in 'AaUuNnWwVv':
                continue
            name = p[2]
            # `func_X.NON_MATCHING` and friends are build aliases, not names
            # the port can link against; the plain name is at the same address.
            if '.' in name:
                continue
            addr = int(p[0], 16)
            # Keep the first name seen, so the result does not depend on nm's
            # ordering between two symbols that genuinely share an address.
            _VRAM_SYMS.setdefault(addr, name)
        return _VRAM_SYMS
    # No matched N64 build (user machines have no MIPS toolchain): use the
    # symbol map committed alongside this script, same content as nm above.
    txt = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'vram_syms.txt')
    if not os.path.exists(txt):
        # Leaving VRAM addresses unresolved does not fail the build; it fails
        # at RUNTIME as a jump to an N64 address (SIGSEGV at 0x80xxxxxx).
        # Refuse to generate silently-broken data.
        sys.stderr.write('gen_data.py: neither %s nor %s exists; '
                         'pointer tables cannot be resolved\n' % (elf, txt))
        sys.exit(1)
    with open(txt) as f:
        for line in f:
            p = line.split()
            if len(p) != 2 or p[0].startswith('#'):
                continue
            _VRAM_SYMS.setdefault(int(p[0], 16), p[1])
    return _VRAM_SYMS


def resolve_scalar_pointer(val):
    """`&symbol` if this integer word is exactly a known VRAM symbol."""
    try:
        addr = int(val, 0)
    except ValueError:
        return None
    if addr < 0x80000000 or addr > 0x807FFFFF:
        return None
    name = vram_symbols().get(addr)
    return name


# Widened-block registry, filled by main() before rendering starts. Interior
# pointer resolution needs it: an N64 byte offset into a widened block is a
# HOST offset of twice that (each 4-byte word became an 8-byte cell).
_WIDENED_REGISTRY = set()
_VRAM_SORTED = None


def resolve_vram_interior(val, refs):
    """C expression for a raw VRAM word that points INSIDE a data block.

    splat leaves a cross-overlay reference as a bare number when the target
    segment is not the one being disassembled, and the enemy kind tables are
    full of them (ovl7 tables pointing at ovl8/ovl10 descriptors). An exact
    symbol hit is `&name`; an address strictly inside a D_* data block becomes
    base + offset, with the offset doubled when the target block is widened.
    Only pointer-bearing contexts call this -- in a pure scalar table a number
    that looks like an address IS a number, and stays one.
    """
    global _VRAM_SORTED
    try:
        addr = int(val, 0)
    except ValueError:
        return None
    if addr < 0x80000000 or addr > 0x807FFFFF:
        return None
    syms = vram_symbols()
    name = syms.get(addr)
    if name:
        refs.add(name)
        return f'&{name}'
    if _VRAM_SORTED is None:
        _VRAM_SORTED = sorted(syms)
    import bisect
    i = bisect.bisect_right(_VRAM_SORTED, addr) - 1
    if i < 0:
        return None
    base_addr = _VRAM_SORTED[i]
    base = syms[base_addr]
    off = addr - base_addr
    # Data blocks only: an address inside a function is not a data pointer,
    # and pretending otherwise would silently corrupt a genuine constant.
    if not base.startswith('D_') or off >= 0x8000:
        return None
    if off % 4:
        return None
    if base in _WIDENED_REGISTRY:
        off *= 2
    refs.add(base)
    return f'(void *)((u8 *)&{base} + {off})'


# Tables that game code reads through a 4-byte-per-field overlay struct
# (struct Ovl1CameraSetup, src/ovl1/ovl1_2.c). The normal pointer-array
# emission makes every slot 8 bytes, so the overlay shears: the entry's
# onCreated callback slot lands where the game reads dlLinkBitMask, and the
# failure is a call through a bit mask (pc=0x400, ovl4 map scene postInit).
# These blocks render as raw 32-bit words via `.long` instead. `.long symbol`
# has the linker truncate the address to 32 bits (R_X86_64_32), which the
# -no-pie low-memory invariant that tools/pc/link.sh documents -- and
# pc_check_low_memory() enforces -- makes lossless. The sibling tables with
# no callback (D_800BF948 etc.) already emit as u32[] and need no entry here.
FORCE_WORD32 = {
    'D_800BFE50',   # ovl1 camera setups: has &func_800A7394 at entry 2
    'D_800BFEF8',   # ovl1 camera setups: &func_800A71E0, &func_800A7348
}

# Camera animation command streams embedded in overlay DATA. Bank-loaded
# anim blocks are widened at load time by func_800A94F4 -- each N64 word
# becomes an 8-byte cell (native value, zero high half) -- because LP64's
# AnimCmd union carries a pointer and is 8 bytes, so `animList++` strides 8.
# These tables feed the very same reader (func_800B2F54 ->
# animSetCameraAnimation -> animProcessCameraAnimation) but arrive as dense
# u32[]; the reader skipped every other word and spun forever (the world-map
# scene hang). Emitting them as void*[] with each word in a (void *)(u32)
# slot reproduces the widened-cell shape exactly. Grown per finding: any
# other overlay-resident table passed to func_800B2F54 /
# animSetCameraAnimation belongs here.
FORCE_WIDEN = {
    'D_8015A0F0_ovl4', 'D_8015A224_ovl4',   # world map, via D_8015A954
    'D_8015B780_ovl4', 'D_8015BB48_ovl4',   # world map, via D_8015C360
    'D_80186960_ovl5',                      # ovl5_4 scene camera
    'D_8018A530_ovl5',                      # ovl5_13 scene camera
    # Enemy descriptor read through struct Sub800E1B50_Unk88 (whose PORT
    # overlay assumes 8-byte cells) but emitted as scalar u32[] because none
    # of its words happen to be relocations. Every sibling descriptor is a
    # pointer block or a mixed block and widens on its own.
    'D_801C5130_ovl7',
}

# Light structs the RSP consumes as a raw BYTE stream. gSPLight/gSPSetLights1
# moves 8/16 bytes of Light/Ambient straight into light state, and the fork's
# GfxSpMovememF3dex2 memcpys them the same way: col[0] is the FIRST BYTE in
# memory. The normal scalar-u32 emission stores each N64 word as a native
# little-endian value, which puts the trailing 0x00 pad byte where col[0]
# (red) should be -- every lit model rendered with red=0 and its green/blue
# shifted (the dark-teal title-screen characters). CPU readers of these
# blocks are byte readers too (func_800A54FC, func_800A7BF4, the u32 copies
# in ovl2_2.c only move memory), so the only correct host image is the N64's
# byte image. Emitted as .byte asm blobs (see FORCE_BYTES_RUNS for why not
# plain u8[]). Grown per finding: any other data block handed to
# gSPLight / gSPSetLights* belongs here.
FORCE_BYTES = {
    'D_800BE548',       # scene ambient (Lights1 head): B4 B4 B4
    'D_800BE550',       # scene diffuse light: FF FF FF, dir 32 3C 28
    'D_801C27D0_ovl7',  # enelib alt ambient: 64 50 96
    'D_801C27D8_ovl7',  # enelib alt diffuse: FF FF 50, dir EC C4 14
}

# FORCE_BYTES symbols that game code addresses through ONE Lights1 struct
# (gsSPSetLights1(D_800BE548) reads the diffuse at &D_800BE548 + 8, i.e.
# D_800BE550): each run is emitted as a single asm blob in the file that
# holds its head, with the later members as interior labels, because
# separate C objects -- even in the same TU -- give the linker license to
# leave a gap between them (and D_800BE548/D_800BE550 don't even share a
# TU). The tail members' own blocks emit nothing.
FORCE_BYTES_RUNS = [
    ('D_800BE548', 'D_800BE550'),
    ('D_801C27D0_ovl7', 'D_801C27D8_ovl7'),
]
_FB_TAILS = {s for run in FORCE_BYTES_RUNS for s in run[1:]}
_FB_DATA = {}   # sym -> (section, [byte, ...]); pre-pass fills it


def _fb_bytes(entries):
    out = []
    for _k, v in entries:
        w = int(v, 0) & 0xFFFFFFFF
        out += list(w.to_bytes(4, 'big'))
    return out


def _fb_asm(section, parts):
    """One asm blob defining [(sym, bytes), ...] back to back."""
    sect = '.rodata' if section == '.rodata' else '.data'
    lines = [f'   .pushsection {sect}', '   .balign 8']
    for sym, data in parts:
        lines.append(f'   .globl {sym}')
        lines.append(f'{sym}:')
        for i in range(0, len(data), 8):
            lines.append('   .byte ' + ', '.join(f'0x{b:02X}'
                                                 for b in data[i:i + 8]))
    lines.append('   .popsection')
    return ('__asm__(' + '\n        '.join(f'"{l}\\n"' for l in lines)
            + ');\n')


def render_bytes(sym, section, entries):
    """A FORCE_BYTES block: the N64 byte image, one u8 per ROM byte."""
    const = 'const ' if section == '.rodata' else ''
    if sym in _FB_TAILS:
        # Emitted inside its run head's blob; only the declaration here.
        return f'extern {const}u8 {sym}[];\n', ''
    for run in FORCE_BYTES_RUNS:
        if run[0] == sym:
            parts = [(s, _FB_DATA[s][1]) for s in run if s in _FB_DATA]
            if len(parts) == len(run):
                fwd = ''.join(f'extern {const}u8 {s}[];\n' for s, _ in parts)
                return fwd, _fb_asm(section, parts)
            break   # partner listing missing; fall back to standalone
    return (f'extern {const}u8 {sym}[];\n',
            _fb_asm(section, [(sym, _fb_bytes(entries))]))


# The one mixed pointer-bearing block that really IS a string table: 632
# entries of sound-name text next to pointer words. Widening would fragment
# every string across 8-byte cells; it keeps the packed-struct emission.
# All other mixed pointer-bearing blocks are enemy descriptors whose leading
# float parsed as a 1-2 char "string" (0x3F/0x40 bytes read as '?','@'), and
# those MUST widen -- see render_mixed_widened.
MIXED_KEEP_PACKED = {'sSoundNames'}


def is_mixed_ref_block(sym, entries):
    """A block mixing .asciz/.short/etc with at least one pointer .word."""
    entries = [e for e in entries if e[0] != 'incbin']
    if not entries or sym in MIXED_KEEP_PACKED:
        return False
    kinds = {k for k, _ in entries}
    return (len(kinds) > 1
            and any(_is_ref(v) for k, v in entries if k == 'word'))


def is_widened_block(sym, entries):
    """Does this block emit as 8-byte cells? (gen_defsyms scales by this.)"""
    entries = [e for e in entries if e[0] != 'incbin']
    if not entries:
        return False
    kinds = {k for k, _ in entries}
    if sym in FORCE_WIDEN and kinds == {'word'}:
        return True
    return is_pointer_block(entries) or is_mixed_ref_block(sym, entries)


def _asciz_bytes(lit):
    """Raw bytes of one .asciz literal, terminator included."""
    s = lit.strip()
    body = s[1:-1] if len(s) >= 2 and s[0] == '"' and s[-1] == '"' else s
    out, i = bytearray(), 0
    esc = {'n': 10, 't': 9, 'r': 13, '0': 0, '\\': 92, '"': 34,
           'f': 12, 'b': 8, 'v': 11, 'a': 7}
    while i < len(body):
        c = body[i]
        if c != '\\':
            out.append(ord(c) & 0xFF)
            i += 1
            continue
        i += 1
        e = body[i]
        if e in 'xX':
            j = i + 1
            while j < len(body) and body[j] in '0123456789abcdefABCDEF':
                j += 1
            out.append(int(body[i + 1:j], 16) & 0xFF)
            i = j
        elif e.isdigit():
            j = i
            while j < len(body) and body[j].isdigit() and j < i + 3:
                j += 1
            out.append(int(body[i:j], 8) & 0xFF)
            i = j
        else:
            out.append(esc.get(e, ord(e)))
            i += 1
    out.append(0)
    return bytes(out)


def _ref_word_expr(v, refs):
    """C expression for one relocated .word, host-offset-scaled."""
    if re.fullmatch(r'\w+', v):
        refs.add(v)
        return f'&{v}'
    m = re.match(r'(\w+)\s*([+-])\s*(\S+)', v)
    if m:
        base, sign, off = m.group(1), m.group(2), m.group(3)
        refs.add(base)
        try:
            n = int(off, 0)
            if base in _WIDENED_REGISTRY:
                n *= 2
            return f'(void *)((u8 *)&{base} {sign} {n})'
        except ValueError:
            return f'(void *)((u8 *)&{base} {sign} ({off}))'
    return '(void *)0'


def render_mixed_widened(sym, section, entries, refs):
    """A descriptor block splat mis-parsed as strings+words: emit 8-byte cells.

    The game reads these through LP64 structs (struct Sub800E1B50_Unk88 and
    friends) whose PORT overlays assume one 8-byte cell per N64 word. The old
    packed-struct emission was wrong twice over: it dropped the .align padding
    after each .asciz (splat writes a float word 0x3F000000 as `.asciz "?"`
    plus alignment, and the padding IS data here), and packed pointers at
    byte offsets no reader struct has. Rebuild the exact N64 byte stream,
    then cut it into big-endian words, one cell each; relocated words stay
    native pointers. Sub-word fields are read back with the same big-endian
    extraction idiom every other widened reader in src/ uses.
    """
    import struct as _struct
    const = 'const ' if section == '.rodata' else ''
    stream, n = [], 0            # ('b', bytes) | ('r', expr), N64 offset

    def pad_to(align):
        nonlocal n
        if n % align:
            k = align - n % align
            stream.append(('b', b'\0' * k))
            n += k

    for k, v in entries:
        if k == 'asciz':
            b = _asciz_bytes(v)
            stream.append(('b', b))
            n += len(b)
            pad_to(4)            # splat always aligns back to word here
        elif k == 'word':
            pad_to(4)
            if _is_ref(v):
                stream.append(('r', _ref_word_expr(v, refs)))
            else:
                stream.append(('b', _struct.pack('>I', int(v, 0) & 0xFFFFFFFF)))
            n += 4
        elif k == 'float':
            pad_to(4)
            stream.append(('b', _struct.pack('>f', float(v))))
            n += 4
        elif k == 'double':
            pad_to(4)
            stream.append(('b', _struct.pack('>d', float(v))))
            n += 8
        elif k == 'short':
            pad_to(2)
            stream.append(('b', _struct.pack('>H', int(v, 0) & 0xFFFF)))
            n += 2
        elif k == 'byte':
            stream.append(('b', _struct.pack('B', int(v, 0) & 0xFF)))
            n += 1
        elif k == 'space':
            z = int(v, 0)
            stream.append(('b', b'\0' * z))
            n += z
    pad_to(4)

    cells, buf = [], b''
    for kind, x in stream:
        if kind == 'b':
            buf += x
            while len(buf) >= 4:
                w, buf = buf[:4], buf[4:]
                word = int.from_bytes(w, 'big')
                expr = resolve_vram_interior(hex(word), refs)
                cells.append(expr or '(void *)(u32)(0x%08X)' % word)
        else:
            if buf:
                raise SystemExit(f'{sym}: relocation not word-aligned')
            cells.append(x)
    if buf:
        raise SystemExit(f'{sym}: trailing sub-word bytes')

    body = ',\n    '.join(cells)
    return (f'extern {const}void *{sym}[];\n',
            f'{const}void *{c_ident(sym)}[] = {{\n    {body}\n}};\n')


def render_widened(sym, section, entries):
    """A FORCE_WIDEN block: one 8-byte (void *)(u32) cell per N64 word."""
    const = 'const ' if section == '.rodata' else ''
    body = ',\n    '.join(f'(void *)(u32)({v})' for _k, v in entries)
    return (f'extern {const}void *{sym}[];\n',
            f'{const}void *{c_ident(sym)}[] = {{\n    {body}\n}};\n')


# BSS blocks whose PC-side definition lives in src/pc/pc_camera_slots.c.
# These are two N64 pointer arrays (GObj *D_800D79B0[10], camera slots
# D_800D79D8[10]) that splat split at every interior label. The 2x doubling
# rule keeps each PIECE big enough, but game code both indexes the BASE with
# LP64 8-byte slots (D_800D79B0[idx] = obj) and reads the INTERIOR labels as
# scalars (D_800D79BC is N64 base+12, i.e. index 3) -- and no doubling of the
# split pieces can satisfy both views at once. func_800A7394 found it: the
# world-map camera callback read D_800D79BC, which sat 24 bytes past where
# D_800D79B0[3] was written. The hand-written file defines each array whole
# and aliases the interior labels at index*8.
SUPPRESS_BSS = {
    'D_800D79B0', 'D_800D79B4', 'D_800D79B8', 'D_800D79BC',
    'D_800D79D8', 'D_800D79DC', 'D_800D79E0',
    # ovl4 planet-map track-id table: D_8015C6AC is D_8015C6A8 + 4 on N64
    # (one s32 array split at the interior label; writers use the C6AC name,
    # readers index from C6A8, and split objects broke the aliasing).
    'D_8015C6A8_ovl4', 'D_8015C6AC_ovl4',
    # The save buffers and every named address inside them, defined whole in
    # src/pc/pc_save_bss.c. Splintered-and-doubled emission made writers
    # (struct EEPROM at native offsets) and readers (splinter names) see
    # different memory; the mixed half-real save records that bricked the
    # world map came from exactly this.
    'gSaveBuffer1', 'gSaveBuffer2',
    'D_800EC9FC', 'D_800ECA00', 'D_800ECA04', 'D_800ECA08', 'D_800ECA14',
    'D_800ECA5C', 'D_800ECA60', 'D_800ECAB8', 'D_800ECB00', 'D_800ECB10',
    'D_800ECBA8', 'D_800ECBAC', 'D_800ECBC0',
}


def render_word32_asm(sym, section, entries, refs):
    """A FORCE_WORD32 block: N64-shaped 4-byte words, pointers included."""
    # pushsection/popsection, NOT .section/.text: GCC does not parse the asm,
    # so a bare section switch desyncs its own section tracking and the next
    # compiler-emitted object lands wherever the asm left the assembler (the
    # first attempt put D_800C96D8 -- 8 KB of mutable table -- in .text, and
    # the game faulted writing to it).
    sect = '.rodata' if section == '.rodata' else '.data'
    lines = [f'   .pushsection {sect}', '   .balign 8', f'   .globl {sym}',
             f'{sym}:']
    for _k, v in entries:
        if _is_ref(v):
            m = re.match(r'([A-Za-z_]\w*)', v)
            refs.add(m.group(1))
            lines.append(f'   .long {v}')
        else:
            name = resolve_scalar_pointer(v)
            if name:
                refs.add(name)
                lines.append(f'   .long {name}')
            else:
                lines.append(f'   .long {v}')
    lines.append('   .popsection')
    asm = ('__asm__(' + '\n        '.join(f'"{l}\\n"' for l in lines)
           + ');\n')
    const = 'const ' if section == '.rodata' else ''
    return f'extern {const}u32 {sym}[];\n', asm


def render(sym, section, entries, refs):
    """(forward_declaration, definition) for one data block.

    Pointer words become `&symbol` so the host linker resolves them, which is
    the whole reason this translation is possible at all: splat already turned
    every pointer word into a symbol name rather than a raw address.
    """
    const = 'const ' if section == '.rodata' else ''
    entries = [e for e in entries if e[0] != 'incbin']
    if not entries:
        return '', ''

    kinds = {k for k, _ in entries}

    if sym in FORCE_WORD32 and kinds == {'word'}:
        return render_word32_asm(sym, section, entries, refs)

    if sym in FORCE_WIDEN and kinds == {'word'}:
        return render_widened(sym, section, entries)

    if sym in FORCE_BYTES and kinds == {'word'}:
        return render_bytes(sym, section, entries)

    # .bss -- plain zeroed storage, and it must NOT be const.
    #
    # DOUBLED, and this is not caution, it is a correctness fix found by a
    # crash. The listing records the size the symbol had on N64, where a
    # pointer is 4 bytes. This build is LP64. Every bss object that holds
    # pointers is therefore too small by exactly the number of pointers in it,
    # and the game writes past the end of it into whatever the linker put next.
    #
    # The one that found it: sched.c declares `OSMesg D_80048C98[8]` and the
    # listing says `.space 32`. At LP64 an OSMesg is 8 bytes, so the queue
    # needs 64 -- and the 32 bytes it ran into were scTaskMQ, whose mtqueue
    # field became the message value 1. osSendMesg then dereferenced 0x1.
    #
    # 2x is an exact upper bound rather than a guess: the only thing that grows
    # is a pointer, 4 -> 8, and alignment inside these structs never exceeds 8,
    # so no layout can more than double. It costs address space and nothing
    # else -- these are zeroed pages the game never reads past its own extent.
    # It cannot disturb tools/pc/gen_defsyms.py either, which resolves an
    # absolute N64 address to <symbol>+<offset from that symbol's N64 start>;
    # growing a symbol does not move its own start.
    if kinds == {'space'}:
        n = sum(int(v, 0) for _, v in entries)
        return f'extern u8 {sym}[];\n', f'u8 {c_ident(sym)}[{n * 2}];\n'

    # Strings. The listing may hold several in one block, in which case the
    # only faithful C form is a flat char array with the terminators kept.
    if kinds == {'asciz'}:
        lits = ' '.join(v for _, v in entries)
        return (f'extern {const}char {sym}[];\n',
                f'{const}char {c_ident(sym)}[] = {lits};\n')

    # A block that is BOTH pointer-bearing and mixed-width cannot be an array
    # of anything. 24 blocks are like this -- string tables where inline .asciz
    # data sits next to pointer words (sSoundNames, D_80192F50_ovl3, ...). An
    # earlier version emitted a void* array with a placeholder for each string,
    # which lost the string AND shifted every later index, because a 9-byte
    # string is not one pointer slot. A packed struct is the only faithful
    # form, and it also beats byte-serialisation for the 41 mixed-width blocks
    # with no pointers, since it keeps the pointers and the values both.
    has_ref = any(_is_ref(v) for k, v in entries if k == 'word')
    if is_mixed_ref_block(sym, entries):
        return render_mixed_widened(sym, section, entries, refs)
    if len(kinds) > 1:
        fields, inits = [], []
        for i, (k, v) in enumerate(entries):
            if k == 'asciz':
                n = len(v.strip('"').encode('latin-1', 'replace')) + 1
                fields.append(f'    char f{i}[{n}];')
                inits.append(v)
            elif k == 'space':
                fields.append(f'    u8 f{i}[{int(v, 0)}];')
                inits.append('{ 0 }')
            elif k == 'word' and _is_ref(v):
                m = re.match(r'([A-Za-z_]\w*)', v)
                refs.add(m.group(1))
                fields.append(f'    void *f{i};')
                inits.append(f'&{v}' if re.fullmatch(r'\w+', v)
                             else f'(void *)((u8 *)&{m.group(1)} + 0)')
            else:
                fields.append(f'    {CTYPE.get(k, "u32")} f{i};')
                inits.append(v)
        tag = f'{sym}_t'
        decl = ('struct __attribute__((packed)) ' + tag + ' {\n'
                + '\n'.join(fields) + '\n};\n'
                + f'extern {const}struct {tag} {sym};\n')
        body = (f'{const}struct {tag} {c_ident(sym)} = {{\n    '
                + ',\n    '.join(inits) + '\n};\n')
        return decl, body

    # Pure pointer array: only the linker can supply these addresses. A pointer
    # word is 4 bytes and so is a u32, which is precisely why the port is ILP32.
    if has_ref:
        body = []
        for k, v in entries:
            if not _is_ref(v):
                expr = resolve_vram_interior(v, refs)
                body.append(expr or f'(void *)(u32)({v})')
            else:
                body.append(_ref_word_expr(v, refs))
        return (f'extern {const}void *{sym}[];\n',
                f'{const}void *{c_ident(sym)}[] = {{\n    ' +
                ',\n    '.join(body) + '\n};\n')

    # Homogeneous scalar block -- emit its natural C type so the host reads it
    # with the same value the N64 would. These are VALUES, not a byte image:
    # writing 0x3F800000 as a u32 gives the right float on either endianness,
    # whereas copying the ROM's bytes would not.
    k = next(iter(kinds))
    if k in CTYPE:
        vals = ', '.join(v for _, v in entries)
        return (f'extern {const}{CTYPE[k]} {sym}[];\n',
                f'{const}{CTYPE[k]} {c_ident(sym)}[] = {{ {vals} }};\n')
    return '', ''


def is_pointer_block(entries):
    """A block that renders as `void *sym[]` -- all .word, at least one a ref."""
    entries = [e for e in entries if e[0] != 'incbin']
    if not entries:
        return False
    return ({k for k, _ in entries} == {'word'}
            and any(_is_ref(v) for _, v in entries))


def render_pointer_run(run, refs):
    """One C array for a run of adjacent pointer blocks, plus interior aliases.

    WHY ADJACENT POINTER BLOCKS MUST BE MERGED AT LP64.
    ---------------------------------------------------
    splat puts a `dlabel` wherever anything in the ROM refers to an address, so
    a single array in the source can arrive here as several blocks. In
    asm/data/ovl1/ovl1_2.data.s one ten-element pointer table is three of them:

        dlabel D_800BF8F0   .word D_800D7990          <- 1 entry
        dlabel D_800BF8F4   .word D_800D7994          <- 1 entry
        dlabel D_800BF8F8   .word D_800D7998 ... x8   <- 8 entries

    On the N64 that does not matter: the labels are addresses in one contiguous
    run of 4-byte words, so `D_800BF8F0[i]` for i in 0..9 reads all three.
    Emitting them as three separate C objects breaks that at ANY pointer width,
    and LP64 makes it worse -- the elements are 8 bytes here, so even the
    offsets between the labels are no longer the ROM's.

    src/ovl1/ovl1_2.c:46 is the loop that found it:

        for (i = 0; i < 10; i++) {  ...  *D_800BF8F0[i] = 0;  }

    D_800BF8F0[2] read the padding after a one-element object, got NULL, and
    the port died storing through it -- during scene setup, several frames into
    a boot that had just started working.

    So a run of adjacent pointer blocks becomes ONE array, and every label
    after the first becomes a symbol at its offset inside it. C cannot name a
    location inside an array, so the aliases are `.set`, which is the same
    mechanism tools/pc/gen_defsyms.py already uses for interior symbols with no
    block of their own.

    Only runs of PURE POINTER blocks are merged. A neighbouring scalar block is
    left alone: its elements are still their natural width, so merging it in
    would change what every index means.
    """
    base = run[0][0]
    section = run[0][1]
    const = 'const ' if section == '.rodata' else ''
    body, fwds, aliases = [], [], []
    offset = 0

    for idx, (sym, _sec, entries) in enumerate(run):
        entries = [e for e in entries if e[0] != 'incbin']
        if idx > 0:
            fwds.append(f'extern {const}void *{sym}[];\n')
            # `.set A, B + n` defines A inside B's section at that offset --
            # a real symbol the linker resolves, not a C-level alias.
            aliases.append('__asm__("   .globl ' + sym + '\\n"\n'
                           '        "   .set ' + sym + ', ' + base +
                           ' + ' + str(offset) + '\\n");\n')
        for k, v in entries:
            if not _is_ref(v):
                expr = resolve_vram_interior(v, refs)
                body.append(expr or f'(void *)(u32)({v})')
            else:
                body.append(_ref_word_expr(v, refs))
            offset += 8

    fwd = f'extern {const}void *{base}[];\n' + ''.join(fwds)
    definition = (f'{const}void *{c_ident(base)}[] = {{\n    '
                  + ',\n    '.join(body) + '\n};\n' + ''.join(aliases))
    return fwd, definition


def group_blocks(blocks):
    """[(kind, [block, ...])] -- adjacent pointer blocks collected into runs."""
    groups, i = [], 0
    while i < len(blocks):
        # FORCE_WORD32 blocks must stay single: merged into a pointer run
        # they would be re-widened to 8-byte slots, undoing the override.
        if (is_pointer_block(blocks[i][2])
                and blocks[i][0] not in FORCE_WORD32):
            j = i + 1
            while (j < len(blocks) and blocks[j][1] == blocks[i][1]
                   and is_pointer_block(blocks[j][2])
                   and blocks[j][0] not in FORCE_WORD32):
                j += 1
            groups.append(('ptrrun', blocks[i:j]))
            i = j
        else:
            groups.append(('single', blocks[i:i + 1]))
            i += 1
    return groups


def main():
    outdir = 'build/pc/data'
    if '-o' in sys.argv:
        outdir = sys.argv[sys.argv.index('-o') + 1]
    os.makedirs(outdir, exist_ok=True)

    # PRUNE GENERATED FILES WHOSE LISTING NO LONGER EXISTS.
    #
    # This directory is generated but never emptied, so a subsegment RENAME
    # leaves the old file behind and the port links both. The ovl3 rodata
    # migration renamed asm/data/ovl3/F7A30 to ovl3/plyshot and split off
    # ovl3/plyshot_2; the stale ovl3_F7A30.c stayed, and the link died on
    # eleven "multiple definition of D_801971xx_ovl3" plus a duplicated jump
    # table. Nothing in the error named F7A30 as stale -- it read as the
    # migration having emitted a constant twice.
    #
    # A generated .c is live iff the .s it came from is still on disk. Its .o
    # goes with it, or make links the object without ever regenerating it.
    live = {p[len('asm/data/'):-2].replace('/', '_')
            for p in glob.glob('asm/data/**/*.s', recursive=True)}
    pruned = 0
    for old in glob.glob(f'{outdir}/*.c'):
        stem = os.path.basename(old)[:-2]
        if stem not in live:
            os.remove(old)
            for ext in ('.o', '.d'):
                p = old[:-2] + ext
                if os.path.exists(p):
                    os.remove(p)
            pruned += 1
    if pruned:
        print(f'pruned {pruned} generated data file(s) whose listing is gone')

    # Anything already defined in C wins; emitting it again is a duplicate
    # symbol at link time.
    defined = set()
    for cf in glob.glob('src/**/*.c', recursive=True):
        txt = open(cf).read()
        for m in re.finditer(r'^(?!extern)(?:[\w\*]+[ \t]+)+?(\w+)\s*(?:\[[^\]]*\])?\s*=',
                             txt, re.M):
            defined.add(m.group(1))

    # Not every .s on disk is live. 54 rodata listings exist under two names --
    # `<file>.rodata.s` and `<file>_rd.rodata.s` -- byte-identical duplicates
    # left behind when the subsegments were renamed to break a splat name
    # collision. The N64 build links only the `_rd` object, so the plain one is
    # stale; emitting both gives thousands of multiple-definition errors at the
    # native link. build/kirby.ld is the authority on which is real.
    live = None
    if os.path.exists('build/kirby.ld'):
        ld = open('build/kirby.ld').read()
        live = set(re.findall(r'build/asm/data/(\S+?)\.o', ld))
        if not live:
            live = None

    # Pre-pass: every block that will emit as 8-byte cells, across ALL files.
    # Interior pointer resolution consults this to scale offsets, and it must
    # be complete before the first file renders -- ovl7 tables point into
    # ovl8/ovl10 blocks that render later.
    global _WIDENED_REGISTRY
    for path in sorted(glob.glob('asm/data/**/*.s', recursive=True)):
        if live is not None and path[len('asm/data/'):-2] not in live:
            continue
        for sym, _sec, entries in parse(path):
            if sym not in defined and sym not in SUPPRESS_BSS \
                    and is_widened_block(sym, entries):
                _WIDENED_REGISTRY.add(sym)
            if sym in FORCE_BYTES and sym not in defined:
                ent = [e for e in entries if e[0] != 'incbin']
                if ent and {k for k, _ in ent} == {'word'}:
                    _FB_DATA[sym] = (_sec, _fb_bytes(ent))

    nfiles = nsyms = skipped = nmerged = 0
    for path in sorted(glob.glob('asm/data/**/*.s', recursive=True)):
        if live is not None and path[len('asm/data/'):-2] not in live:
            skipped += 1
            continue
        # A block already defined in C is dropped, and it also BREAKS a run:
        # its C definition has whatever size that source says, so the blocks
        # around it are no longer known to be adjacent.
        segments, seg = [], []
        for b in parse(path):
            if b[0] in defined or b[0] in SUPPRESS_BSS:
                if seg:
                    segments.append(seg)
                seg = []
            else:
                seg.append(b)
        if seg:
            segments.append(seg)
        blocks = [b for s in segments for b in s]
        if not blocks:
            continue
        groups = [g for s in segments for g in group_blocks(s)]
        rel = path[len('asm/data/'):-2].replace('/', '_')
        with open(f'{outdir}/{rel}.c', 'w') as f:
            f.write('/* GENERATED by tools/pc/gen_data.py -- do not edit.\n'
                    '   PC build only; the N64 build assembles the .s directly. */\n'
                    '#include "pc/pc_types.h"\n\n')
            refs, fwds, bodies = set(), [], []
            for kind, run in groups:
                if kind == 'ptrrun' and len(run) > 1:
                    fwd, body = render_pointer_run(run, refs)
                    nmerged += len(run) - 1
                else:
                    sym, section, entries = run[0]
                    fwd, body = render(sym, section, entries, refs)
                fwds.append(fwd)
                bodies.append(body)
                nsyms += len(run)
            # A block routinely points at a symbol defined LATER in the same
            # file, so every local symbol needs a forward declaration -- and it
            # must carry the SAME type as its definition, because `extern u8 X;`
            # against a `u32 X[]` definition is a hard type conflict, not a
            # warning. Symbols from other files get the opaque `extern u8`.
            local = {b[0] for b in blocks}
            f.writelines(fwds)
            for s in sorted(refs - local):
                f.write(f'extern u8 {s};\n')
            f.write('\n')
            f.writelines(bodies)
        nfiles += 1
    print(f'{nsyms} data symbols -> {nfiles} C files in {outdir}'
          + (f' ({skipped} stale listing(s) skipped)' if skipped else '')
          + (f', {nmerged} interior pointer labels aliased into their run'
             if nmerged else ''))


if __name__ == '__main__':
    main()
