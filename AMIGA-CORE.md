# The Amiga 500 core (branch: amiga)

This fork adds a Commodore Amiga 500 to retro-go. The emulated machine is
[rusty-nail](https://github.com/gotnull/rusty-nail)'s `fcamiga` (Musashi 68000,
Agnus, Copper, Blitter, Denise, Paula, CIAs), reached through a small C API.
**This file is the working context: read it before touching the core.**

## State as of 28th July 2026

Working, verified on the console:

- The `Amiga 500` tab appears in the menu with a Kickstart entry (cover art
  known missing - open item).
- Launching Kickstart boots the real machine to the insert-disk screen,
  rendered via an L8 LTDC layer + CLUT at 280MHz.
- Input: d-pad = joystick AND mouse (both ports live, as real hardware),
  A = fire, GAME/TIME = left/right mouse buttons, PAUSE = exit (reset).
- Audio is wired end to end (Paula -> downmix -> SAI DMA halves) but nothing
  on stock flash makes a sound: the insert screen is silent on a real Amiga
  too, and no .adf fits the 1MB chip. First audible test needs the 64MB chip.

Awaiting confirmation at handoff: the flicker fix (blit moved to just after
vblank) and a formal PAUSE-exits-cleanly pass (slice 4).

## Architecture in five lines

- The machine is a Rust staticlib: `~/development/gandw/firmware/fcamiga-gw`
  (path in `Makefile.common` as `AMIGA_CRATE`). It builds the WORKING TREE of
  `~/development/rusty-nail/fcamiga` - uncommitted changes next door are
  picked up, which is how the emulator gets developed.
- Its code executes in place from EXTERNAL flash like the SNES ports
  (`*libfcamiga_gw.a:` matcher in the ld). Measured XIP cost: ~6%.
- Chip RAM (512K) sits at 0x24000000 OVER `.lcd1/.lcd2` and the overlay
  arena; jump table, capture and L8 framebuffer fill AXI to 1,041,728 of
  1,048,576. Legal because the core drives its own LTDC layer and exit is
  `NVIC_SystemReset`.
- The staticlib's ~43K of statics spill to AHB SRAM (0x30000000); the core
  enables that bank's clocks, scrubs it with 64-bit stores, and copies its
  own `.data` - retro-go's startup knows none of it.
- Licence: built with `musashi410` (MIT) NOT the default `musashi332`
  (non-commercial) - retro-go is GPL v2. The Makefile deletes the archive's
  embedded newlib/compiler_builtins members; see "Five traps" below.

## Building

```sh
export GCC_PATH=~/development/toolchains/arm-gnu-toolchain-14.2.rel1-darwin-arm64-arm-none-eabi/bin
make GNW_TARGET=mario COVERFLOW=1 BIG_BANK=1 PYTHON3=/tmp/z3venv/bin/python -j8
```

`PYTHON3` must point at a Pillow-equipped interpreter (`python3 -m venv` +
`pip install pillow` if /tmp/z3venv is gone). Without it, cover art fails to
pack SILENTLY - parse_roms catches the conversion error with a bare `pass`,
and the menu shows blank covers with no message anywhere.

ROMs: `roms/amiga/` takes a PRE-SWAPPED Kickstart as `.rom`
(`tools/kickstart_swap.py` does the swap - the core executes it in place and
cannot swap at runtime) plus `.adf` disks (never compressed; read in place).
The Kickstart entry boots bare; an .adf entry boots with that disk inserted.

## Flashing

```sh
./tools/debug/flash-all.sh
```

Erase-first via OpenOCD, then BOTH images via gnwmanager through the gandw
wrappers. Every deviation from that recipe has a failure mode documented in
`~/development/gandw/RETRO-GO.md`.

## Debugging

```sh
./tools/debug/fault-scan.py
```

One command after any BSOD/black screen: fault registers, exception frames
with symbols, and a return-address census of the stack (ONE giant call-free
gap = a huge stack local; that exact signature found a 19K temporary in
fcamiga's `Chipset::reset`).

Two traps the tooling cannot see for you:

- **SWD bypasses the RCC clock gates and the MPU.** RAM that reads fine over
  the probe can still bus-fault the CPU. If SWD and the CPU disagree, believe
  the CPU.
- **A framebuffer screenshot is not the panel.** The capture path halts the
  CPU and reads memory; retro-go's LTDC interrupt machinery can be scanning a
  different buffer entirely. The machine ran perfectly for several rounds of
  "white screen" while its picture was stomped one vblank after every blit.

## Five traps this integration hit (full stories in `git log`, commits 7212f2b..HEAD)

1. **An `AT> FLASH` section placed before `.isr_vector` evicts the vector
   table** from 0x08000000. Boot becomes a fault loop with BFAR 0xFFFFFFFC.
2. **ld gives input sections to the FIRST matching pattern.** Spill sections
   defined later receive nothing unless the main `.data/.bss` use
   `EXCLUDE_FILE (*libfcamiga_gw.a)`.
3. **A Rust staticlib embeds newlib and compiler-builtins.** Any symbol they
   shadow (snprintf, memmove, even `__popcountsi2` from
   `__builtin_popcount`) lands in extflash and gets executed before OSPI
   memory-mapping exists. `compiler_builtins` compiles as rcgu objects, so
   "keep rcgu" is not a whitelist. The Makefile deletes the shadowing crates
   by name.
4. **AHB SRAM: clocks off since reset** (SystemInit only enables under
   `DATA_IN_D2_SRAM`) **and MPU region 0 was Strongly-Ordered**, where the
   architecture faults LDREX/STREX - fcamiga's first `AtomicBool::swap` died
   identically on every launch after a full 128K scrub had passed. Region 0
   is now TEX=1 Normal non-cacheable (`Core/Src/main.c`).
5. **retro-go's LTDC line interrupt reprograms the layer every frame.** The
   core disables `LTDC_IRQn`/`LTDC_ER_IRQn` on entry or its L8 config
   survives less than one vblank.

Also: launcher stack is 30K (was 20K; the padding that funded it was
measured first), and fcamiga's `Chipset::reset` no longer builds a ~19K stack
temporary - fixed upstream in rusty-nail (`fcamiga/src/chipset.rs`, const
flash template).

## Open items, in order

1. **Confirm the flicker fix** (chunked D-cache clean chasing the blit).
   PAUSE-exit is confirmed working; cover art and the About credit
   ("gotnull", per the fork chain's convention) are in as of 29th July.
2. **Commit-and-push hygiene**: rusty-nail has two local commits (cycle-table
   trim, chipset reset) that only the owner pushes.
4. **Publishability**: the fork references the fcamiga-gw crate by absolute
   home path. Before a public PR it needs vendoring or a submodule, and a
   NOTICE for the MIT Musashi.
5. **64MB flash chip** (MX25U51245GZ4I-00G, ordered): then .adf disks, the
   first audible Paula, cover art for games, and `adf-fits` stops mattering.
6. **Overclock interplay**: the menu's CPU overclock (312/353MHz) changes the
   frame budget arithmetic; the Amiga core paces on vblank so it should
   simply gain headroom - verify, then document.
7. **Save states**: fcamiga has none yet (Chip RAM + chipset + CPU ~600K).
   rusty-nail's ledger tracks it.

## Cross-repo map

| Repo | Role |
|---|---|
| this fork, branch `amiga` | the product: retro-go + Amiga core |
| `~/development/gandw` | SDK, probe tooling, measurements, AMIGA-PORT.md / RETRO-GO-CORE.md / CLOCK-SHEAR.md post-mortems |
| `~/development/rusty-nail` | fcamiga itself; fixes to the MACHINE go there, fixes to the HOST go here |

Rule of thumb from the placement investigation: **before calling a rendering
artefact an emulator bug, boot the same ROM in FS-UAE** (installed; recipe in
gandw's memory notes and AMIGA-PORT.md). One reference boot beats a day of
code audit - the "mirrored label" was authentic 1988 artwork.
