#!/bin/sh
# Dev build: incremental compile of generated C (only changed files) + runtime, then link.
set -e
cd "$(dirname "$0")"
mkdir -p build/gen
CC=${CC:-clang}; CXX=${CXX:-clang++}; J=${J:-2}
ls gen/main_*.c gen/functable.c | while read f; do
  o=build/gen/$(basename "$f" .c).o
  if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ runtime/include/xrt.h -nt "$o" ]; then echo "$f"; fi
done | xargs -r -P"$J" -I{} sh -c "$CC -O1 -w -fno-strict-aliasing -c -Iruntime/include -Igen {} -o build/gen/\$(basename {} .c).o && echo '  cc {}'"
# drop objects of generated files that no longer exist
for o in build/gen/*.o; do [ -f "gen/$(basename "$o" .o).c" ] || rm -f "$o"; done
for f in runtime/src/*.c; do $CC -O1 -g -c -Iruntime/include -Iruntime/src "$f" -o build/$(basename "$f").o; done
for f in runtime/src/*.cpp; do $CXX -std=c++17 -O1 -g -c -Iruntime/include -Iruntime/src -Ithird_party/vulkan/include "$f" -o build/$(basename "$f").o; done
$CXX -o build/xc3 build/*.o build/gen/*.o -lm -lpthread -ldl
echo "built build/xc3"
