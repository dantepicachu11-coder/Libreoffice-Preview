#!/bin/sh
# Builds and runs the portable core unit tests.  Works with g++ or clang++.
set -e
CXX="${CXX:-g++}"
DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="${TMPDIR:-/tmp}/lopreview-tests"
"$CXX" -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter -o "$OUT" "$DIR/test_core.cpp"
"$OUT"
