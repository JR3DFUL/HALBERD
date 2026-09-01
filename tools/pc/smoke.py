#!/usr/bin/env python3
"""How far does the native port get? Run it headless and say so.

    make -f Makefile.pc smoke          unattended: how far does it boot on its own
    make -f Makefile.pc smoke-deep     driven: how far can it be pushed with input
    python3 tools/pc/smoke.py --real-time --timeout 120
    python3 tools/pc/smoke.py --input "2:START,3:A,4:START"

TWO QUESTIONS, TWO MODES. Left alone the port cannot leave the attract loop --
game_tick() only does that when someone presses START -- so the default run
measures the boot and stops at that ceiling, which is the right thing for a
regression check. --deep adds the synthetic controller from
src/pc/pc_input_script.c and measures how far the port can actually be driven,
which is the right thing when you are hunting the next crash.

WHY THIS EXISTS. The port links and boots, so the question stopped being
"which symbol is missing" -- tools/pc/gap.py answers that -- and became "which
frame does it die on". That answer was only available by hand: run the binary,
wait a minute, read a gdb backtrace. A number nobody can produce twice is not
a measurement, so this turns the boot into one line that a make target prints
and a person can compare against the last one.

WHAT IT MEASURES. src/pc/pc_progress.c tracks the furthest NAMED milestone the
process reached and prints a single machine-readable [verdict] line however
the run ends -- clean exit, timeout, or fatal signal. The milestones are the
boot sequence of src/ovl1/game.c's game_tick(); that file has the full list and
where each name comes from. This script runs the binary, parses that line,
resolves any crash backtrace to file:line with addr2line, and exits non-zero if
the run did not reach the stage it is expected to.

WHY IT IS NOT REAL-TIME BY DEFAULT. Unattended, the boot is ~66 seconds of
logos, opening movie and title screen before the first attract demo, which is
too long for a check anyone runs after every edit. KIRBY_PC_TIMESCALE (see
src/pc/os_time.c) scales the game's count register, so the whole simulated
world -- retraces, scheduler deadlines, animation -- moves together at N times
wall clock and the same boot takes about 8 seconds. Nothing in the game can
observe the difference. --real-time turns it off when you suspect it can.

THE EXPECTED STAGE IS A RATCHET, and deliberately a dumb one: a constant in
this file, raised by hand when a fix makes the port reach further. It is the
high-water mark somebody actually measured, so a regression shows up as a
failing smoke test rather than as a slightly different anecdote.
"""
import argparse
import os
import re
import subprocess
import sys

# Every tool in this tree derives the repo root from its own location. No path
# belonging to whoever is running it may ever appear in the repository.
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BINARY = os.path.join('build', 'pc', 'kirby64')
DEFAULT_ROM = 'baserom.us.z64'

# Ordered; index is the comparison. Must agree with the PC_STAGE_* constants in
# src/pc/pc_platform.h and the names in src/pc/pc_progress.c.
STAGES = [
    'process-start',
    'osInitialize',
    'first-thread-dispatched',
    'first-vi-retrace',
    'first-graphics-task',
    'logos',
    'opening-movie',
    'title-screen',
    'attract-demo-1',
    'title-screen-2',
    'attract-demo-2',
    'title-screen-3',
    'attract-demo-3',
    'title-screen-4',
    'attract-loop-complete',
    'file-select-menu',
    'world-select',
    'level-select',
    'intro-cutscene',
    'gameplay',
]

# THE RATCHET. Raise this only after MEASURING the new stage, and say what
# changed to earn it. The history is the useful part of this comment:
#
#   attract-demo-1  the port boots through the HAL/Nintendo logos, the opening
#                   movie and the title screen, and enters the first attract
#                   demo, where it dies in the collision raycast -- the
#                   trailing `s32` parameters on src/ovl2/ovl2_7.c's
#                   CollisionState wrapper family are pointers and truncate
#                   under LP64. FIXED.
#   attract-demo-2  demo 1 plays to the end and the title screen comes back;
#                   demo 2 dies in func_8019F410_ovl7 reading an anim header
#                   through EnemyKindDesc.animTable -- the PORT-widened
#                   struct in include/unk_structs/D_800E1B50.h padded its
#                   8-byte pointer fields as if they were 4 bytes, so
#                   animTable sat in cell 7 (terrainKind) instead of cell 5.
#                   FIXED, commit 59eef667.
#   attract-loop-   all three demos play, the title screen returns between
#   complete        each, and game_tick's case 9 sends gGameState back to 2:
#                   the whole unattended cycle, closed. Measured twice round
#                   in a 90s run with no crash. This is the ceiling without
#                   input -- for anything past it see DEEP_EXPECTED_STAGE.
EXPECTED_STAGE = 'attract-loop-complete'

# THE SECOND RATCHET, for --deep. An unattended run cannot leave the attract
# loop -- game_tick() only does that when someone presses START -- so
# everything past it is measured with the synthetic controller in
# src/pc/pc_input_script.c. Kept separate from EXPECTED_STAGE because the two
# answer different questions: the default one is "did the port regress", this
# one is "how deep can it be driven", and a deep run needs a ROM, input and
# more patience.
#
#   intro-cutscene  title -> file select -> world select -> the world 1-1
#                   opening cutscene (overlay 18), held for 150s / 32915
#                   frames with no crash, driven by `autostart`. It did not
#                   reach gGameState 15 because `autostart` presses only
#                   START and A, and A answers YES to the watch-the-cutscene
#                   prompt every time it comes round (src/pc/pc_input_script.c
#                   has the mechanism). Not a bug: a prompt nobody answered.
#   gameplay        driven by `advance`, which picks the button from
#                   gGameState and answers that prompt with D-LEFT then A.
#                   Measured 2026-09-01 on llvmpipe, `--input walk` (advance
#                   plus a held D-RIGHT in-level), timescale 8: gameplay at
#                   45.33 s, route 0>1>2>3>10>11>12>15, render=raster,
#                   drawn=19965 sampled=2495 nonblank=2489 distinct=2485,
#                   300 s with no fault. --deep now drives with `advance`.
DEEP_EXPECTED_STAGE = 'gameplay'

# Beyond the expected stage there is nothing to wait for: with no controller
# attached the game cycles the attract loop forever, so a run that gets that
# far is a PASS that then has to be stopped. The default timeout is sized for
# the accelerated boot with room to spare -- the full unattended cycle is
# about 42 accelerated seconds.
DEFAULT_TIMEOUT = 60
DEFAULT_TIMESCALE = 8

VERDICT_RE = re.compile(r'^\[verdict\] (.*)$', re.M)
FRAME_RE = re.compile(r'^(\S*kirby64)(?:\(([^)+]*)\+?0x[0-9a-f]*\))?\[(0x[0-9a-f]+)\]')


def parse_verdict(text):
    m = None
    for m in VERDICT_RE.finditer(text):
        if m.group(1).startswith('outcome='):
            break
    if m is None or not m.group(1).startswith('outcome='):
        return None
    out = {}
    for tok in m.group(1).split():
        if '=' in tok:
            k, v = tok.split('=', 1)
            out[k] = v
    return out


def resolve_backtrace(text, binary):
    """Turn pc_progress.c's raw frames into file:line, innermost first.

    backtrace_symbols_fd gives a name (because tools/pc/link.py links
    -rdynamic) and an address; addr2line turns the address into the source
    line, which is the part that says WHY. Frames outside the binary -- libc,
    the signal trampoline -- carry no game meaning and are dropped."""
    started = False
    addrs = []
    for line in text.split('\n'):
        if line.startswith('[verdict] backtrace:'):
            started = True
            continue
        if not started:
            continue
        m = FRAME_RE.match(line.strip())
        if m:
            addrs.append(m.group(3))
    if not addrs:
        return []
    try:
        r = subprocess.run(['addr2line', '-f', '-p', '-e', binary] + addrs,
                           capture_output=True, text=True)
    except OSError:
        return []
    if r.returncode != 0:
        return []
    out = []
    for line in r.stdout.strip().split('\n'):
        if '??' in line:
            continue
        # The signal handler is on top of every crash backtrace and says
        # nothing about the crash. Drop it so the first line printed is the
        # frame that actually faulted.
        if line.startswith('fatal at ') and 'pc_progress.c' in line:
            continue
        # addr2line -p prints absolute paths; strip the repo prefix so no
        # local path can be pasted into a report or a commit message.
        out.append(line.replace(REPO + os.sep, '').replace(REPO, '.'))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--timeout', type=int, default=DEFAULT_TIMEOUT,
                    help=f'seconds before the run is stopped (default {DEFAULT_TIMEOUT})')
    ap.add_argument('--timescale', type=int, default=DEFAULT_TIMESCALE,
                    help=f'game-clock multiplier (default {DEFAULT_TIMESCALE}, 1 = real time)')
    ap.add_argument('--real-time', action='store_true',
                    help='shorthand for --timescale 1 --timeout 120')
    ap.add_argument('--input', default=None, metavar='SPEC',
                    help='drive a synthetic controller: "autostart", or a '
                         'timeline like "2:START,3:A" -- see '
                         'src/pc/pc_input_script.c. Without it the run is '
                         'unattended and cannot leave the attract loop.')
    ap.add_argument('--deep', action='store_true',
                    help='shorthand for --input advance with a longer '
                         'timeout and the deepest reachable stage expected')
    ap.add_argument('--expect', default=EXPECTED_STAGE, choices=STAGES,
                    help=f'stage that must be reached (default {EXPECTED_STAGE})')
    ap.add_argument('--rom', default=None,
                    help=f'base ROM supplying assets (default ./{DEFAULT_ROM})')
    ap.add_argument('--verbose', action='store_true',
                    help='log every milestone, and show the run output in full')
    args = ap.parse_args()

    os.chdir(REPO)

    if args.real_time:
        args.timescale = 1
        if args.timeout == DEFAULT_TIMEOUT:
            args.timeout = 120

    if args.deep:
        if args.input is None:
            args.input = 'advance'
        # Reaching gameplay took 45 s on llvmpipe (the timescale does not
        # speed up a renderer-bound simulation; see the doc), so the deep
        # budget is sized for that plus a margin.
        if args.timeout == DEFAULT_TIMEOUT:
            args.timeout = 150
        if args.expect == EXPECTED_STAGE:
            args.expect = DEEP_EXPECTED_STAGE

    if not os.path.exists(BINARY):
        print(f'smoke: no {BINARY} -- run `make -f Makefile.pc` first')
        return 1

    # src/pc/os_pi.c falls back to ./baserom.us.z64 when KIRBY_ROM is unset, so
    # setting it is only meaningful for a ROM somewhere else. Setting it to a
    # path that does not exist would be worse than leaving it alone -- it
    # would override the working default with a broken one.
    rom = args.rom or DEFAULT_ROM
    env = dict(os.environ)
    if os.path.exists(rom):
        # Left as given (the child runs with cwd = REPO), so nothing this
        # script produces can carry a path off the machine it ran on.
        env['KIRBY_ROM'] = rom
    else:
        print(f'smoke: WARNING: no {rom} -- the run has no cartridge to DMA '
              'from and will stop early.')
    env['KIRBY_PC_TIMESCALE'] = str(args.timescale)
    if args.input:
        env['KIRBY_PC_INPUT'] = args.input
    else:
        # Inherited from the caller's shell it would silently change what the
        # default smoke test measures.
        env.pop('KIRBY_PC_INPUT', None)
    if args.verbose:
        env['KIRBY_PC_PROGRESS'] = '1'

    print(f'smoke: {BINARY}  timescale={args.timescale}x  timeout={args.timeout}s'
          + (f'  input={args.input}' if args.input else ''))
    try:
        proc = subprocess.Popen([BINARY], env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
    except OSError as exc:
        print(f'smoke: could not run {BINARY}: {exc}')
        return 1

    # TIMEOUT IS SIGTERM, NOT SIGKILL, and the difference is the whole result.
    # A run that reaches the attract loop never ends on its own -- with no
    # controller the game cycles it forever -- so the timeout is the NORMAL
    # ending of a healthy run, not a failure mode. The port traps SIGTERM
    # (src/pc/os_time.c) and pc_pump_events prints the verdict on the way out,
    # so a terminated run still reports how far it got. SIGKILL is only the
    # fallback for a process too wedged to notice.
    try:
        output, _ = proc.communicate(timeout=args.timeout)
    except subprocess.TimeoutExpired:
        proc.terminate()
        try:
            output, _ = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            output, _ = proc.communicate()
            print('smoke: the binary ignored SIGTERM and had to be killed -- '
                  'it is wedged somewhere that never reaches pc_pump_events()')
    output = output or ''

    if args.verbose:
        print(output.rstrip())
        print('---')

    v = parse_verdict(output)
    if v is None:
        print('smoke: the run produced no [verdict] line.')
        print('       That means it died before src/pc/pc_progress.c could '
              'report -- a link problem, or a crash inside osInitialize.')
        tail = [l for l in output.strip().split('\n') if l][-15:]
        for l in tail:
            print('       | ' + l)
        return 1

    reached = v.get('stage', 'unknown')
    try:
        got_i = STAGES.index(reached)
        want_i = STAGES.index(args.expect)
    except ValueError:
        got_i, want_i = -1, 0

    print(f'smoke: reached  {reached}  at {v.get("stage_at", "?")}s '
          f'(gGameState route {v.get("route", "-")})')
    print(f'smoke: outcome  {v.get("outcome", "?")}'
          + (f' ({v["detail"]})' if v.get('detail') else '')
          + f', after {v.get("elapsed", "?")}s')

    # Only worth saying when one of them never happened -- that is a stalled
    # VI or a game that never submitted a display list, and it is a different
    # failure from "the game logic stopped".
    def when(k):
        try:
            return float(v.get(k, '0'))
        except ValueError:
            return 0.0
    if when('first_retrace') == 0.0:
        print('smoke: NO VI RETRACE EVER FIRED -- the pump is not running; '
              'see pc_vi_tick in src/pc/os_vi.c')
    # WHAT THE STAGE NUMBER DOES NOT SAY. Every line above this one is game
    # LOGIC: milestones, gGameState, gtlDrawnFrameCounter. All of them count
    # up exactly the same on a build linked against src/pc/pc_backend_null.c,
    # which draws nothing at all -- and a day of such runs was once reported
    # as rendering progress because the output never mentioned the
    # difference. render= is that missing fact, straight from pcb_gfx_stats.
    render = v.get('render')
    if render is None:
        print('smoke: this binary predates render= in the verdict line; it '
              'cannot say whether anything was drawn. Rebuild it.')
    elif render == 'none':
        print('smoke: RENDERED NOTHING -- linked against a backend that does '
              'not rasterise (pc_backend_null.c). Everything above is game '
              'logic only; this run is NOT evidence about rendering.')
    else:
        drawn = int(v.get('drawn', '0') or 0)
        sampled = int(v.get('sampled', '0') or 0)
        nonblank = int(v.get('nonblank', '0') or 0)
        distinct = int(v.get('distinct', '0') or 0)
        if drawn == 0:
            print('smoke: RASTERISING BACKEND DREW 0 FRAMES -- the display '
                  'lists never reached Fast3D. A rendering failure, not a '
                  'game-logic one.')
        elif sampled == 0:
            print(f'smoke: {drawn} frame(s) rasterised and presented. Set '
                  'KIRBY_PC_FRAMEHASH=1 to also check they were not blank.')
        else:
            print(f'smoke: {drawn} frame(s) rasterised, {sampled} sampled, '
                  f'{nonblank} non-blank, {distinct} distinct '
                  f'(hash {v.get("framehash", "?")})')
            if nonblank == 0:
                print('smoke: EVERY SAMPLED FRAME WAS BLANK -- frames are '
                      'being presented but nothing is drawn into them.')
            elif distinct <= 1:
                print('smoke: THE IMAGE NEVER CHANGED -- the same frame is '
                      'being presented over and over. The game is drawing '
                      'but not advancing.')

    if when('first_gfxtask') == 0.0:
        print('smoke: NO GRAPHICS TASK WAS EVER SUBMITTED -- the game never '
              'reached osSpTaskStartGo')

    if v.get('outcome') == 'crash':
        frames = resolve_backtrace(output, BINARY)
        if frames:
            print('smoke: died at')
            for f in frames[:12]:
                print('       ' + f)

    if got_i < want_i:
        print(f'smoke: FAIL -- expected to reach {args.expect}, stopped at {reached}')
        return 1
    if got_i > want_i:
        print(f'smoke: PASS, and further than expected ({args.expect}). '
              'Raise EXPECTED_STAGE in tools/pc/smoke.py.')
        return 0
    print(f'smoke: PASS -- reached {args.expect}, which is the recorded '
          'high-water mark')
    return 0


if __name__ == '__main__':
    sys.exit(main())
