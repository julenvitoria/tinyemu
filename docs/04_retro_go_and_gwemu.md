# Working with Retro-Go and gwemu

Based on past project experience (specifically from `game-and-watch-retro-go-sd`), working with Retro-Go and `gwemu` introduces extremely strict constraints. Failure to adhere to these exact points has cost real days of development time.

## 1. Build System Traps (Read Before Compiling)
- **Make Flags Are NOT Dependencies**: Changing build flags recompiles **nothing**. If you change a flag, you MUST run `make clean`, otherwise you will boot stale objects and test the wrong thing. Never do a manual `rm -rf build`.
- **SD Card Image Staleness**: `gwemu_release` does update the intflash image, but the SD card is meant to imitate real life and doesn't automatically get wiped. If you need to update its contents for testing, simply delete `build/sdcard.img` and starting new is perfectly valid.

## 2. gwemu is NOT for Performance, SD Timing, or Peripheral Setup
- **Peripherals are not initialised, they are assumed.** `gwemu` presents external flash at `0x90000000` as plain readable memory. On silicon that address returns nothing until the OCTOSPI is configured for memory-mapped access. Firmware that never configures it passes every gwemu run and bus-faults immediately on hardware — this is exactly how `test_core.elf` failed on 2026-08-12 (`BFAR=0x90000000`, `PRECISERR`, stacked `PC=memcpy+0x10`, `LR=disk_read+0x2b`, from an unconditional `vdisk_init((void*)0x90000000)` at reset). Generalise the rule: **anything that depends on a peripheral being set up is unproven under gwemu.**
- **Always give bare-metal firmware real fault handlers.** With a two-entry vector table (SP + Reset) a fault escalates to HardFault with nowhere to go, the device wedges, and the Python harness sees an indistinguishable timeout. Latch `CFSR`/`HFSR`/`BFAR` and the stacked PC so the host can read *why*.
- **Never let new tooling's first execution be a hardware run.** A failure then has two candidate causes — your script or the target — and the tier hierarchy exists precisely to keep those separate. Validate at the lowest tier that exercises the same code path first.

- **Timing**: `gwemu` cannot produce timing numbers. Its CPU/blit ratio is wildly inverted.
- **Measured 2026-08-12, CPU-only workload** (1,000,001 x86 instructions, register-only `DEC`/`JNZ` loop, no memory operands, no devices):

  | | gwemu | hardware |
  |---|---|---|
  | cyc/insn | 6.47 – 7.40 | **57.500** |
  | run-to-run spread | **~14%** | **8 cycles in 57.5M** |
  | instruction count | 1000001 | 1000001 |

  Two corrections to what this document used to say. First, the ratio here is **~8.5x, not ~400x** — the 400x figure presumably came from a full-system run where the blit/SD inversion compounds, and should not be quoted for CPU work. Second, and more useful: **gwemu's cycle counts are not merely inaccurate, they are not reproducible.** A 14% spread on identical firmware means gwemu cannot resolve any optimization smaller than about a sixth of total runtime, whatever the scaling factor is.

  The flip side is the genuinely valuable finding: **hardware is deterministic to about one part in 10^7**, so silicon can resolve sub-0.001% changes. For assembly micro-optimization the device is not just the authoritative instrument, it is the *precise* one. Instruction counts are exact on both and are what transfers.
- **SD Card Busy State**: `gwemu` **DOES NOT MODEL SD CARD BUSY STATE**. Under emulation, the SD card is always ready instantly. Any code testing the `SD_ReadyWait()` handshake or FastSPI polling is DEAD CODE in `gwemu`. If you are working on SD I/O, `gwemu` only proves plumbing; you must run on silicon to prove the handshake.

## 3. Headless & Shared Build Directories
- **Never run concurrent builds** or tests out of the same `build/` directory. `gwemu` hardcodes paths to `build/qemu_bank1.bin`, `build/extflash.bin`, etc. Concurrent runs will corrupt each other. Serialise builds and copy artifacts to an isolated `/tmp` directory before launching `gwemu`.
- **Agent GUI Usage**: When coordinating with the user, ALWAYS run `gwemu` in GUI mode so they can verify it visually. If working independently, run it headlessly (preferring Docker).

## 4. Hardware Debugging & GDB
- **No Inferior Calls**: **NEVER** make a GDB inferior call (`call foo()`) on this target. It re-enters, GDB abandons it, and the device is left halted with a corrupted heap/stack frame.
- **Breakpoints**: Use `hbreak` (hardware breakpoints), not `break`, for flash addresses (`0x08xxxxxx`). A software breakpoint silently fails to write to flash.
- **Device Log Ring Buffer**: The hardware device log is only 4KB and wraps very quickly. Do NOT use `gnwmanager monitor` terminal commands. Instead, strictly use the Python bindings (e.g. `scripts/gwtarget.py`) to read the log buffer out of memory, as per the global rules.

## 4b. Reading a fault once retro-go has BSOD'd
Retro-go installs its own fault handlers: `HardFault_Handler` tail-calls
`common_fault_handler_c`, which calls `BSOD(type, pc, lr)`. `BSOD()` **paints
the faulting PC and LR on the panel** and then spins in `buttons_get()` waiting
for a keypress. Two consequences:

- A sampled PC of `HAL_GPIO_ReadPin` / `buttons_get` after a crash means "sitting
  in the BSOD", not "polling input".
- `probe.py --faults` still gives you a true `CFSR`/`HFSR`/`BFAR` (they latch),
  but its **stacked-frame decode is meaningless here**: it derives MSP-vs-PSP
  from the *current* `LR`, and by then `LR` is an ordinary function return
  inside the BSOD loop, not an `EXC_RETURN`. The R0-R3/PC/xPSR it prints are
  whatever happens to be at that address.
- The reliable read is the screen: `scripts/screenshot.py`, then crop/upscale
  the top two lines (`FATAL EXCEPTION: ...` and `PC=0x… LR=0x…`) and resolve
  them with `arm-none-eabi-addr2line -f -i -e .../build/gw_retro_go.elf`.
  Note the LR may skip a frame: a tail-called leaf keeps its caller's LR.

## 5. Input Replay (`gwharness.py`)
- `scripts/gwharness.py` allows interleaving button inputs via shadow-cell writes with the log poll, without evicting the OpenOCD connection. This enables true deterministic timeline replay on real hardware, which can then be directly diff'd against a `gwemu` log.

## 6. Automated Testing Infrastructure
When testing with `gwemu`, do **not** drive GDB manually or script it via shell wrapper subprocesses (e.g. `arm-none-eabi-gdb` batch modes). This approach is highly prone to timing out and hanging on quit confirmations, and requires fragile output parsing.

Instead, use `gnwmanager`'s Python bindings to directly attach to the QEMU target over its exposed GDB port. This approach leverages the same reliable backend used to interface with real silicon via OpenOCD. 

**Scripting Rules:**
1. **Never use raw GDB subprocesses.** Use `gnwmanager.ocdbackend.gdb_backend.GDBBackend` and `gnwmanager.GnW` for inspecting state and pushing button events dynamically (as seen in `remote_input.py`).
2. **Be Re-usable and Extensible.** Any Python test script must not be a "dumb script". It must always use `argparse` so it can be parameterized by developers (e.g., exposing `--clean` to allow omitting `make clean` for speed, or providing target firmware paths).
3. **Control Flow.** Launch QEMU paused (`-S`), open the GDBBackend to attach to `localhost:1234`, hit `backend.resume()`, wait for processing, hit `backend.halt()`, and then use `backend.read_uint32()` directly to inspect memory spaces cleanly. Find addresses programmatically (e.g., via `arm-none-eabi-nm`) rather than hardcoding.
4. **NEVER run QEMU manually via bash.** Always use the established project test runners (e.g., `python3 tests/test_benchmark.py` or `python3 run_tests.py`). Running QEMU directly via CLI command creates disconnected state, breaks the intended validation flow, and is strictly prohibited in this project.

## 7. The Pause overlay over a 256-colour mode (Queue B6)

`build/artifacts/hw_overlay.png` is the device capture: Wolfenstein 3-D behind
the Pause menu is **orange and magenta instead of dimmed**, and the menu itself
is a pale-yellow box whose text is barely readable. Both come from one cause and
neither is a bug in our renderer.

### The mechanism, in the order it happens

`odroid_overlay.c:929-950` — the overlay's own `_repaint()`:

1. `lcd_clear_active_buffer()`
2. `repaint()` — **our** `core_repaint()` (`src/main.c`), which renders the
   guest frame into the ACTIVE buffer
3. `odroid_overlay_darken_all()` (`odroid_overlay.c:563`) — in LUT8 that is
   literally `for (i) fb[i] |= LCD_DARKEN_BIT;`, `LCD_DARKEN_BIT == 0x20`
4. `odroid_overlay_draw_dialog()` — the box and text, drawn AFTER the darken and
   therefore never darkened themselves

The `|= 0x20` only dims because `lcd_set_clut(clut, count)` programs a 40%
darker twin of entry `i` at `count + i` (`sdk/include/Core/Inc/gw_lcd.h:188-194`)
— which is only true when `count == 32`. A 256-colour mode owns the whole table
(`GW_LCD_SET_CLUT_EXT`, `src/main.c:2702-2712`) because 256 entries would need
512 slots and the LTDC has 256, so `i | 0x20` picks an unrelated **guest**
colour. That is the orange. The pale yellow is the other half: the menu's
colours go through `lcd_pack_color()`, which nearest-matches into whatever
palette is live — ours — so the theme's colours become the closest thing
Wolf3D's palette has.

### Why the obvious fix is still forbidden

A bit-5-invariant palette (`CLUT[i | 0x20] == dim(CLUT[i])` for all `i`) forces
`CLUT[i] == dim(CLUT[i])` for every `i` that already has bit 5 set, so it
collapses to 128 usable entries and mangles half the colours. gnw-doom shipped
it; `docs/08` says **do not repeat it**, and that verdict stands **for the
gameplay palette**.

### What actually works, with NO firmware change

The collapse is only unacceptable *during gameplay*. Scoped to the paused
overlay it is free, and step 3 above hands us the lever: after `darken_all()`
every displayed index has bit 5 set, so only the 128 indices `0x20..0x3F,
0x60..0x7F, 0xA0..0xBF, 0xE0..0xFF` are ever scanned out while the menu is up.
We own both the palette and the pixels, so:

1. `core_repaint()` is called ONLY from the overlay — it is the "menu is open"
   signal, and no new firmware hook is needed to detect it.
2. On the first such call, build a 128-colour reduction of the live guest
   palette (histogram the frame, keep the most frequent 128, nearest-match the
   rest into a 256-byte `guest index -> reduced index` map). Program each
   bit-5-set slot with the **dimmed** reduced colour, and reserve four of them
   for the overlay theme's exact RGB so `lcd_pack_color()` finds an exact match
   instead of Wolf3D's nearest yellow. Push it with `GW_LCD_SET_CLUT_EXT`.
3. Render the frame through the map, ORing `0x20` — `darken_all()`'s OR is then
   idempotent and the picture is dimmed EXACTLY, by the CLUT, at scanout.
4. On the first non-overlay frame, re-push the guest's own 256-entry palette.
   `src/main.c`'s `clut256_pushed` pointer compare already does this the moment
   the array it points at changes, so "push the overlay palette" and "push the
   real one back" are the same code path with a different array.

Price: one 76,800-pixel histogram plus a 256-entry nearest-match, both **while
the guest is paused**, and one extra `OR` per pixel in the repaint path that
only the overlay uses. ~1 KB of .bss for the second palette and the map. No
firmware change, no ABI change, nothing that can affect a frame during play.

Cost to the picture: the paused backdrop is quantised to 128 colours and four
of those are spent on menu colours. It is a dimmed, deliberately unimportant
background; the alternative in the capture is that it is the wrong colour
entirely.

### The firmware patch, if the firmware is ever ours to change

The one-line version fixes the same thing more cleanly, and would let the core
delete all of the above:

```c
/* odroid_overlay.c, odroid_overlay_darken_all(), LUT8 branch */
if (lcd_get_mode() == LCD_MODE_LUT8) {
    if (lcd_clut_is_ext())          /* new predicate: a caller-owned 256-entry
                                       palette is live, so there are no twins */
        return;                     /* the core dims its own palette instead */
    ...existing OR loop...
}
```

paired with an ABI op that asks the firmware to program a dimmed copy of the
live ext CLUT (`GW_LCD_SET_CLUT_EXT_DIM`, or a percent argument on the existing
op). The core then dims by pushing a palette and never touches a pixel. This is
a recommendation, not something we land: `Makefile:13-17` — **nothing may link
against `references/`**, and the firmware is not built from this repo.
