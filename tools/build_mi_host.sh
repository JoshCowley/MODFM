#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# FM1 Rack: tests/rack_test.c on the host: the Mutable objects (tools/mi_build.py, host clang++) and the
# firmware's C through tests/hostsim.c with FELUCCA_RACK=1. Run from the repo root after ./build.sh (build/gen).
#   sh tools/build_mi_host.sh && build/host/rack_test
set -e
OUT=build/host
mkdir -p "$OUT/mi"
OBJS=$(python3 tools/mi_build.py "$OUT/mi" | tail -1)
CC="${CC:-cc}"
HALI="-Ifirmware/hal -Ifirmware/bsp/chip -Ifirmware/bsp/boards/fm1 -Ifirmware/src"
$CC -O2 -w -DFELUCCA_RACK=1 $HALI -Ibuild/gen -Ifirmware/src -c tests/rack_test.c -o "$OUT/rack_test.o"
${CXX:-clang++} -o "$OUT/rack_test" "$OUT/rack_test.o" $OBJS -lm
echo "built $OUT/rack_test"
