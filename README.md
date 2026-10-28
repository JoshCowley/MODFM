# MODFM — FM1 Rack firmware for the M-VAVE FM-1

**FM1 Rack v1.7.0-rack2**: a fork of [Felucca](https://github.com/hugelton/Felucca) 1.7.0 that turns the FM-1 into a
dedicated rack. Felucca's own engines are dropped; the sound is Mutable Instruments' module code (Emilie Gillet, MIT)
under the rack's own names: **MACRO**, **MODEL**, **XMOD**, **GRAIN**, and per part two modulator slots (ENV, LFO,
SLOPE, CYCLE, TRIG1..3). Everything else is Felucca's. Full notes: [FM1-RACK.md](FM1-RACK.md).

> **Prototype status:** built for the FM-1 and host-tested. **Not yet run on an FM-1.** Flash at your own risk and keep
> a way back (below).

## Prototype quick start

1. **Firmware:** [`prebuilt/fm1-rack-1.7.0-rack2.fwsc`](prebuilt/) (MD5 `0f794676f489eb97bb9d0d103acb07b5`).
2. **Way back first:** have M-VAVE's updater / FM-1-transporter or the web installer's *Return to official V15* ready,
   and the BSP's `fm1_rescue` for a unit that won't start.
3. **Install:** connect the FM-1 by USB and install the `.fwsc` with Felucca's web installer (Chrome or Edge), as for
   any Felucca build.
4. **Bench checks:** work through [First things to check on the device](FM1-RACK.md#first-things-to-check-on-the-device)
   (boot/splash, tuning, CPU load on the heavy MODEL models, GRAIN, XMOD, TRIG, 2ND CORE, project save/load).

## Build from source

```sh
git clone https://github.com/JoshCowley/MODFM.git && cd MODFM
git submodule update --init --recursive   # Felucca-Engines, felucca-bsp, pichenettes/eurorack (stmlib)
./build.sh                                # needs the JieLi toolchain + AC79 SDK, see BUILDING.md
```

Without Docker: `JIELI_TOOLCHAIN=path AC79_SDK=path JIELI_DOCKER=0 ./build.sh`. `FELUCCA_RACK=0 ./build.sh` builds stock
Felucca 1.7.0. The output lands in `build/`.

## Licence

GPL-3.0-only, as Felucca. Mutable Instruments code is MIT; Felucca-Engines MIT; felucca-bsp MPL-2.0. See
[LICENSING.md](LICENSING.md), [LICENSES/](LICENSES/) and [CREDITS.md](CREDITS.md). Not affiliated with Hügelton,
M-VAVE or Mutable Instruments.

---

*Upstream Felucca README follows.*

## Felucca (upstream)

[![License: GPL-3.0-only](https://img.shields.io/badge/license-GPL--3.0--only-blue.svg)](LICENSE)

## Maintenance Notice

What began as a personal experiment grew far beyond my expectations. I’m grateful to everyone who contributed, supported the project, and made music with Felucca.

I’ve decided to step back from active development to focus on creating new instruments and music.

Felucca will remain available with occasional maintenance, but new features, issue responses, and pull request reviews are not guaranteed.

Thank you for everything. I hope you’ll look forward to my next creations.

## Overview

Multi-engine synthesizer firmware for the M-VAVE FM-1, version 1.7.0. Connect the FM-1 to a computer by USB, open the
[web installer](https://hugelton.github.io/Felucca/) in Chrome or Edge and press Install; no extra hardware is needed.
Installing is at your own risk: M-VAVE's updater or the installer's **Return to official V15** takes you back.
[Try it in your browser](https://hugelton.github.io/Felucca/webapp/try/), no FM-1 needed.

- Install: [web installer](https://hugelton.github.io/Felucca/)
- Edit: [web editor](https://hugelton.github.io/Felucca/webapp/editor/) ([Felucca-WebApp](https://github.com/hugelton/Felucca-WebApp))
- Read: the manual and the release notes are in the [wiki](https://github.com/hugelton/Felucca/wiki/Home)
- Build: [BUILDING.md](BUILDING.md)
- Related: [Felucca-Engines](https://github.com/hugelton/Felucca-Engines) (the ANALOG, LOFI, TRIO, WHEEL, NOISE and PHASE engines, MIT; still maintained) ·
  [felucca-bsp](https://github.com/hugelton/felucca-bsp) (the FM-1 hardware layer, MPL-2.0; Felucca builds on it from 1.7.0)

## AI disclaimer

Felucca is developed with the assistance of AI coding agents (code, tests, documentation, translation, maintenance). The
design, features, sound design and direction are the maintainer's or the community's, and no generative AI is used for music,
icons or artwork. See [On AI-Assisted Development and Responsibility](https://github.com/hugelton/Felucca/discussions/166).

## Credits and licence

Free software: [GPL-3.0-only](LICENSE); the Hügelton Sample Pack is CC0 1.0; the engines in `firmware/engines` are MIT
([Felucca-Engines](https://github.com/hugelton/Felucca-Engines)); the hardware layer in `firmware/bsp` is MPL-2.0
([felucca-bsp](https://github.com/hugelton/felucca-bsp)); the bundled fonts and ported DSP keep their own licences
([LICENSES/](LICENSES/), [LICENSING.md](LICENSING.md)). Thanks to everyone whose work and contributions are in it: [CREDITS.md](CREDITS.md).

M-VAVE and FM-1 are trademarks of their respective owners. Felucca is not affiliated with or endorsed by them.

Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
