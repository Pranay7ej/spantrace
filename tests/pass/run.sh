#!/bin/sh
# usage: run.sh <opt> <FileCheck> <plugin.so> <test.ll>
# Extra pass flags come from a "; ARGS:" line in the test file.
set -eu
opt="$1"; filecheck="$2"; plugin="$3"; test="$4"
args=$(sed -n 's/^; ARGS: //p' "$test")
# shellcheck disable=SC2086
"$opt" -load-pass-plugin="$plugin" -passes=spantrace $args -S "$test" -o - | "$filecheck" "$test"
