#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""FM1 Rack: the Mutable Instruments objects the rack calls, for the FM-1 and the host.

The firmware is one C translation unit (felucca.c); the Mutable code is C++ and is compiled here on its own, in two
groups:
  int  Braids (MACRO) and Peaks (MOD A / B's ENV and LFO): integer code, compiled as it is
  fpu  Plaits (MODEL), Clouds (GRAIN) and Warps' cross-modulation (the XMOD insert): float code, compiled for the
       FM-1's single-precision FPU (FPU_FLAGS on the target) with every constant single precision; their maths
       (sinf, sqrtf, ...) is firmware/mi/mi_math.cc's (there is no libm)
Steps:
  1. copies of the sources the rack needs changed (PATCHES, SILENCED, OVERRIDES) into OUT; the checkout is not touched
  2. the DSP sources and the glue (firmware/mi/mi_glue.cc, mi_glue_fp.cc, mi_math.cc), freestanding (firmware/mi/inc:
     no C library)
  3. the tables they leave undefined, read from `objdump -t` (*UND* braids::... / plaits::... ...)
  4. each module's resources.cc cut to those tables (tools/mi_trim.py) and compiled; the object list returned
tools/build.py calls build() with the JieLi toolchain (pi32v2); tools/build_mi_host.sh runs it with the host's
clang++ for tests/rack_test.c. The eurorack checkout: ext/eurorack (git submodule; pichenettes/eurorack with stmlib),
or EURORACK=path."""
import os
import re
import subprocess
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent
MI = SRC / "firmware" / "mi"

MI_INT = [
    "braids/analog_oscillator.cc",
    "braids/digital_oscillator.cc",
    "braids/macro_oscillator.cc",
    "peaks/modulations/lfo.cc",
    "peaks/modulations/multistage_envelope.cc",
]                                                 # (stmlib/utils/random.cc: not needed, see PATCHES)
MI_FPU = [
    "plaits/dsp/voice.cc",
    "plaits/dsp/engine/additive_engine.cc", "plaits/dsp/engine/bass_drum_engine.cc",
    "plaits/dsp/engine/chord_engine.cc", "plaits/dsp/engine/fm_engine.cc", "plaits/dsp/engine/grain_engine.cc",
    "plaits/dsp/engine/hi_hat_engine.cc", "plaits/dsp/engine/modal_engine.cc", "plaits/dsp/engine/noise_engine.cc",
    "plaits/dsp/engine/particle_engine.cc", "plaits/dsp/engine/snare_drum_engine.cc",
    "plaits/dsp/engine/speech_engine.cc", "plaits/dsp/engine/string_engine.cc", "plaits/dsp/engine/swarm_engine.cc",
    "plaits/dsp/engine/virtual_analog_engine.cc", "plaits/dsp/engine/waveshaping_engine.cc",
    "plaits/dsp/engine/wavetable_engine.cc",
    "plaits/dsp/engine2/chiptune_engine.cc", "plaits/dsp/engine2/phase_distortion_engine.cc",
    "plaits/dsp/engine2/six_op_engine.cc", "plaits/dsp/engine2/string_machine_engine.cc",
    "plaits/dsp/engine2/virtual_analog_vcf_engine.cc", "plaits/dsp/engine2/wave_terrain_engine.cc",
    "plaits/dsp/chords/chord_bank.cc", "plaits/dsp/fm/algorithms.cc", "plaits/dsp/fm/dx_units.cc",
    "plaits/dsp/physical_modelling/modal_voice.cc", "plaits/dsp/physical_modelling/resonator.cc",
    "plaits/dsp/physical_modelling/string.cc", "plaits/dsp/physical_modelling/string_voice.cc",
    "plaits/dsp/speech/lpc_speech_synth.cc", "plaits/dsp/speech/lpc_speech_synth_controller.cc",
    "plaits/dsp/speech/lpc_speech_synth_phonemes.cc", "plaits/dsp/speech/lpc_speech_synth_words.cc",
    "plaits/dsp/speech/naive_speech_synth.cc", "plaits/dsp/speech/sam_speech_synth.cc",
    "clouds/dsp/correlator.cc", "clouds/dsp/granular_processor.cc", "clouds/dsp/mu_law.cc",
    "clouds/dsp/pvoc/phase_vocoder.cc",          # (silenced: SPECTRAL is not one of GRAIN's modes; no STFT)
    "warps/dsp/oscillator.cc",
    "stmlib/dsp/units.cc", "stmlib/dsp/atan.cc",
]
GLUE_INT = ["mi_glue.cc"]
GLUE_FPU = ["mi_glue_fp.cc", "mi_math.cc"]
RESOURCES = {"braids": "braids/resources.cc", "peaks": "peaks/resources.cc", "plaits": "plaits/resources.cc",
             "clouds": "clouds/resources.cc", "warps": "warps/resources.cc", "stmlib": None}
FPU_FLAGS = ["-mcpu=r3", "-mfprev1", "-ffp-contract=off"]   # the AC79's FPU (the SDK's own flags; no fused mul-add)
FLOAT_FLAGS = ["-fsingle-precision-constant", "-DTEST"]   # (TEST: stmlib's portable C, not its ARM assembly)

# models the rack leaves out (never selected: eng_macro.c BRD_SHAPE, eng_model.c MDL_ENGINE): their renders are
# compiled as silence, so their code and tables (Braids' 33 KB, Plaits' 50 KB of wavetables) stay out of the flash
SILENCED = {
    "braids/digital_oscillator.cc": ("DigitalOscillator", {
        n: "\n  (void)sync;\n  while (size--) *buffer++ = 0;\n}" for n in
        ("RenderWavetables", "RenderWaveMap", "RenderWaveLine", "RenderWaveParaphonic", "RenderQuestionMark")}),
    "plaits/dsp/engine/wavetable_engine.cc": ("WavetableEngine", {
        "LoadUserData": "\n  (void)user_data;\n}",
        "Render": "\n  (void)parameters; (void)already_enveloped;\n  for (size_t i = 0; i < size; ++i) out[i] = aux[i] = 0.0f;\n}",
    }),
    # Clouds' SPECTRAL mode (its phase vocoder: the STFT, 16 KB of window and ~9 KB of code): GRAIN has the other three
    "clouds/dsp/pvoc/phase_vocoder.cc": ("PhaseVocoder", {"Init": "\n}", "Process": "\n}", "Buffer": "\n}"}),
}
# Each oscillator and LFO its own random numbers: Braids and Peaks draw from stmlib::Random, one generator for all;
# on the FM-1 two cores render parts at once (fx.c mix_block), so they would share it, race on it, and a part's
# sound would depend on which core rendered it (tests/dualcore_test.c caught it). The classes get a generator of
# their own (SeedRandom, RandomWord, RandomSample: the same LCG as stmlib's), their sources call it. Plaits and
# Clouds: see below
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
    # Warps' cross-modulation functions are the class's private statics: public, so XMOD can call them alone
    "clouds/dsp/grain.h": [(r"Interpolate\(lut_window, gain, 4096\.0f\)", "Interpolate(lut_window, gain, 1024.0f)")],
    # Warps' sine table is Clouds' to the last digit: one copy (5 KB)
    "warps/dsp/oscillator.cc": [(r"(#include \"warps/dsp/oscillator.h\")", "\\1\n#include \"clouds/resources.h\""),
                                (r"\blut_sin\[", "clouds::lut_sin[")],
    # SPECTRAL's window table, never used (the phase vocoder is silenced)
    "clouds/dsp/granular_processor.cc": [(r"\blut_sine_window_4096\b", "NULL")],
    # (a constant a library call computes: a static constructor; sqrtf(20) written out)
    "warps/dsp/vocoder.h": [(r"sqrtf\(kNumBands\)", "4.47213595f")],
    "warps/dsp/modulator.h": [(r"(\n  template<XmodAlgorithm algorithm>\n  static float Xmod)", r"\n public:\1")],
}
# Plaits and Clouds draw from stmlib::Random's one generator as they are: they render on the first core only (MODEL
# parts and XMOD inserts are never handed to the second, fx.c mix_split; GRAIN is a bus, always the first core's), so
# its numbers come in one order. Its state is mi_glue_fp.cc's
OVERRIDES = {
    "plaits/user_data.h": "user_data.h",        # no user data in flash: the factory banks and waves only
}
# tables a pointer table lists that this build never reads: Peaks' filters and delay (its envelope indexes only
# its own curves), Plaits' 6-OP banks 2 and 3 (engines 3 and 4, never selected: eng_model.c MDL_ENGINE); kept as one zero element, the pointer 0
STUBS = {
    "braids": [], "clouds": [], "warps": [], "stmlib": [],
    "plaits": ["syx_bank_1", "syx_bank_2"],       # 6-OP's second and third DX7 banks (MODEL offers the first)
    "peaks": ["lut_svf_cutoff", "lut_svf_damp", "lut_svf_scale", "lut_gravity", "lut_delay_times"],
}
# smooth tables kept at a quarter of their points (4097 -> 1025, read with linear interpolation as before; their
# readers' scales patched to match: PATCHES, warps_xmod): Clouds' grain window, Warps' FOLD curve (12 KB each)
DECIMATE = {("clouds", "lut_window"): 4, ("warps", "lut_bipolar_fold"): 4}
CXXFLAGS = ["-x", "c++", "-std=gnu++11", "-Os", "-ffunction-sections", "-fdata-sections", "-fno-exceptions",
            "-fno-rtti", "-fno-threadsafe-statics", "-fno-use-cxa-atexit", "-fno-unwind-tables",
            "-fno-asynchronous-unwind-tables", "-w"]
FREESTANDING = ["-DMI_FREESTANDING", "-ffreestanding", "-fno-builtin", "-nostdlibinc", "-nostdinc++"]
SOFT_DOUBLE = re.compile(r"__(add|sub|mul|div|neg|eq|ne|lt|le|gt|ge|un|extendsf|truncdf|fix\w*|float\w*)df\w*")


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
    """the (namespace, name) pairs an `objdump -t` listing leaves undefined, and the plain C names"""
    out, plain = set(), set()
    for line in objdump_out.splitlines():
        f = line.split()
        if "*UND*" not in f or f[-1] == "*UND*" or f.index("*UND*") >= len(f) - 2:
            continue                              # (the null symbol: no name after the size)
        sym = f[-1]
        r = demangle_ns(sym)
        if r:
            out.add(r)
        elif not sym.startswith("_Z"):
            plain.add(sym)
    return out, plain


def defined(objdump_out):
    """the global names an `objdump -t` listing defines"""
    return {ln.split()[-1] for ln in objdump_out.splitlines() if "*UND*" not in ln and re.search(r"\sg\s", ln)}


# the sections an object may bring: what firmware/app.ld places (anything else would land where the linker
# guesses: a constructor table, C++'s .data.rel.ro, exception tables)
SECTIONS_OK = re.compile(r"\.(text|rodata|srodata|data|sdata|bss|sbss|pool)(\..*)?$|\.(comment|note.*|group|"
                         r"llvm_addrsig|symtab|strtab|shstrtab|rela?\..*|\w+\.attributes)$|$")


def silence(src, cls, bodies):
    """the text of src with the bodies of cls::<name> replaced by bodies[name] (silence)"""
    text = src.read_text()
    for n, body in bodies.items():
        m = re.search(r"^(?:void|bool) " + cls + "::" + n + r"\(.*?\)\s*\{", text, re.M | re.S)
        if not m:
            raise SystemExit(f"mi_build: {src.name}: {cls}::{n} not found")
        depth, i = 1, m.end()
        while depth:
            depth += {"{": 1, "}": -1}.get(text[i], 0)
            i += 1
        text = text[:m.start()] + text[m.start():m.end()] + body + text[i:]
    return f"// {', '.join(bodies)} silenced (FM1 Rack: not in the rack)\n" + text


def patch(src, dst, subs, text=None):
    """src with the regex substitutions subs (each must apply), into dst"""
    text = src.read_text() if text is None else text
    for pat, rep in subs:
        text, n = re.subn(pat, rep, text)
        if not n:
            raise SystemExit(f"mi_build: {src.name}: {pat!r} not found (a different eurorack checkout?)")
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(f"// generated by tools/mi_build.py from {src.name} (FM1 Rack): do not edit\n" + text)


def decimate(cut, ns):
    """the DECIMATE tables of a cut resources file at every k-th point"""
    text = cut.read_text()
    for (n_, name), k in DECIMATE.items():
        if n_ != ns:
            continue
        m = re.search(r"(const float " + name + r"\[\] = \{)(.*?)(\};)", text, re.S)
        if not m:
            raise SystemExit(f"mi_build: {cut.name}: {name} not found")
        vals = [v.strip() for v in m.group(2).split(",") if v.strip()]
        if (len(vals) - 1) % k:
            raise SystemExit(f"mi_build: {name}: {len(vals)} points do not divide by {k}")
        text = text[:m.start(2)] + "\n  " + ", ".join(vals[::k]) + "\n" + text[m.end(2):]
    cut.write_text(text)


def warps_xmod(e, dst):
    """warps/dsp/modulator.cc's cross-modulation functions (Diode, the Xmod<> specialisations) alone, as a header
    the XMOD glue includes: the rest of Warps' modulator (its vocoder, 73 KB of state) stays out"""
    text = (e / "warps/dsp/modulator.cc").read_text()
    a = text.index("/* static */\ninline float Modulator::Diode")
    m = re.search(r"inline float Modulator::Xmod<ALGORITHM_NOP>\(.*?\n\}\n", text, re.S)
    if not m:
        raise SystemExit("mi_build: warps/dsp/modulator.cc: the Xmod functions not found")
    dst.parent.mkdir(parents=True, exist_ok=True)
    body = text[a:m.end()]
    fold = "return Interpolate(lut_bipolar_fold + 2048, sum, kScale);"
    if fold not in body or "const float kScale = 2048.0f" not in body:
        raise SystemExit("mi_build: warps/dsp/modulator.cc: FOLD's table read not found")
    body = body.replace(fold, fold.replace("2048", "512")).replace("const float kScale = 2048.0f",
                                                                    "const float kScale = 512.0f")   # (DECIMATE)
    dst.write_text("// generated by tools/mi_build.py from warps/dsp/modulator.cc (FM1 Rack): do not edit\n"
                   "#include \"warps/dsp/modulator.h\"\n#include \"warps/resources.h\"\n"
                   "#include \"stmlib/dsp/dsp.h\"\nnamespace warps {\nusing namespace stmlib;\n" +
                   body + "\n}  // namespace warps\n")


def build(cc, objdump, out, freestanding=True, extra=(), rel=False, fpu=()):
    """compile the Mutable objects into OUT; cc(args) / objdump(args) run the tools; rel: paths relative to the
    repository (the toolchain in Docker sees it at its working directory); fpu: the float group's target flags.
    Returns the object paths"""
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
    for h, f in OVERRIDES.items():
        d = ovr / h
        d.parent.mkdir(parents=True, exist_ok=True)
        d.write_text((MI / "override" / f).read_text())
    warps_xmod(e, ovr / "warps_xmod.h")
    base = [*CXXFLAGS, *(FREESTANDING if freestanding else []), *extra, *inc, f"-I{P(ovr)}", f"-I{P(e)}",
            f"-I{P(MI)}"]
    groups = {"int": base, "fpu": [*base, *fpu, *FLOAT_FLAGS]}
    objs = []

    def compile_one(s, group, srcpath):
        o = out / (s.replace("/", "_").replace(".cc", ".o"))
        cc([*groups[group], "-c", P(srcpath), "-o", P(o)])
        objs.append(o)
    for group, lst in (("int", MI_INT), ("fpu", MI_FPU)):
        for s in lst:
            src = e / s
            if s in SILENCED or s in PATCHES:
                src = out / (s.replace("/", "_"))
                text = silence(e / s, *SILENCED[s]) if s in SILENCED else (e / s).read_text()
                patch(e / s, src, PATCHES.get(s, []), text)
            compile_one(s, group, src)
    for g in GLUE_INT:
        compile_one(g, "int", MI / g)
    for g in GLUE_FPU:
        compile_one(g, "fpu", MI / g)
    need, plain, mine = set(), set(), set()
    for o in objs:
        t = objdump(["-t", P(o)])
        n, p = undefined(t)
        need |= n
        plain |= p
        mine |= {demangle_ns(d) for d in defined(t)}
    need -= mine                                  # (a table another object defines: Plaits' word banks)
    for ns, res in RESOURCES.items():
        names = sorted(n for s, n in need if s == ns)
        if not res or not names:
            continue
        cut = out / f"{ns}_resources.cc"
        subprocess.run([sys.executable, str(SRC / "tools" / "mi_trim.py"), str(e / res), ns, str(cut),
                        *(n for n in names if n not in STUBS[ns]), *("~" + s for s in STUBS[ns])],
                       check=True)
        decimate(cut, ns)
        compile_one(f"{ns}_resources_cut.cc", "fpu" if ns in ("plaits", "clouds", "warps") else "int", cut)
    have = set()
    for o in objs:
        have |= defined(objdump(["-t", P(o)]))
    soft = sorted(s for s in plain if SOFT_DOUBLE.fullmatch(s))
    if soft and freestanding:                      # the FPU is single precision: a double crept in
        raise SystemExit(f"mi_build: soft-double routines needed: {' '.join(soft)}")
    for o in objs:                                # nothing may run before main: no constructor tables, and only
        heads = objdump(["-h", P(o)])             # sections app.ld places
        if re.search(r"\s\.(init_array|ctors|preinit_array)\b", heads):
            raise SystemExit(f"mi_build: {o.name} has a constructor table (a static object with a constructor)")
        if freestanding:
            for ln in heads.splitlines():
                m = re.match(r"\s*\d+\s+(\S+)\s+[0-9a-fA-F]+", ln)
                if m and not re.fullmatch(r"[0-9a-fA-F]{8}", m.group(1)) and not SECTIONS_OK.match(m.group(1)):
                    raise SystemExit(f"mi_build: {o.name}: section {m.group(1)} is not one firmware/app.ld places")
    missing = sorted(s for s in plain - have if s not in ("memset", "memcpy", "memcmp", "memmove"))
    if missing and freestanding:
        raise SystemExit(f"mi_build: no definition here or in the firmware for: {' '.join(missing)}")
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
