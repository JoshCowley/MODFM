# FM1 Rack: Felucca 1.7.0 with a BRAIDS engine

A fork of [Felucca](https://github.com/hugelton/Felucca) 1.7.0 (GPL-3.0-only). It adds one sound engine, **BRAIDS**
(engine 14). BRAIDS is Mutable Instruments' Braids macro oscillator, with a modulator per part (Peaks' envelope or
LFO, a Tides-style slope, or Grids' drum map). Felucca's ADSR is the VCA. Everything else is Felucca: the sequencer,
FX, matrix, UI, storage and USB.

**Status: host-tested, not yet built for or run on an FM-1.** The JieLi toolchain could not be downloaded where this
was written, so the first device build happens on your Mac. Everything under "Checked" ran on a Linux PC.

## What BRAIDS does

| Page | Knobs |
| --- | --- |
| EDIT 1 (OSC) | **MODL**: 43 of Braids' 47 models (CSAW .. QPSK; the four wavetable models are left out). **TIMB** / **COLR**: Braids' two parameters. **DPTH**: the modulator into TIMBRE (bipolar). |
| EDIT 2 (MOD) | **MOD**: OFF, PENV, PLFO, TIDE, LOOP, GBD, GSD, GHH. The next three knobs follow MOD: PENV: ATK DEC SUS. PLFO: RATE SHAP. TIDE / LOOP: TIME or RATE, SHAP, SLOP. GBD / GSD / GHH: X Y FILL. |
| HOME | MODL TIMB COLR DPTH |

- **PENV** is Peaks' envelope (multistage_envelope.cc). It is gated while a key of the part is held.
- **PLFO** is Peaks' LFO (lfo.cc). It runs free.
- **TIDE** is an AD slope that starts with each note. **LOOP** is the same slope looping (0.05 to 100 Hz). Neither is
  Mutable's code: Tides v1 needs about 55 KB of tables, more than the flash has. They are small integer C in
  `eng_braids.c`, modelled on Tides' AD and looping modes.
- **GBD / GSD / GHH** are Grids' drum map, one lane each, stepped by the transport's 1/16s. They are silent while
  stopped. A hit is a trigger envelope and also strikes the part's sounding oscillators, so PLUCK, BELL, DRUM, KICK,
  CYMB and SNARE ring on the pattern. The GRID DRUM preset holds the VCA open, so Grids plays the drum.
- The track's own modulation still works: FLT (LFO DEST, ENV DEST, the matrix's CUT) moves TIMBRE, and SHP moves
  COLOR. MIDI CC74 sets TIMB and CC71 sets COLR.
- There are 8 presets: CSAW, PLUCK, BELLS, VOWEL, FM BASS, SWARM, GRID DRUM, CLOUD. Their categories are set, and
  they can be starred.

**Limits**

- There are **4 Braids oscillators for all parts** (17 KB of RAM each). One part plays up to 4 voices. When two
  parts both play BRAIDS, they share the 4, and the oldest note gives way (it goes silent until its key is released).
- To make room for the oscillators, the **FX delay line is half as long** (0.74 s, was 1.49 s). Long delay times at
  slow tempos clamp sooner.
- **USB audio runs at 44.1 kHz only** (`FELUCCA_UAC_48K=0`). Its 48 kHz filter's 8 KB of flash went to Braids.
- The web editor shows only 24 enum names, so BRAIDS models past the 24th appear as numbers there.
- Braids was designed at 96 kHz and is retuned for the FM-1's 44.1 kHz (A4 = 440.2 Hz measured). Peaks was designed
  at 48 kHz, so its times run 8.8 % long.
- On screen the engine keeps the Mutable module names. The eurorack guidelines ask derivative works not to use them,
  so rename them before anything public. Keep the repository private until then.

## Build on the Mac

1. Docker Desktop, plus Felucca's toolchain and SDK, as in `BUILDING.md`:
   `tools/get_toolchain.sh` and the AC79 SDK clone at `AC79NN_SDK_V1.2.1_2023-12-13`.
2. Submodules, including the Mutable Instruments code:
   `git submodule update --init --recursive` (this fills `firmware/engines`, `firmware/bsp` and `ext/eurorack` with
   `stmlib`).
3. `./build.sh`. BRAIDS is on by default in this fork. Read the `ok` lines at the end:
   - `flash: N B of the app slot free`
   - `image ... RAM .data+.bss ... pool ...`
4. If the build stops with **image exceeds the app slot**, the message names the levers:
   - `FELUCCA_ICONS=0 ./build.sh` drops the parameter icons.
   - Or silence more Braids models: add them to `tools/mi_build.py` `SILENCED` and take them out of
     `eng_braids.c` `BRD_SHAPE`.
5. If the old compiler (clang 4.0.1) rejects the C++, the error is in `build/mi/`. The patched copies of the Braids
   and Peaks sources are there too.
6. `FELUCCA_BRAIDS=0 ./build.sh` builds Felucca 1.7.0 as it was.

**Expected budgets.** These are estimates from a RISC-V compile scaled to Felucca's real 1.7.0 image. Check them
against the build's own lines.

- **Flash:** stock 1.7.0 is 513,332 B of a 581,564 B slot, leaving 68 KB free. Turning USB 48 kHz off frees about
  8 KB more. BRAIDS needs about 50 to 60 KB (Braids code about 20 to 27 KB, tables 31 KB, the engine about 5 KB).
- **Pool RAM:** about 42 KB free in stock 1.7.0. The halved delay line frees 64 KB, and the oscillators take 69 KB.
  That leaves about 37 KB, and the build insists on at least 8 KB.

## Install and recover

Install the `build/felucca.fwsc` package with Felucca's web installer, as for any Felucca build. The version line
reads **v1.7.0-rack1**. Keep FM-1-transporter or the installer's "Return to official V15" ready before the first
flash, and the BSP's `fm1_rescue` for a unit that will not start.

## First things to check on the device

1. It boots, and ABOUT shows v1.7.0-rack1.
2. BRAIDS CSAW, A4 is in tune with a tuner (440 Hz).
3. Scroll every model at 4 held notes and watch the audio load. HARM, CYMB and BELL are the heaviest (on a PC, HARM
   costs about three ANALOG voices per voice). Felucca's overload fades should hold. Note the load.
4. Try 2ND CORE ON with two BRAIDS parts. The host tests show it is bit for bit the same as one core.
5. GRID DRUM with the transport running: the hits follow the 1/16s and stop when stopped.

## Checked (host, Linux)

- `tests/braids_test.c`:
  - A4 = 440.2 Hz on CSAW, MORPH, FOLD, SAW3 and COMB, and A2 and A5 on CSAW, all within 0.05 %.
  - All 43 models render with no silence, no clipping and DC under 400.
  - The level sits with ANALOG's.
  - PENV holds at SUS and ends. PLFO and LOOP swing both ways. TIDE runs once per note. GRID hits only while
    playing, fewer at less FILL, none at FILL 0.
  - The 4-voice cap holds, the oscillators are shared with no slot faults, and every preset plays and ends.
- Felucca's regression with BRAIDS built in: all 98 of Felucca's golden renders are unchanged, BRAIDS's 8 renders
  pass the health checks, and the voice and routing checks pass.
- Felucca's regression with BRAIDS off: 98 of 98 unchanged.
- Second core: the dual-core test is bit for bit on one core and on two, every split, with BRAIDS (30 scenes) and
  without (28). It first failed with BRAIDS. Braids and Peaks shared one random-number generator across cores, so
  each oscillator and LFO now gets its own.
- UI: ui_test, editor_test and project_test pass with BRAIDS. ui_render, every screen in 10 palettes, shows 0 lint
  findings and 0 alignment misses. With BRAIDS off, everything passes as before.
- `tests/run_tests.sh` ran up to its SDK step (the SDK could not be fetched here). The BRAIDS sections are in it.

## Not checked

The device build itself, which means four things:

- the flash and RAM figures (estimates above)
- whether clang 4.0.1 accepts the C++ (it was written for that: no exceptions, RTTI, static constructors or C
  library, and the build checks for constructor tables and stray sections)
- the CPU load on the AC79
- the sound on hardware

## How it is put together

| File | What |
| --- | --- |
| `firmware/src/eng_braids.c` | The engine: models, the modulators, oscillator slots, presets |
| `firmware/src/grids_map.h` | Grids' 25 drum-map nodes and their 5 x 5 map (GPL-3.0) |
| `firmware/mi/mi_glue.cc`, `mi_glue.h` | Braids and Peaks behind a C interface; objects in the pool, no constructors |
| `firmware/mi/inc/` | The few C and C++ headers the Mutable code includes (the FM-1 has no C library) |
| `tools/mi_build.py` | Compiles the Mutable objects. At build time it gives each oscillator and LFO its own random numbers, silences the wavetable models, and cuts `resources.cc` to the tables the code uses (`tools/mi_trim.py`) |
| `tools/build_mi_host.sh`, `tests/braids_test.c` | The host build and test |
| `ext/eurorack` | pichenettes/eurorack at f4cfa76, stmlib at e3bd7c9 (git submodule) |

Felucca's own files changed:

- `core.h` (FELUCCA_BRAIDS, NENGINES)
- `engines.c` (the table, the order)
- `fx.c` (the delay length, the second core's cost)
- `seq.c` (the 1/16 count for Grids)
- `favorites.c` and `midi_learn.c` (BRAIDS's stars)
- `category.c`, `midi_control.c`, `ui_graph.c`, `icons.c`, `ui_menu.c` (CREDITS)
- `tools/build.py`, `tests/run_tests.sh`, `tests/ui_test.c`
- `LICENSING.md`, `LICENSES/MIT-Braids-Peaks.txt`
