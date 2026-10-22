#!/usr/bin/env bash
# Builds the libFuzzer harnesses with Homebrew LLVM (Apple's clang ships no libFuzzer), CPU code only.
# Usage: fuzz/build.sh [BUILD_DIR] [OUT_DIR]
#   BUILD_DIR: a configured CMake build of Nocturne, for its generated sources (default: build)
#   OUT_DIR:   where the harnesses go (default: BUILD_DIR/fuzz)
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
build=$(cd "${1:-$root/build}" && pwd)
out=${2:-$build/fuzz}
cxx=${NOCTURNE_FUZZ_CXX:-/opt/homebrew/opt/llvm/bin/clang++}
jobs=${NOCTURNE_FUZZ_JOBS:-3}

[[ -x $cxx ]] || { echo "fuzz: no $cxx (brew install llvm)" >&2; exit 1; }
for generated in build_info.cpp isf_sources.cpp; do
    [[ -f $build/$generated ]] || { echo "fuzz: no $build/$generated; configure the build first" >&2; exit 1; }
done

flags=(-std=c++20 -O1 -g -fno-omit-frame-pointer -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize=vptr
       -I"$root/src" -isystem "$root/third_party/nlohmann" -mmacosx-version-min=14.0)

# The protocol library's sources, as CMakeLists.txt lists them, plus the media and Pro DJ Link units the harnesses need.
protocol=$(awk '/^add_library\(nocturne_protocol STATIC/ {on=1; next} on && /^\)/ {exit} on {print $1}' "$root/CMakeLists.txt" |
    sed -e "s|\${CMAKE_CURRENT_BINARY_DIR}|$build|" -e "s|^src/|$root/src/|")
sources=($protocol "$root/src/media/hap.cpp" "$root/src/media/snappy.cpp" "$root/src/djlink/djlink_packets.cpp")

rm -rf "$out/obj"
mkdir -p "$out/obj"
printf '%s\n' "${sources[@]}" | xargs -P "$jobs" -I{} sh -c \
    'f="$1"; o="$2/obj/$(basename "$f" .cpp).o"; shift 2; exec "$@" -c "$f" -o "$o"' _ {} "$out" "$cxx" "${flags[@]}"
rm -f "$out/libnocturne_fuzz.a"
ar rcs "$out/libnocturne_fuzz.a" "$out"/obj/*.o

for harness in "$root"/fuzz/*_fuzz.cpp; do
    name=$(basename "$harness" .cpp)
    echo "fuzz: $name"
    "$cxx" "${flags[@]/fuzzer-no-link/fuzzer}" "$harness" "$out/libnocturne_fuzz.a" \
        -framework CoreFoundation -framework Foundation -o "$out/$name"
done
