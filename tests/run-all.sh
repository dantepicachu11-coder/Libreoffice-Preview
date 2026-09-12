#!/bin/sh
# Everything that can be verified without Windows, in one command.
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$DIR")"

echo "== 1/4 portable core unit tests =="
"$DIR/run-tests.sh"

echo
echo "== 2/4 assembler in sync =="
python3 "$ROOT/tools/assemble.py" --check

echo
echo "== 3/4 static consistency checks =="
python3 "$DIR/check-windows-code.py"

echo
echo "== 4/4 syntax-compile the generated mods (stub SDK) =="
"$DIR/compile-check.sh"

echo
echo "all local checks passed"
