# LOPreview 0.5.0

**LibreOffice documents in the normal Windows File Explorer preview pane** —
`.odt`, `.ods`, `.odp`, `.odg` (and `.odf`).

Explorer keeps using **its own preview pane and the PDF preview handler that is
already installed** for `.pdf`. LOPreview only adapts the LibreOffice document
into a PDF stream before that handler ever sees it, and converts with the
LibreOffice that is already installed on the machine.

```
Explorer  ->  preview pane  ->  existing PDF preview handler
                                        ^
                                        |  an IStream containing a PDF
                                        |
                              lo-explorer-preview.wh.cpp   (prevhost.exe)
                                        |
                     cache hit?  -------+--------  no
                        |                            |
             %LOCALAPPDATA%\LOPreview\cache   request file in the low scratch
                        |                            |
                        |                 lo-preview-broker.wh.cpp (explorer.exe)
                        |                            |
                        |                    soffice --headless --convert-to pdf
                        |                            |
                        +----------------------------+
```

No new preview application, no second PDF renderer, no Microsoft Office, no
cloud, no network, no Administrator rights.

---

## 1. Install

### a. Register the preview handler for LibreOffice documents (once)

From an **already open PowerShell window** (so you can read the output):

```powershell
cd <repo>\installer
powershell -ExecutionPolicy Bypass -File .\install-user.ps1
```

Expected output (the CLSID is discovered, never hard-coded):

```
=== Preview handler associations =========================================
  [ok]   .odt -> LibreOffice.WriterDocument.1 -> {A5A41CC7-02CB-41D4-8C9B-9087040D6098}
  [ok]   .ods -> LibreOffice.CalcDocument.1 -> {A5A41CC7-02CB-41D4-8C9B-9087040D6098}
  [ok]   .odp -> LibreOffice.ImpressDocument.1 -> {A5A41CC7-02CB-41D4-8C9B-9087040D6098}
  [ok]   .odg -> LibreOffice.DrawDocument.1 -> {A5A41CC7-02CB-41D4-8C9B-9087040D6098}
  [ok]   .odf -> LibreOffice.MathDocument.1 -> {A5A41CC7-02CB-41D4-8C9B-9087040D6098}
```

It also reports Windows build, LibreOffice, the PDF preview handler, Windhawk,
and it creates `%LOCALAPPDATA%\LOPreview` (cache/logs/config) plus the
low-integrity scratch folder. Everything is per user and reversible
(`uninstall-user.ps1` or `install-user.ps1 -RestoreAssociations`).
Use `-Diagnose` to look without touching anything.

### b. Install the two Windhawk mods

The mods live in the repository root and are **generated** from `src/`
(regenerate with `python3 tools/assemble.py` or `installer\build-mod.ps1`).

| File | Process inclusion list | What it does |
|---|---|---|
| `lo-explorer-preview.wh.cpp` | `prevhost.exe` | Wraps the PDF preview handler, hands it a converted PDF |
| `lo-preview-broker.wh.cpp`   | `explorer.exe`  | Runs LibreOffice at the user's normal integrity level |

In Windhawk: **Advanced → Create new mod**, paste the complete file, *Compile
mod*, then enable it and set the process inclusion list to the process in the
table (not `*`). Restart Explorer (or sign out and in) afterwards.

> **Why two mods?** The preview host `prevhost.exe` runs at **low integrity**.
> A process started from there cannot write to the user profile, so LibreOffice
> cannot create a profile, cannot write temporary files and cannot produce a
> PDF. `explorer.exe` runs at the user's normal integrity level and is already
> always running, so the conversion is done there. Details and evidence:
> [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

### c. Use it

Explorer → **Alt+P** → click `MyDocument.odt`. The first preview converts
(a few seconds); selecting the same unchanged file again is instant (cache).

---

## 2. What is verified, and what is not

This matters more than a feature list, so it comes before anything else.

**Verified in this repository (no Windows machine needed):**

* `tests/run-tests.sh` — 195 checks of the portable core: ODF/ZIP and flat-XML
  sniffing, cache keys, the prevhost↔broker wire protocol (including hostile
  input), INI parsing, file URL escaping, and the built-in PDF writer (whose
  xref offsets are parsed back and validated).
* `tests/check-windows-code.py` — every `lop::`/`lopw::` symbol used by the mods
  exists and is declared before use; no duplicate definitions; brace balance;
  every class method has a definition.
* `tests/compile-check.sh` — **both mods are compiled** (`g++ -std=c++20
  -fsyntax-only -Wall -Wextra`) against the stub SDK in `tests/win-stubs`. This
  found and fixed 8 real compile errors while writing 0.5.0 (namespace
  qualification, narrow/wide string mixing, wrong argument types).

**Not verified here:** anything that requires Windows at runtime — the
Windhawk hook actually firing, `IShellItem` being available from the preview
site object, LibreOffice starting from prevhost's low-integrity token, and the
PDF handler accepting the generated stream. The mods log every step of those
paths so a single test run tells you exactly which one fails
(see [`docs/TESTING.md`](docs/TESTING.md)).

**Preserved from the previously working build (v0.4):** the Windhawk metadata
block, `@include`, the `// @compilerOptions -std=c++20 -lole32 -luuid -lshlwapi
-lshell32 -ladvapi32` line, `Wh_Log` usage, `Wh_ModInit`/`Wh_ModUninit`, and the
`WindhawkUtils::SetFunctionHook(...)` call form, so the way you compile the mod
does not change.

---

## 3. Repository layout

```
src/lop_core.h              portable core (unit tested on any platform)
src/lop_win.h               Windows helpers shared by both mods
src/prevhost-mod.wh.cpp     client role  (prevhost.exe)
src/broker-mod.wh.cpp       broker role  (explorer.exe)

lo-explorer-preview.wh.cpp  GENERATED - the file you compile in Windhawk
lo-preview-broker.wh.cpp    GENERATED - the file you compile in Windhawk

installer/install-user.ps1  per-user install + full diagnostics
installer/uninstall-user.ps1
installer/build-mod.ps1     assembler (PowerShell peer of tools/assemble.py)
tools/assemble.py           assembler (used to produce the files above)

tests/run-tests.sh          portable core unit tests
tests/test_core.cpp
tests/check-windows-code.py static consistency checks
tests/compile-check.sh      compiles both mods against tests/win-stubs
tests/win-stubs/            stub Win32/COM headers (test scaffolding only)
tests/run-all.sh            everything above, in one command

docs/ARCHITECTURE.md        why this design, integrity analysis, file protocol
docs/TESTING.md             exact test procedure and test matrix
docs/TROUBLESHOOTING.md     failure modes and what to check
```

---

## 4. Behaviour and guarantees

* **Cache** — `%LOCALAPPDATA%\LOPreview\cache\<key>.pdf`, keyed on the
  canonical lower-cased path + file size + last-write time (content hash only
  when the host gives no path at all). Unchanged documents never re-convert.
  Age (30 days) and size (512 MB) limits are enforced by the broker, and a PDF
  that was touched in the last 10 minutes is never evicted.
* **The user's folders are never written to.** LibreOffice only ever opens a
  private copy in `%LOCALAPPDATA%\LOPreview\tmp\<key>\`, so no
  `.~lock.MyDocument.odt#` files appear next to your documents.
* **Security** — documents are treated as untrusted input: conversion runs with
  a throw-away LibreOffice profile that has **macro execution disabled**
  (`DisableMacrosExecution=true`, `MacroSecurityLevel=3`), LibreOffice's proxy
  settings point at a dead local port, arguments are passed as a properly quoted
  command line (no shell), the document is opened read-only, UNC/remote paths
  are refused (no network), the broker only accepts bare file names inside its
  own scratch directory (path traversal is impossible by construction), every
  conversion runs in a job object with a timeout and is killed as a tree, and
  nothing requires elevation.
* **Never crash Explorer** — all failure paths return a result; if no PDF can be
  produced, the pane shows a *diagnostic PDF* that names the failed step, the
  log paths and a checklist. The preview pane never goes blank silently.
* **Privacy** — no telemetry, no network calls, nothing leaves the machine.

---

## 5. Quick reference

| Thing | Where |
|---|---|
| Configuration | `%LOCALAPPDATA%\LOPreview\config.ini` |
| Broker + client log | `%LOCALAPPDATA%\LOPreview\logs\lopreview.log` |
| Low-integrity client log | `%USERPROFILE%\AppData\LocalLow\LOPreview\prevhost.log` |
| Request/status hand-off | `%USERPROFILE%\AppData\LocalLow\LOPreview\scratch\` |
| Broker heartbeat | `%LOCALAPPDATA%\LOPreview\broker.json` |
| PDF cache | `%LOCALAPPDATA%\LOPreview\cache\` |
| LibreOffice work dirs | `%LOCALAPPDATA%\LOPreview\tmp\` |
| Windhawk log | Windhawk → the mod → *Show log* (per process) |

### Version history

* **0.5.0** — complete rework of 0.4: medium-integrity broker in `explorer.exe`
  (the low-integrity prevhost cannot run LibreOffice usefully), file based
  hand-off that works in both integrity directions, PDF cache, LibreOffice
  discovery, macro-hardened throw-away profile, job-object timeouts, bounded
  concurrency with per-document dedup and cancellation, diagnostic PDF for
  failures, and the test/verification suite above.
* 0.4 — synchronous in-prevhost conversion proof of concept (kept as `src/`
  history): established that Explorer routes ODF documents to the PDF preview
  handler once the ProgID is registered.
