#!/usr/bin/env python3
"""Assembles the single-file Windhawk mods from src/.

Windhawk compiles one self-contained .wh.cpp file, so the shared portable core
(src/lop_core.h) and the shared Windows helpers (src/lop_win.h) are inlined into
each role file.  Edit the sources in src/ and re-run this script (or
installer/build-mod.ps1 on Windows); never edit the generated files.

Outputs (repository root, next to the README):
  lo-explorer-preview.wh.cpp   prevhost.exe  - the preview proxy
  lo-preview-broker.wh.cpp     explorer.exe  - the conversion broker

Usage:
  python3 tools/assemble.py [--check]
"""

import hashlib
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")

ROLES = [
    ("prevhost-mod.wh.cpp", "lo-explorer-preview.wh.cpp"),
    ("broker-mod.wh.cpp", "lo-preview-broker.wh.cpp"),
]

GUARDS = {
    "lop_core.h": ("LOPREVIEW_CORE_H",),
    "lop_win.h": ("LOPREVIEW_WIN_H",),
}

GENERATED_NOTE = (
    "// ---------------------------------------------------------------------------\n"
    "// GENERATED FILE - do not edit here.\n"
    "// Assembled by tools/assemble.py from src/{role}, src/lop_core.h and\n"
    "// src/lop_win.h.  Edit those files and re-run the assembler instead.\n"
    "// ---------------------------------------------------------------------------\n"
)


def read(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def build_id(role_path, header_paths):
    """Short, reproducible digest of the sources a generated file came from.

    Identical algorithm in installer/build-mod.ps1: sha256 of the concatenated
    sha256 digests (hex, lower case) of the role source followed by lop_core.h
    and lop_win.h, truncated to 8 characters.  It is logged at mod startup so a
    log line can be matched to the exact sources that were compiled.
    """
    digests = []
    for path in [role_path] + list(header_paths):
        with open(path, "rb") as fh:
            digests.append(hashlib.sha256(fh.read()).hexdigest())
    combined = hashlib.sha256("".join(digests).encode("utf-8")).hexdigest()
    return combined[:8]


def strip_guard(text, names):
    lines = []
    for line in text.splitlines(keepends=True):
        stripped = line.strip()
        if any(
            stripped == "#ifndef " + name
            or stripped == "#define " + name
            or stripped.startswith("#endif") and name in stripped
            for name in names
        ):
            continue
        lines.append(line)
    return "".join(lines)


def include_names(text):
    return re.findall(r'^\s*#include\s+"(lop_[a-z_]+\.h)"', text, re.M)


def strip_internal_includes(text):
    """Internal headers are inlined exactly once; drop the now-dead #include lines."""
    return re.sub(r'^[ \t]*#include\s+"lop_[a-z_]+\.h"[^\n]*\n', "", text, flags=re.M)


def assemble(role_file, headers):
    text = read(os.path.join(SRC, role_file))
    wanted = include_names(text)
    missing = [h for h in wanted if h not in headers]
    if missing:
        raise SystemExit("unknown internal header(s): %s" % ", ".join(missing))

    out = []
    inserted = set()
    for line in text.splitlines(keepends=True):
        m = re.match(r'^\s*#include\s+"(lop_[a-z_]+\.h)"', line)
        if not m:
            out.append(line)
            continue
        name = m.group(1)
        if name in inserted:
            continue  # already pulled in (lop_win.h includes lop_core.h)
        # pull in this header plus anything it includes, exactly once each
        for dep in include_names(headers[name]) + [name]:
            if dep in inserted:
                continue
            inserted.add(dep)
            out.append("\n// ==== inlined from src/%s ====\n" % dep)
            out.append(strip_internal_includes(strip_guard(headers[dep], GUARDS[dep])))
    body = "".join(out)

    # Stamp the build digest computed from the *sources* (never from this file,
    # which would be circular).
    role_path = os.path.join(SRC, role_file)
    digest = build_id(role_path, [os.path.join(SRC, "lop_core.h"), os.path.join(SRC, "lop_win.h")])
    if "@BUILDID@" not in body:
        raise SystemExit("%s: no @BUILDID@ placeholder found" % role_file)
    body = body.replace("@BUILDID@", digest)

    # Insert the "generated" banner right after the Windhawk metadata block.
    marker = "// ==/WindhawkMod==\n"
    note = GENERATED_NOTE.format(role=role_file)
    if marker in body:
        head, tail = body.split(marker, 1)
        body = head + marker + "\n" + note + tail
    else:
        body = note + body
    return body


def main():
    check_only = "--check" in sys.argv
    headers = {
        name: read(os.path.join(SRC, name))
        for name in ("lop_core.h", "lop_win.h")
    }
    status = 0
    for role_file, out_name in ROLES:
        body = assemble(role_file, headers)
        out_path = os.path.join(ROOT, out_name)
        if check_only:
            current = read(out_path) if os.path.exists(out_path) else ""
            if current != body:
                print("stale: %s" % out_name)
                status = 1
            else:
                print("up to date: %s" % out_name)
            continue
        with open(out_path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(body)
        print("wrote %s (%d lines)" % (out_name, body.count("\n") + 1))
    return status


if __name__ == "__main__":
    sys.exit(main())
