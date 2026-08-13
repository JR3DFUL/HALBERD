# Draft PR description — kirby64_decomp → Kirby64Ret/kirby64

*(Draft only. Not filed. Assumes the `upstream-pr` branch split — PORT
guards stripped — has been executed first.)*

---

**Title:** 1,145 newly matched functions, per-function splits, and a
verification toolchain

## Summary

This PR brings the decompilation from 2,042 remaining assembly functions
down to 897 — 1,145 functions newly matched — together with the structural
and tooling changes that made that pace possible. Every commit in the
series builds a byte-exact US ROM.

## What's in it

### Matched functions
1,145 functions across every overlay converted from `GLOBAL_ASM` pragmas to
matching C, including large subsystems in ovl1 (object/sprite/bank
management), ovl5, ovl6, and the main segment. All matches are verified
byte-exact at the linked-ROM level, not just per-object.

### Per-function assembly splits
The monolithic per-segment `.s` files are re-split into one listing per
function under `asm/nonmatchings/` (splat-generated, plus hand-carried
listings under `asm_manual/` for padding-sensitive cases). This is the
layout most active decomp projects use: per-function diffs, per-function
progress tracking, no merge conflicts between people working in the same
segment.

### Verification toolchain (`tools/decomp/`)
- `mk.sh` — serialized, source-hashing build gate
- `verify.py` + `check_tu_size.py` / `check_sections.py` /
  `check_rodata_bytes.py` / `check_layout.py` — the per-TU chain that
  catches the failure classes a plain instruction diff misses (translation
  unit shrinkage from padding traps, rodata literal drift, section layout
  movement)
- `verify_rom.py` / `rom_diff.py` — linked-ROM arbiter
- A decomp-permuter harness (`seed_queue.py`, `factory.py`,
  `permute_queue.py`) that closes instruction-order residues automatically,
  and a clone-family pipeline (`find_clones.py`, `apply_family.py`) that
  ports matched shapes across duplicate functions
- `LEVERS.md` — a measured catalogue of IDO codegen levers (evaluation
  order, register-allocation behaviors, stack layout rules) collected while
  matching; kept because it makes the remaining 897 functions cheaper for
  everyone

### Cleanup
Legacy `asm.old/` and `src.old/` trees retired (the handful of headers
still referenced moved verbatim into `src/`), one-shot migration scripts
removed after their job finished, dead scratch files dropped.

## How to verify

```
./tools/decomp/mk.sh          # full build
sha1sum build/kirby.us.z64    # 6cea2d46b929a3bb347b060a77fccc83526fb855
```

The same hash gates every commit in the branch.

## Notes for review

- This is a large structural diff. If you'd prefer it in stages
  (splits first, then matches, then tooling), I'm happy to break it up —
  the commits are ordered so that's mechanical.
- No port/PC code is included. (A separate PC port project consumes this
  repo as a submodule from its own branch, following the
  ssb-decomp-re/BattleShip convention; nothing of it touches this PR.)
- Naming follows the existing conventions (`func_80xxxxxx`, `D_80xxxxxx`);
  no wholesale renames were done, so the diff against your tree stays
  reviewable.
