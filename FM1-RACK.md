# FM1 Rack: a dedicated rack on Felucca 1.7.0

A fork of [Felucca](https://github.com/hugelton/Felucca) 1.7.0 (GPL-3.0-only). Felucca's own engines are dropped. The
sound comes from Mutable Instruments' module code (Emilie Gillet, MIT), under names of the rack's own:

| On screen | What it is | Code |
| --- | --- | --- |
| **MACRO** (engine 14) | a macro oscillator, 43 models | Braids |
| **MODEL** (engine 15) | a macro oscillator, 21 models | Plaits |
| **XMOD** (an INSERT type) | cross-modulation of the part by a carrier | Warps |
| **GRAIN** (the third FX bus) | a granular processor, where the reverb was | Clouds |
| **ENV**, **LFO** | MOD A / MOD B types | Peaks |
| **SLOPE**, **CYCLE** | MOD A / MOD B types | the rack's own, after Tides |
| **TRIG1**, **TRIG2**, **TRIG3** | MOD A / MOD B types | Grids' drum map |

The firmware is called **FM1 RACK** (splash, ABOUT). The version line reads **v1.7.0-rack2**. The credits name the
modules and their author. Everything else is Felucca's: the sequencer, the matrix, the delay and chorus, the slicer,
the UI, storage and USB.

**Status: built for the FM-1 and host-tested. Not yet run on an FM-1.** Everything under "Checked" ran in a Linux
container with the JieLi toolchain and the host's compilers.

## The rack, page by page

Each part (track) is a rack. Its pages, in EDIT:

| Page | Knobs |
| --- | --- |
| EDIT 1 / EDIT 2 | the engine's eight values (below) |
| **RACK** | **MOD A** type, **MOD B** type, **INSRT** (the INSERT's type: XMOD or one of Felucca's), **MIX** (its dry / wet) |
| **MOD A**, **MOD B** | four values; their labels follow the type |
| VOICE ... | Felucca's, as before |

The two modulators are matrix sources, **MODA** and **MODB**. The matrix (MOD page) routes them to any destination:
PITCH, CUT, SHP, AMP, PAN, the sends, the LFO's rate, VIB, E1..E8 (every engine value), DEPTH, VEL and TIME. MACRO's
DPTH also takes MOD A into TIMBRE directly.

| Type | Values | Output |
| --- | --- | --- |
| ENV | ATK DEC SUS | 0..1, gated while a key of the part is held (release = DEC) |
| LFO | RATE SHAP | -1..1, free-running |
| SLOPE | TIME SHAP SLOP | 0..1, one rise and fall per note (2 ms .. 10 s; log, linear, expo) |
| CYCLE | RATE SHAP SLOP | -1..1, the slope looping (0.05 .. 100 Hz) |
| TRIG1 / TRIG2 / TRIG3 | X Y FILL CHAO | Grids' kick, snare and hat lanes, stepped by the transport's 1/16s. A hit is a trigger envelope and strikes the part's sounding voices. **CHAO** (CHAOS) adds a random amount to each step, as Grids does. Silent while stopped |

### MACRO (Braids)

- EDIT 1 (OSC): **MODL** (43 of Braids' 47 models; the four wavetable models are out), **TIMB**, **COLR**, **DPTH**
  (MOD A into TIMBRE).
- EDIT 2 (TONE): **BITS** and **RATE** (Braids' bit and sample-rate crusher), **SIGN** (its waveshaper), **DRFT** (a
  slow random wander of the pitch).
- 4 oscillators shared by all parts (one part plays up to 4 voices). 9 presets.

### MODEL (Plaits)

- EDIT 1 (MODEL): **ENGN**, **HARM**, **TIMB**, **MRPH**: Plaits' model and its three knobs. The 21 models are VA,
  WSHP, FM, GRAIN, ADDTV, CHORD, SPCH, SWARM, NOISE, PRTCL, STRNG, MODAL, BDRUM, SDRUM, HIHAT, VAVCF, PHDST, 6OP,
  TERRN, SMACH and CHIP.
- EDIT 2 (LPG): **DCAY** (the internal envelope's and the gate's decay), **LPG** (0 = OFF, the part's ADSR is the VCA;
  above 0, Plaits' low-pass gate, pinged by each note, at that colour), **TENV** (the internal envelope into TIMBRE
  and MORPH, bipolar), **AUX** (Plaits' main out .. its aux out).
- Each note is a trigger, so the drums, strings and modal models strike on it. A TRIG hit strikes the sounding voices
  again.
- 2 voices shared by all parts. 9 presets.
- Left out for the flash: the wavetable model (WTBL), and two of 6-OP's three DX7 banks.

### XMOD (Warps), an INSERT type

The part's sound is the modulator. A carrier oscillator plays the part's last note.

- **ALGO**: morphs through XFADE, FOLD, analog ring, digital ring, XOR and COMPARATOR, as Warps' big knob does.
- **TIMB**: the algorithm's parameter.
- **CARR**: the carrier: SIN TRI SAW PLS, or a sine 5, 7 or 12 semitones up, or 12 down (S+5 S+7 S+12 S-12).
- **MIX** (INSERT 2, or the RACK page): dry .. wet. MIX 0 does nothing.

### GRAIN (Clouds), the third FX bus

The tracks' third send, labelled **GRN** on the FX page, feeds it. The return is wet only.

- **GRAIN**: **MODE** (GRAN, STRCH, LOOP, or FRZ = GRAN frozen), **POS**, **SIZE**, **PTCH** (-24 .. +24 semitones).
- **GRAIN 2**: **DENS**, **TEXT**, **FDBK**, **VERB** (Clouds' own reverb).
- Mono, low fidelity (Clouds' 8-bit mode): 1.5 s of buffer.
- Clouds' fourth mode, SPECTRAL, is left out.

## Limits

- **The engines run at 44.1 kHz**, not their modules' rates:
  - MACRO and MODEL are retuned. A4 measures 440.2 Hz on MACRO and 439.9 Hz on MODEL.
  - Plaits' and Peaks' times run about 8.8 % long.
  - Clouds' grains are about 27 % shorter than on the module.
- **Voices**: 4 MACRO oscillators and 2 MODEL voices for all parts. When parts share them, the oldest note gives way.
- **MODEL and XMOD stay on the first core.** The second core takes only the other parts. The reasons: their float
  code, stmlib's one random-number generator, and Plaits' stack (the second core has 2.8 KB).
- **The FX delay line is a quarter of Felucca's** (0.37 s, was 1.49 s). Long delay times at slow tempos clamp sooner.
- **The reverb is gone.** GRAIN is the third bus. A Felucca project's reverb settings load as GRAIN values.
- **USB audio is 44.1 kHz only.**
- **Projects**:
  - A rack project stores 127 values per track and has no FM6 patch block. Felucca refuses it, showing EMPTY.
  - A Felucca project loads here. Its tracks' dropped engines become MACRO (preset 0's values).
- **Not checked on hardware**:
  - the CPU load (below);
  - whether the FPU behaves as the toolchain expects. The FM-1's FPU works on the general registers, so an interrupt
    has no extra state to save. That comes from your fm1-x0x design notes; it was not tested here.

### CPU (host estimates only)

Measured with valgrind on x86, these are instruction counts, not FM-1 cycles:

| What | Instructions per sample |
| --- | --- |
| One MODEL voice | about 190 (SPCH) to 870 (ADDTV); most models 450..650 |
| GRAIN | about 1,300 |

On the FM-1, check the load with the heavy models on two voices, plus GRAIN, plus four MACRO voices. Felucca's
overload fades are still in place.

## Install and recover

1. Install **fm1-rack-1.7.0-rack2.fwsc** with Felucca's web installer, as for any Felucca build.
2. Before the first flash, keep FM-1-transporter or the installer's "Return to official V15" ready, and the BSP's
   `fm1_rescue` for a unit that will not start.

## First things to check on the device

1. It boots. The splash says FM1 Rack, and ABOUT shows v1.7.0-rack2.
2. MACRO CSAW, A4: 440 Hz on a tuner. MODEL FM (TIMB 0), A4: 440 Hz.
3. Each MODEL model on two held notes. Watch the CPU readout (MENU > SYSTEM > INFO) on BDRUM, SDRUM, MODAL, STRNG,
   CHORD and ADDTV. Note the heaviest.
4. GRAIN: send a part (FX page, GRN) and try each MODE; FRZ holds the buffer.
5. XMOD on a MACRO part, MIX 100 %: sweep ALGO.
6. MOD A TRIG1 with the transport running: hits follow the 1/16s and stop when stopped.
7. 2ND CORE ON with MACRO parts and a MODEL part.
8. Save a project, power-cycle, load it: the rack's slots and GRAIN come back.

## Build

`./build.sh` builds the rack. It needs the JieLi toolchain and SDK, as in `BUILDING.md`, and the submodules:
`git submodule update --init --recursive` fills `ext/eurorack` with stmlib. Without Docker:
`JIELI_TOOLCHAIN=path AC79_SDK=path JIELI_DOCKER=0 ./build.sh`.

`FELUCCA_RACK=0 ./build.sh` builds Felucca 1.7.0 as it was.

The build's last lines, as measured for v1.7.0-rack2:

| Budget | Used | Headroom (the build's minimum) |
| --- | --- | --- |
| Flash | 550,724 B | 30,840 B free |
| RAM (.data + .bss) | 53,508 B of 61,440 | 7.9 KB (needs 4 KB) |
| Pool | 368,164 B of 380,928 | 12.5 KB (needs 8 KB) |

If a change overflows the flash, the levers are `FELUCCA_ICONS=0`, or more models silenced in `tools/mi_build.py`
`SILENCED` (with `eng_macro.c` `BRD_SHAPE` or `eng_model.c` `MDL_ENGINE`).

## Checked (Linux container)

- **Device build**: compiles and links with the JieLi clang 4.0.1. The float code is built for the FPU
  (`-mcpu=r3 -mfprev1 -ffp-contract=off`, every constant single precision), with these build checks:
  - no soft-double routine;
  - no static constructor;
  - no undefined symbol;
  - no section the linker script does not place.

  No function of the image mixes 64-bit multiplies with 16-bit loads, apart from Felucca's own `midi_clock_pulse`,
  as in 1.7.0.
- **`tests/rack_test.c`**:
  - maths: `mi_math.cc` against libm, within 8e-8 .. 4e-7;
  - MACRO: tuning on five models and three octaves; all 43 models play, end, have no DC and do not clip;
  - MODEL: tuning on FM and WSHP; all 21 models play and end; LPG decays by itself; TRIG hits strike it again; two
    parts share the voices with no slot faults;
  - ENV, LFO, CYCLE, SLOPE, TRIG (FILL, stopped, CHAOS);
  - XMOD: each algorithm changes the sound; MIX 0 never runs; it stays on the first core;
  - GRAIN: each mode returns the send and holds a tail; FRZ plays the frozen buffer;
  - all 18 presets play and end;
  - a rack project packs and loads with its slots, MODEL, GRAIN and MIDI channels.
- **Second core** (`tests/dualcore_test.c`, rack build):
  - MACRO scenes are bit for bit the same on one core and on two, at every split;
  - all-MODEL scenes stay on the first core;
  - mixed scenes split the MACRO parts only.
- **UI** (`tests/ui_render.c`, rack build): 242 screens in 10 palettes, every value of every page of MACRO and MODEL,
  and the MOD A / MOD B / INSERT pages under every type. Result: 0 lint findings and 0 alignment misses.
- **Felucca's own tests** (`FELUCCA_RACK=0`): the second-core test passes, and the regression's voice and routing
  checks pass. The renders that need the CC0 sample library (SAMPLE, GRAIN, SLICE) could not run here; the library
  was not fetched.

## Not checked

- anything on hardware: the sound, the CPU load, the FPU in practice, the stacks under load;
- `regress.c` and `ui_test.c` on the rack build. They test Felucca's engines, which the rack drops.

## How it is put together

| File | What |
| --- | --- |
| `firmware/src/eng_macro.c`, `eng_model.c` | the two engines: parameters, voices, presets and their rack slots |
| `firmware/src/rack_mod.c` | MOD A / MOD B |
| `firmware/src/grids_map.h` | Grids' drum map (GPL-3.0) |
| `firmware/src/fx.c` | XMOD (`ins_xmod`), GRAIN (`grain_bus`), the first-core rule (`rack_core0`) |
| `firmware/mi/mi_glue.cc`, `mi_glue_fp.cc`, `mi_glue.h` | the Mutable code behind a C interface; objects in the pool, built at first use |
| `firmware/mi/mi_math.cc`, `inc/`, `override/` | the float maths (there is no libm), the few C / C++ headers, Plaits without user data |
| `firmware/app_rack.ld` | the memory map with the RAM / pool boundary moved |
| `tools/mi_build.py`, `mi_trim.py` | compile the Mutable objects in two groups (integer; float for the FPU), with build-time patches, and cut the resource tables to what the code reads |
| `tools/build_mi_host.sh`, `tests/rack_test.c` | the host build and test |
| `ext/eurorack` | pichenettes/eurorack at f4cfa76, stmlib at e3bd7c9 (git submodule) |

Felucca's own files changed:

- `core.h`, `engines.c`, `params.c`, `mod.c`, `voice.c`, `fx.c`, `seq.c`, `project.c`, `upreset.c`, `favorites.c`;
- `category.c`, `midi_control.c`, `midi_learn.c`, `icons.c`, `ui.c`, `ui_input.c`, `ui_graph.c`, `ui_draw.c`,
  `ui_menu.c`, `ui_help.c`;
- `tools/build.py`, `tools/gen_samples.py`;
- `tests/run_tests.sh`, `tests/hostsim.c`, `tests/c1_host.h`, `tests/dualcore_test.c`, `tests/ui_test.c`,
  `tests/ui_render.c`;
- `LICENSING.md`, `LICENSES/MIT-Mutable-Instruments.txt`.
