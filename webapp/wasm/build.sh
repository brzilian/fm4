#!/bin/sh
# Build the FM-4 engine to WebAssembly (freestanding, no libc).
# Needs clang with the wasm32 target and wasm-ld.
set -e
here=$(cd "$(dirname "$0")" && pwd)
src="$here/../../firmware/app/fm4"
out="$here/../fm4.wasm"
clang --target=wasm32 -O2 -nostdlib -ffreestanding -fno-builtin -DFM4_HOST \
  -I"$here/include" -I"$src" -Wall -Wno-unused-function \
  -Wl,--no-entry -Wl,--export-memory -Wl,--strip-all -Wl,-z,stack-size=65536 \
  -o "$out" "$here/fm4_wasm.c" "$src/opz.c" "$src/opz_ymfm_tables.c" "$src/multi.c" \
  "$src/fx.c" "$src/tq5.c" "$src/seq.c"
ls -l "$out"
