#!/bin/sh
# Syntax-checks the generated single-file mods against the stub SDK in
# tests/win-stubs (see that folder's README).  This catches typos, wrong
# argument types and use-before-declaration problems without a Windows compiler.
set -e
CXX="${CXX:-g++}"
DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$DIR")"
for f in lo-explorer-preview.wh.cpp lo-preview-broker.wh.cpp; do
  printf 'compiling %s ... ' "$f"
  "$CXX" -std=c++20 -fsyntax-only -Wall -Wextra -Wno-unused-parameter -Wno-unknown-pragmas \
      -I "$DIR/win-stubs" "$ROOT/$f"
  echo ok
done
