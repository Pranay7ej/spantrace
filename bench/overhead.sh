#!/usr/bin/env bash
# Builds bench/workload.cpp three ways and reports the slowdown per phase.
#
#   bench/overhead.sh <build-dir> [clang++] [runs]
#
# plain     - no instrumentation
# traced    - every function
# filtered  - only pipeline::* (the "I know where to look" case)
set -euo pipefail

build="${1:?build dir}"
cxx="${2:-clang++-18}"
runs="${3:-5}"
here="$(cd "$(dirname "$0")" && pwd)"
plugin="$build/pass/SpanTracePass.so"
rt="$build/runtime/libspantrace.a"
out="$build/overhead"
mkdir -p "$out"

common=(-O2 -std=c++17 -pthread)
"$cxx" "${common[@]}" "$here/workload.cpp" -o "$out/plain"
"$cxx" "${common[@]}" -fpass-plugin="$plugin" "$here/workload.cpp" "$rt" -o "$out/traced"
# -mllvm options are parsed before -fpass-plugin loads the plugin, so the
# plugin also has to be loaded with -Xclang -load for its flags to exist.
"$cxx" "${common[@]}" -fpass-plugin="$plugin" -Xclang -load -Xclang "$plugin" \
  -mllvm -spantrace-filter='^pipeline::' \
  "$here/workload.cpp" "$rt" -o "$out/filtered"

# median of N runs for one phase
median() { sort -n | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}'; }

declare -A result
for variant in plain traced filtered; do
  for phase in fib pipeline sort total; do : > "$out/$variant.$phase"; done
  for _ in $(seq "$runs"); do
    SPANTRACE_OUT="$out/$variant.json" "$out/$variant" 2>/dev/null |
      while read -r phase ms; do echo "$ms" >> "$out/$variant.$phase"; done
  done
  for phase in fib pipeline sort total; do
    result[$variant.$phase]=$(median < "$out/$variant.$phase")
  done
done

echo "| phase | plain ms | traced ms | slowdown | filtered ms | slowdown |"
echo "|---|---:|---:|---:|---:|---:|"
for phase in fib pipeline sort total; do
  p=${result[plain.$phase]}; t=${result[traced.$phase]}; f=${result[filtered.$phase]}
  awk -v ph="$phase" -v p="$p" -v t="$t" -v f="$f" \
    'BEGIN { printf "| %s | %.1f | %.1f | %.2fx | %.1f | %.2fx |\n", ph, p, t, t/p, f, f/p }'
done
if [ -f "$out/traced.json" ]; then
  ls -l "$out/traced.json" | awk '{printf "\ntrace size (traced): %.1f MB\n", $5/1048576}'
fi
