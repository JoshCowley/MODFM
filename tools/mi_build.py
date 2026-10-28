#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""FM1 Rack: the Mutable Instruments objects (Braids, Peaks) for the BRAIDS engine, for the FM-1 and the host.

The firmware is one C translation unit (felucca.c); the Mutable code is C++ and is compiled here on its own:
  1. the DSP sources (MI_DSP) and firmware/mi/mi_glue.cc, freestanding (firmware/mi/inc: no C library)
  2. the tables they leave undefined, read from `objdump -t` (*UND* braids::... / peaks::...)
  3. each module's resources.cc cut to those tables (tools/mi_trim.py), Braids' wavetables kept as stubs
  4. the cut resources compiled; the object list is returned for the link.
tools/build.py calls build() with the JieLi toolchain (pi32v2); tools/build_mi_host.sh runs it with the host's
clang++ for tests/braids_test.c. The eurorack checkout: ext/eurorack (git submodule; pichenettes/eurorack with
stmlib), or EURORACK=path."""
import os
import re
import subprocess
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent
MI = SRC / "firmware" / "mi"

MI_DSP = [
    "braids/analog_oscillator.cc",
    "braids/digital_oscillator.cc",
    "braids/macro_oscillator.cc",
    "peaks/modulations/lfo.cc",
    "peaks/modulations/multistage_envelope.cc",
]                                                 # (stmlib/utils/random.cc: not needed, see PATCHES)
RESOURCES = {"braids": "braids/resources.cc", "peaks": "peaks/resources.cc"}
# models BRAIDS leaves out (eng_braids.c BRD_SHAPE: never selected): their renders are compiled as silence, so
# their code and tables (33 KB of wavetables) stay out of the flash. A patched copy of the source is compiled;
# the original is not touched
SILENCED = {
    "braids/digital_oscillator.cc": ["RenderWavetables", "RenderWaveMap", "RenderWaveLine", "RenderWaveParaphonic",
                                     "RenderQuestionMark"],
}
# Each oscillator and LFO its own random numbers: Braids and Peaks draw from stmlib::Random, one generator for all;
# on the FM-1 two cores render parts at once (fx.c mix_block), so they would share it, race on it, and a part's
# sound would depend on which core rendered it (tests/dualcore_test.c caught it). The classes get a generator of
# their own (SeedRandom, RandomWord, RandomSample: the same LCG as stmlib's), their sources call it. Headers are
# patched into OUT/override (first on the include path), sources into OUT; the checkout is not touched.
RNG_MEMBERS = """
  // FM1 Rack (tools/mi_build.py): its own random numbers, not stmlib::Random's shared one
  inline void SeedRandom(uint32_t s) { rng_ = s; }
  inline uint32_t RandomWord() { rng_ = rng_ * 1664525UL + 1013904223UL; return rng_; }
  inline int16_t RandomSample() { return static_cast<int16_t>(RandomWord() >> 16); }
  uint32_t rng_;
"""
RNG_CALLS = [(r"\bRandom::GetWord\(\)", "RandomWord()"), (r"\bRandom::GetSample\(\)", "RandomSample()")]
PATCHES = {
    "braids/digital_oscillator.h": [(r"(class DigitalOscillator \{\n public:\n)", r"\1" + RNG_MEMBERS)],
    "braids/macro_oscillator.h": [(r"(class MacroOscillator \{\n public:\n)",
                                   r"\1  inline void SeedRandom(uint32_t s) { digital_oscillator_.SeedRandom(s); }"
                                   r"  // FM1 Rack\n")],
    "peaks/modulations/lfo.h": [(r"(class Lfo \{\n public:\n)", r"\1" + RNG_MEMBERS)],
    "braids/digital_oscillator.cc": RNG_CALLS,
    "peaks/modulations/lfo.cc": RNG_CALLS[1:],     # (GetSample only)
}

# tables a pointer table lists that this build never reads: Peaks' filters and delay (its envelope indexes only
# its own curves); kept as one zero element, the pointer 0
STUBS = {
    "braids": [],
    "peaks": ["lut_svf_cutoff", "lut_svf_damp", "lut_svf_scale", "lut_gravity", "lut_delay_times"],
}
CXXFLAGS = ["-x", "c++", "-std=gnu++11", "-Os", "-ffunction-sections", "-fdata-sections", "-fno-exceptions",
            "-fno-rtti", "-fno-threadsafe-statics", "-fno-use-cxa-atexit", "-fno-unwind-tables",
            "-fno-asynchronous-unwind-tables", "-w"]
FREESTANDING = ["-ffreestanding", "-fno-builtin", "-nostdlibinc", "-nostdinc++"]


def eurorack():
    e = Path(os.environ.get("EURORACK", SRC / "ext" / "eurorack")).resolve()
    if not (e / "braids" / "macro_oscillator.cc").exists() or not (e / "stmlib" / "stmlib.h").exists():
        raise SystemExit(f"mi_build: no eurorack checkout at {e}: git submodule update --init ext/eurorack && "
                         "git -C ext/eurorack submodule update --init stmlib (or EURORACK=path)")
    return e


def demangle_ns(sym):
    """_ZN6braids8wav_sineE -> ('braids', 'wav_sine'); anything else -> None"""
    m = re.fullmatch(r"_ZN(\d+)(\w+)", sym)
    if not m:
        return None
    n = int(m.group(1))
    rest = m.group(2)
    ns, rest = rest[:n], rest[n:]
    m2 = re.fullmatch(r"(\d+)(\w+)E", rest)
    if not m2:
        return None
    k = int(m2.group(1))
    name = m2.group(2)[:k]
    if len(m2.group(2)) != k:
        return None
    return ns, name


def undefined(objdump_out):
    """the (namespace, name) pairs an `objdump -t` listing leaves undefined"""
    out = set()
    for line in objdump_out.splitlines():
        if "*UND*" not in line:
            continue
        r = demangle_ns(line.split()[-1])
        if r:
            out.add(r)
    return out


# the sections an object may bring: what firmware/app.ld places (anything else would land where the linker
# guesses: a constructor table, C++'s .data.rel.ro, exception tables)
SECTIONS_OK = re.compile(r"\.(text|rodata|srodata|data|sdata|bss|sbss|pool)(\..*)?$|\.(comment|note.*|group|"
                         r"llvm_addrsig|symtab|strtab|shstrtab|rela?\..*|\w+\.attributes)$|$")


def silence(src, names):
    """the text of src with the bodies of DigitalOscillator::<name> for each name replaced by silence"""
    text = src.read_text()
    for n in names:
        m = re.search(r"^void DigitalOscillator::" + n + r"\(.*?\)\s*\{", text, re.M | re.S)
        if not m:
            raise SystemExit(f"mi_build: {src.name}: DigitalOscillator::{n} not found")
        depth, i = 1, m.end()
        while depth:
            depth += {"{": 1, "}": -1}.get(text[i], 0)
            i += 1
        head = text[m.start():m.end()]
        body = "\n  (void)sync;\n  while (size--) *buffer++ = 0;   // FM1 Rack: not in BRAIDS (tools/mi_build.py)\n}"
        text = text[:m.start()] + head + body + text[i:]
    return f"// {', '.join(names)} silenced (FM1 Rack: not in BRAIDS)\n" + text


def patch(src, dst, subs, text=None):
    """src with the regex substitutions subs (each must apply), into dst"""
    text = src.read_text() if text is None else text
    for pat, rep in subs:
        text, n = re.subn(pat, rep, text)
        if not n:
            raise SystemExit(f"mi_build: {src.name}: {pat!r} not found (a different eurorack checkout?)")
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(f"// generated by tools/mi_build.py from {src.name} (FM1 Rack): do not edit\n" + text)


def build(cc, objdump, out, freestanding=True, extra=(), rel=False):
    """compile the Mutable objects into OUT; cc(args) / objdump(args) run the tools; rel: paths relative to the
    repository (the toolchain in Docker sees it at its working directory). Returns the object paths"""
    e = eurorack()
    out = Path(out).resolve()
    out.mkdir(parents=True, exist_ok=True)

    def P(p):
        p = Path(p).resolve()
        if not rel:
            return str(p)
        try:
            return str(p.relative_to(SRC))
        except ValueError:
            raise SystemExit(f"mi_build: {p} is outside {SRC} (the Docker build sees only the repository): "
                             "use ext/eurorack")
    inc = [f"-I{P(MI / 'inc')}"] if freestanding else []
    ovr = out / "override"
    for h, subs in PATCHES.items():
        if h.endswith(".h"):
            patch(e / h, ovr / h, subs)
    flags = [*CXXFLAGS, *(FREESTANDING if freestanding else []), *extra, *inc, f"-I{P(ovr)}", f"-I{P(e)}",
             f"-I{P(MI)}"]
    objs = []
    for s in MI_DSP:
        o = out / (s.replace("/", "_").replace(".cc", ".o"))
        src = e / s
        if s in SILENCED or s in PATCHES:
            src = out / (s.replace("/", "_"))
            text = silence(e / s, SILENCED[s]) if s in SILENCED else (e / s).read_text()
            patch(e / s, src, PATCHES.get(s, []), text)
        cc([*flags, "-c", P(src), "-o", P(o)])
        objs.append(o)
    need = set()
    for o in objs:
        need |= undefined(objdump(["-t", P(o)]))
    for ns, res in RESOURCES.items():
        names = sorted(n for s, n in need if s == ns)
        cut = out / f"{ns}_resources.cc"
        subprocess.run([sys.executable, str(SRC / "tools" / "mi_trim.py"), str(e / res), ns, str(cut),
                        *(n for n in names if n not in STUBS[ns]), *("~" + s for s in STUBS[ns])],
                       check=True)
        o = out / f"{ns}_resources.o"
        cc([*flags, "-c", P(cut), "-o", P(o)])
        objs.append(o)
    o = out / "mi_glue.o"
    cc([*flags, "-c", P(MI / "mi_glue.cc"), "-o", P(o)])
    objs.append(o)
    for o in objs:                                # nothing may run before main: no constructor tables, and only
        heads = objdump(["-h", P(o)])             # sections app.ld places
        if re.search(r"\s\.(init_array|ctors|preinit_array)\b", heads):
            raise SystemExit(f"mi_build: {o.name} has a constructor table (a static object with a constructor)")
        if freestanding:
            for ln in heads.splitlines():
                m = re.match(r"\s*\d+\s+(\S+)\s+[0-9a-fA-F]+", ln)
                if m and not re.fullmatch(r"[0-9a-fA-F]{8}", m.group(1)) and not SECTIONS_OK.match(m.group(1)):
                    raise SystemExit(f"mi_build: {o.name}: section {m.group(1)} is not one firmware/app.ld places")
    return objs


if __name__ == "__main__":                        # host: tools/mi_build.py OUTDIR (clang++ and llvm-objdump)
    def run(tool):
        def f(args):
            r = subprocess.run([tool, *args], capture_output=True, text=True)
            if r.returncode:
                sys.stderr.write(r.stdout + r.stderr)
                raise SystemExit(f"mi_build: {tool} failed")
            return r.stdout
        return f
    objs = build(run(os.environ.get("CXX", "clang++")), run(os.environ.get("OBJDUMP", "llvm-objdump")), sys.argv[1],
                 freestanding=False, extra=["-O2"])
    print(" ".join(map(str, objs)))
