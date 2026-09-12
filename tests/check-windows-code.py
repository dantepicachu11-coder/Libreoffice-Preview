#!/usr/bin/env python3
"""Static consistency checks for the Windows-only LOPreview sources.

There is no Windows compiler in this workspace, so the mod sources are checked
mechanically for the mistakes that would otherwise only show up as clang errors
on the user's machine:

  1. every lop::/lopw:: symbol used by the mod sources exists,
  2. every symbol is declared *before* it is used (the mods are single
     translation units after tools/assemble.py inlines the headers),
  3. every method declared in a class body has an out-of-line definition,
  4. braces/parentheses balance in each file,
  5. no duplicate definition of the same function (assembler accidents).

Run: python3 tests/check-windows-code.py
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORE = os.path.join(ROOT, "src", "lop_core.h")
WIN = os.path.join(ROOT, "src", "lop_win.h")
MODS = [
    os.path.join(ROOT, "src", "prevhost-mod.wh.cpp"),
    os.path.join(ROOT, "src", "broker-mod.wh.cpp"),
]
# The generated single-file mods are what Windhawk compiles, so they get the
# same treatment (ordering checks excluded: the headers are inlined at the top).
GENERATED = [
    os.path.join(ROOT, "lo-explorer-preview.wh.cpp"),
    os.path.join(ROOT, "lo-preview-broker.wh.cpp"),
]

problems = []


def read(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


DEF_RE = re.compile(
    r"^(?:inline\s+|static\s+|constexpr\s+)*[A-Za-z_][\w:<>,&*\s]*?\b([A-Za-z_]\w*)\s*\("
)
TYPE_RE = re.compile(
    r"^(?:class|struct|enum(?:\s+class)?|using|typedef)\s+([A-Za-z_]\w*)|^constexpr\s+\w+\s+([A-Za-z_]\w*)\s*=|^inline\s+constexpr\s+\w+\s+([A-Za-z_]\w*)\s*="
)
MEMBER_RE = re.compile(r"^\s+(?:static\s+)?(?:constexpr\s+)?[A-Za-z_][\w:<>,&*\s]*?\b([A-Za-z_]\w*)\s*=")
ENUM_MEMBER_RE = re.compile(r"^\s*([A-Za-z_]\w*)\s*(?:=|,)")


def collect_declarations(path):
    """Returns {name: first_line} for functions and types defined at namespace scope."""
    names = {}
    lines = read(path).splitlines()
    in_template = False
    for idx, line in enumerate(lines, start=1):
        if line.startswith("template"):
            in_template = True
            continue
        if line.startswith("namespace") or line.startswith("#") or line.startswith("//"):
            in_template = False
            continue
        m = DEF_RE.match(line)
        if m:
            name = m.group(1)
            if name not in ("if", "for", "while", "switch", "return", "sizeof"):
                names.setdefault(name, idx)
            in_template = False
            continue
        m = TYPE_RE.match(line)
        if m:
            name = next(g for g in m.groups() if g)
            names.setdefault(name, idx)
            in_template = False
            continue
        if line.startswith("constexpr") or line.startswith("enum"):
            for part in re.findall(r"([A-Za-z_]\w*)\s*=", line):
                names.setdefault(part, idx)
            in_template = False
            continue
        if in_template:
            for part in re.findall(r"\b([A-Za-z_]\w*)\b", line.split("//")[0]):
                names.setdefault(part, idx)
            in_template = False
        # enum members inside an enum body
        if re.match(r"^    [A-Za-z_]\w*(,|\s*=)", line):
            m2 = ENUM_MEMBER_RE.match(line)
            if m2:
                names.setdefault(m2.group(1), idx)
    # global constants (arrays, pointers, constexpr values)
    for idx, line in enumerate(lines, start=1):
        if not re.match(r"^(?:inline\s+|constexpr\s+|const\s+|static\s+)+", line):
            continue
        for part in re.findall(r"\b([A-Za-z_]\w*)\s*(?:\[|=)", line):
            names.setdefault(part, idx)
        for part in re.findall(r"\bconst\s+([A-Za-z_]\w*)", line):
            names.setdefault(part, idx)
    return names


def collect_enum_members(path):
    """Enum members can be referenced as lop::kSomething."""
    out = {}
    text = read(path)
    for m in re.finditer(r"enum(?:\s+class)?\s+\w*\s*\{([^}]*)\}", text, re.S):
        for name in re.findall(r"([A-Za-z_]\w*)\s*(?:=[^,]*,|,)", m.group(1)):
            out.setdefault(name, text[: m.start()].count("\n") + 1)
    return out


def check_balance(path):
    """Full scanner: comments, string/char literals and raw strings are skipped."""
    text = read(path)
    i, n, line = 0, len(text), 1
    pairs = {"}": "{", ")": "(", "]": "["}
    stack = []
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            line += text[i:j if j > 0 else n].count("\n")
            i = n if j < 0 else j + 2
            continue
        if c == "R" and i + 1 < n and text[i + 1] == '"':
            j = text.find("(", i + 2)
            k = text.find(")", j)
            line += text[i : k if k > 0 else n].count("\n")
            i = n if k < 0 else k + 1
            continue
        if c == '"' or (c in "LuU" and i + 1 < n and text[i + 1] == '"') or (
            c == "u" and text[i + 1 : i + 8] == '8"'
        ):
            q = i if c == '"' else (i + 1 if c in "LuU" else i + 7)
            j = q + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == '"':
                    break
                if text[j] == "\n":
                    line += 1
                j += 1
            i = j + 1
            continue
        if c == "'":
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == "'":
                    break
                j += 1
            i = j + 1
            continue
        if c in "{[(":
            stack.append((c, line))
        elif c in "}])":
            if not stack or stack[-1][0] != pairs[c]:
                problems.append("%s:%d: stray '%s'" % (os.path.basename(path), line, c))
                return
            stack.pop()
        i += 1
    if stack:
        problems.append(
            "%s: %d unclosed bracket(s), innermost %r at line %d"
            % (os.path.basename(path), len(stack), stack[-1][0], stack[-1][1])
        )


def check_duplicate_definitions(path):
    text = read(path)
    seen = {}
    # namespace-scope definitions only (line starts at column 0, no leading
    # template parameter list) - template overloads are legitimate.
    for m in re.finditer(
        r"^(?:inline\s+|static\s+)?[A-Za-z_][\w:<>,&*\s]*?\b([A-Za-z_]\w*)\s*\(([^;{]*)\)\s*(?:const\s*)?\{",
        text,
        re.M,
    ):
        name = m.group(1)
        # Overloads are legal; only an identical parameter list is a duplicate.
        params = re.sub(r"\s+", " ", m.group(2)).strip()
        key = "%s(%s)" % (name, params)
        seen[key] = seen.get(key, 0) + 1
    for key, count in sorted(seen.items()):
        if count > 1:
            problems.append("%s: '%s' defined %d times at namespace scope"
                            % (os.path.basename(path), key, count))


def check_symbol_usage(sources, known, check_order=True):
    for path in sources:
        lines = read(path).splitlines()
        for idx, line in enumerate(lines, start=1):
            for ns, name in re.findall(r"\b(lop|lopw)::([A-Za-z_]\w*)", line):
                key = name
                if key not in known:
                    problems.append(
                        "%s:%d: %s::%s is not declared in lop_core.h/lop_win.h"
                        % (os.path.basename(path), idx, ns, name)
                    )
                    continue
                decl_line, decl_file = known[key]
                if check_order and decl_file == os.path.basename(path) and decl_line > idx:
                    problems.append(
                        "%s:%d: %s is declared later (line %d) - move the definition up"
                        % (os.path.basename(path), idx, name, decl_line)
                    )


def check_class_methods(path, class_names):
    text = read(path)
    for cls in class_names:
        body = re.search(r"class\s+%s\b.*?\n\};" % cls, text, re.S)
        if not body:
            problems.append("%s: class %s not found" % (os.path.basename(path), cls))
            continue
        body_text = body.group(0)
        declared = set()
        # A declaration line: leading whitespace, at least one type token, then
        # the name and a parameter list, ending with ';' (no initialiser).
        for line in body_text.splitlines():
            if "=" in line or not line.rstrip().endswith(";"):
                continue
            m = re.match(
                r"^\s+[\w:<>*&]+(?:\s+[\w:<>*&]+)*\s+(\w+)\s*\(", line)
            if m:
                declared.add(m.group(1))
        defined = set(re.findall(r"%s::([A-Za-z_]\w*)\s*\(" % cls, text))
        for name in sorted(declared):
            if name in ("QueryInterface", "AddRef", "Release"):
                continue
            inline_def = re.search(r"\b%s\s*\([^;{)]*\)\s*(?:const\s*)?(?:override\s*)?\{" % name,
                                   body_text)
            if name not in defined and not inline_def:
                problems.append("%s: %s::%s is declared but never defined"
                                % (os.path.basename(path), cls, name))


def main():
    known = {}
    for path in (CORE, WIN):
        for name, line in collect_declarations(path).items():
            known.setdefault(name, (line, os.path.basename(path)))
    for path in (CORE, WIN):
        for name, line in collect_enum_members(path).items():
            known.setdefault(name, (line, os.path.basename(path)))

    for path in (CORE, WIN) + tuple(MODS) + tuple(GENERATED):
        if not os.path.exists(path):
            problems.append("%s: missing (run tools/assemble.py)" % os.path.basename(path))
            continue
        check_balance(path)
        check_duplicate_definitions(path)

    check_symbol_usage(MODS, known, check_order=True)
    check_symbol_usage(GENERATED, known, check_order=False)
    check_class_methods(MODS[0], ["StreamProxy", "MemoryStream"])
    check_class_methods(GENERATED[0], ["StreamProxy", "MemoryStream"])

    # Generated mods must be self-contained single files and carry a real build
    # digest (a leftover placeholder means the assembler was not re-run).
    for path in GENERATED:
        text = read(path)
        if "@BUILDID@" in text:
            problems.append("%s: build digest placeholder was not substituted"
                            % os.path.basename(path))
        m = re.search(r'kBuildId\[\] = L"([0-9a-f]{8})"', text)
        if not m:
            problems.append("%s: no build digest found" % os.path.basename(path))

    # Generated mods must be self-contained single files.
    for path in GENERATED:
        text = read(path)
        for line in text.splitlines():
            if re.match(r'^\s*#include\s+"lop_', line):
                problems.append("%s: internal include left in the generated file" % os.path.basename(path))

    if problems:
        print("check-windows-code: %d problem(s)" % len(problems))
        for p in problems:
            print("  -", p)
        return 1
    print("check-windows-code: OK (%d symbols known)" % len(known))
    return 0


if __name__ == "__main__":
    sys.exit(main())
