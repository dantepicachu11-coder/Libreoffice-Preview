# Testing LOPreview

Everything here is meant to be run from an **already open** PowerShell window so
that output cannot disappear.

## 0. Preconditions

```powershell
# Windows 11, LibreOffice installed, Windhawk installed, preview pane enabled.
powershell -ExecutionPolicy Bypass -File .\installer\install-user.ps1 -Diagnose
```

The report must show:

* Windows 11 (build 22000+),
* a `soffice.exe` path,
* a PDF preview handler CLSID (on the reference machine:
  `{A5A41CC7-02CB-41D4-8C9B-9087040D6098}`),
* Windhawk found.

## 0. First: which build is actually loaded?

Windhawk keeps the **compiled** mod until you press *Compile mod* again, so an
old build can stay active while the editor already shows new source. Version
0.4 and 0.5 also share the same mod id, so the log tag
(`[local\@lo-explorer-preview]`) looks identical either way. Every line now
carries the build:

```
[I 0.5.1 prevhost] LOPreview 0.5.1 build 490c01f2 in prevhost.exe (pid 4321, integrity low (4096))
[I 0.5.1 broker]   LOPreview broker 0.5.1 build b7091ce4 starting in explorer.exe (pid 1234, integrity medium (8192))
```

If the version reads **0.5.0** (or there is no build digest), the file you
compiled is the one that failed with `use of undeclared identifier
'ASSOCSTR_SHELLIDLIST'`; re-paste the generated file and press *Compile mod*
again before testing.

The generated files are checked like this before you paste them:

```powershell
# nothing may be printed (the call was removed in 0.5.1):
Select-String -Path .\lo-explorer-preview.wh.cpp -Pattern 'ASSOCSTR_SHELLIDLIST'
# must report 0.5.1:
Select-String -Path .\lo-explorer-preview.wh.cpp -Pattern '@version'
# must match the published checksum:
(Get-FileHash .\lo-explorer-preview.wh.cpp -Algorithm SHA256).Hash
(Get-FileHash .\lo-preview-broker.wh.cpp -Algorithm SHA256).Hash
```

If you see these, the **0.4** build is still running - the new code never ran:

| Old log line | Where it comes from |
|---|---|
| `[WH] ... [125:Wh_ModInit]: PDF Preview Handler found` | the last line of the 0.4 file, its `Wh_ModInit` |
| `[WH] ... [106:Initialize]: Initialize stream name='...'` | the 0.4 `StreamProxy::Initialize` |

Neither string exists in 0.5.0. Note also that 0.4 stops logging right there:
its `Initialize` returns on a failed temp-directory creation **without** logging
anything further (that silent return is one of the things 0.5.0 replaced), so
"I see the name and then nothing" is expected for the old build.

To move to 0.5.0: open the mod in Windhawk, select all the source, replace it
with the generated `lo-explorer-preview.wh.cpp`, press **Compile mod**, confirm
no compile error (if there is one, Windhawk writes
`%ProgramData%\Windhawk\EditorWorkspace\compiler_errors.log`), then re-enable.
Do the same for `lo-preview-broker.wh.cpp`, whose `@include` must be
`explorer.exe`.

## 1. Build and install

```powershell
# 1a. regenerate the single-file mods after any edit in src/
python tools\assemble.py            # or: installer\build-mod.ps1

# 1b. register the preview handler for the LibreOffice ProgIDs (per user)
cd installer
powershell -ExecutionPolicy Bypass -File .\install-user.ps1
```

In Windhawk, for **each** of the two generated files:

1. Advanced → *Create new mod* → paste the whole file → **Compile mod**.
2. Enable it.
3. Settings → process inclusion list:
   * `lo-explorer-preview.wh.cpp` → `prevhost.exe`
   * `lo-preview-broker.wh.cpp` → `explorer.exe`
   (not `*`; the broker additionally refuses to run unless it really is the
   shell process, so a wide list cannot cause damage.)
4. Restart Explorer: `Stop-Process -Name explorer -Force` (it restarts itself)
   or sign out and back in.

## 2. First run — the three checks that matter

Open Windhawk → `lo-preview-broker` → **Show log**, and Windhawk →
`lo-explorer-preview` → **Show log** (or DbgViewMini, filter `[WH]`).

### Check A — is the broker alive and did it find LibreOffice?

Broker log (`lo-preview-broker` in explorer.exe, also
`%LOCALAPPDATA%\LOPreview\logs\lopreview.log`):

```
LOPreview broker 0.5.0 starting in explorer.exe (pid 1234, integrity medium (8192))
LibreOffice: C:\Program Files\LibreOffice\program\soffice.exe (found via registry ...)
```

If the second line is missing → *LibreOffice was not found*: set `soffice_path`
in `%LOCALAPPDATA%\LOPreview\config.ini` and disable/enable the mod.

### Check B — does the client see the right handler and the broker?

Client log (`lo-explorer-preview` in prevhost.exe — prevhost only starts when
you select a file):

```
LOPreview 0.5.0 in prevhost.exe (pid 4321, integrity low (4096))
PDF preview handler: {A5A41CC7-02CB-41D4-8C9B-9087040D6098} (found via UserChoice(...))
Explorer broker: running (pid 1234), soffice C:\Program Files\LibreOffice\program\soffice.exe
CoCreateInstance hook installed
```

* `integrity low` is expected and *is the reason the broker exists*.
* If it says `Explorer broker: not running` → the broker mod is not enabled for
  `explorer.exe`; previews will still be attempted directly from prevhost and
  the log will say why that fails.

### Check C — the conversion itself

Select an `.odt` in Explorer with the preview pane open (`Alt+P`). Client log:

```
IInitializeWithStream::Initialize(stream 0x... , mode 0)
LibreOffice document detected: name='C:\Users\...\MyDocument.odt' type='.odt' key=8f...
asking the Explorer broker (pid 1234) to convert C:\Users\...\MyDocument.odt
broker produced the preview PDF: %LOCALAPPDATA%\LOPreview\cache\8f....pdf
```

Broker log for the same moment:

```
running soffice: --headless --nologo ... --convert-to pdf --outdir "..." "...\input.odt"
converted ...\input.odt -> ...\cache\8f....pdf (41234 bytes)
```

and the preview pane shows the document.

### Then verify the cache

Select another file, then select the `.odt` again:

```
IInitializeWithStream::Initialize(...)
LibreOffice document detected: ...
cache hit: C:\Users\...\AppData\Local\LOPreview\cache\8f....pdf
```

Instant, no LibreOffice process (`Get-Process soffice*` shows nothing).

## 3. Test matrix

| Area | Cases | What to expect |
|---|---|---|
| Writer | small, multi-page, images, tables | pages render, scrolling works |
| Calc | small, large (1000+ rows), several sheets, charts, formulas | all sheets exported, charts drawn |
| Impress | small, many slides, images, transitions | one page per slide |
| Draw | single/multi page, shapes | pages render |
| Math (.odf) | formula document | formula renders |
| File names | spaces, Unicode (Cyrillic/CJK), long path (>260 with long-path enabled) | identical behaviour, log shows the correct path |
| Cache | select twice unchanged | second is a `cache hit`, no soffice process |
| Cache | modify and save, select again | regenerated (key changes: size/mtime) |
| Cache | delete `cache\*.pdf`, select | regenerated |
| Corrupt cache | truncate a cached PDF to 100 bytes | ignored, regenerated (`generated file is not a valid PDF` in the broker log if it came from LibreOffice) |
| Corrupt document | rename a `.zip` to `.odt` | diagnostic PDF explaining that LibreOffice produced no output |
| Password protected | encrypted `.odt` | diagnostic PDF, not a hang (timeout is the backstop) |
| Locked document | open the ODF in LibreOffice and hold it | conversion of the on-disk copy still works; if the OS denies the read, diagnostic PDF "locked by another program" |
| Rapid selection | arrow-key quickly through 10 ODF files | previews settle on the last file; earlier jobs are skipped/killed (`skipping superseded request`) |
| Concurrency | preview two ODF files from two Explorer windows | at most `max_concurrent` soffice processes; the second request for the same document is deduplicated |
| LibreOffice missing | set `soffice_path=` to a wrong path, disable/enable the mods | diagnostic PDF naming the checked locations |
| LibreOffice crash | kill `soffice.bin` while converting | diagnostic PDF, next preview works |
| Timeout | set `timeout_seconds=5` | diagnostic PDF "conversion timed out after 5 s"; no orphaned soffice (`Get-Process soffice*`) |
| Broker absent | disable the broker mod, restart Explorer | direct fallback is attempted and the log states the exact low-integrity failure; the pane still shows a diagnostic page rather than an error dialog |
| Explorer restart | restart Explorer while converting | conversion output is still published or ignored; no crash, next preview works |
| Windows restart | reboot, preview again | broker starts with Explorer (mod loads with the process), cache is used |
| Uninstall | `uninstall-user.ps1` | the previous preview handler values return; `.odt` still opens in LibreOffice via the default app |

## 4. Local (non-Windows) checks

```bash
tests/run-all.sh
```

* `tests/run-tests.sh` — 195 portable core checks.
* `tests/check-windows-code.py` — symbol/definition/balance checks.
* `tests/compile-check.sh` — compiles both generated mods against the stub SDK.

Run these after every change to `src/`; they caught several real bugs
(namespace qualification, narrow/wide string mixing, a bad argument type, a
mis-numbered PDF object) before any Windows machine was involved.

## 5. Reporting a failure usefully

Collect, in this order:

1. `%LOCALAPPDATA%\LOPreview\logs\lopreview.log`
2. `%USERPROFILE%\AppData\LocalLow\LOPreview\prevhost.log`
3. Windhawk's log for both mods (they include process name and pid)
4. `%LOCALAPPDATA%\LOPreview\config.ini`
5. The output of `install-user.ps1 -Diagnose`

The first lines of the client log tell which stage failed: identification,
broker hand-off, LibreOffice start, or PDF validation — each stage logs its own
result, and the preview itself shows the same reason as a PDF page.
