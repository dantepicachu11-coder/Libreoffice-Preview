# Troubleshooting

Every failure is visible in two places: the preview pane itself (a diagnostic
PDF naming the failed stage) and the logs listed in `TESTING.md` §5.

## The preview pane shows a diagnostic page

The page lists what was tried. The most common reasons and their fixes:

### "the Explorer broker is not running"
The `lo-preview-broker` mod is not enabled for `explorer.exe`, or Explorer was
not restarted after enabling it.

1. Windhawk → `lo-preview-broker` → enabled, process list `explorer.exe`.
2. Restart Explorer. Verify: `%LOCALAPPDATA%\LOPreview\broker.json` exists and
   its `tick=` value is fresh (it is rewritten every 5 s).

### "LibreOffice (soffice.exe) was not found"
Set the full path in `%LOCALAPPDATA%\LOPreview\config.ini`:

```ini
soffice_path=C:\Program Files\LibreOffice\program\soffice.exe
```

then disable/enable the mods (or restart Explorer). The log lists every
location that was checked: registry `LibreOffice\UNO\InstallPath` (user and
machine, both registry views), `%ProgramW6432%`, `%ProgramFiles%`,
`%ProgramFiles(x86)%`, `PATH`, `%LOCALAPPDATA%\Programs\LibreOffice`,
`C:\LibreOffice`.

### "the document is locked by another program"
Another process holds the file without sharing read access. Close it, or copy
the file elsewhere and preview the copy. Note that having the document open in
LibreOffice itself is *not* a problem — LibreOffice does not lock the file.

### "conversion timed out"
Raise `timeout_seconds` for very large spreadsheets. The timeout always kills
the whole LibreOffice process tree (job object), so nothing is left behind.

### "LibreOffice did not produce a PDF (exit code 0)"
LibreOffice could not import the document — corrupt file, password protected,
or a document whose content does not match its extension (e.g. a renamed `.zip`).
The page repeats LibreOffice's own message when it printed one.

### Direct (low-integrity) conversion failed
If the broker is unavailable, the client tries to run LibreOffice itself. That
can only work if everything LibreOffice touches lives in the low-labelled
scratch folder, and it usually does not (documented in `ARCHITECTURE.md` §2).
The log states the exact reason; the fix is to get the broker running.

## The pane says "The PDF could not be previewed due to an internal error"

This is the message the *PDF preview handler* produced, i.e. the handler was
started but rejected the stream it was given. In LOPreview 0.5 this only happens
if the handler was initialised with a stream that is not a PDF — check the
client log for `IInitializeWithStream::Initialize failed on the generated PDF`
or `cached PDF was rejected by the handler` (both log the HRESULT). Please
report that HRESULT: the two known causes are a cached file that was truncated
after publishing (delete `%LOCALAPPDATA%\LOPreview\cache\*.pdf` and retry) and a
handler that requires a file name in `Stat()` (the generated stream includes one,
so this would be new information).

## The pane still shows the PDF handler's own error message

"The PDF could not be displayed/previewed due to an internal error" is the *PDF
preview handler* talking: it was handed something that is not a PDF. In 0.5.0
that can only happen after the client log says
`IInitializeWithStream::Initialize failed on the generated PDF` or
`cached PDF was rejected by the handler` (both print the HRESULT - please report
it). If those lines are absent, the **old 0.4 build is still active**: check the
version stamps described in `TESTING.md` §0. 0.4 forwards the ODF stream itself,
which produces exactly this message.

## Nothing happens at all / no log lines from prevhost

* Confirm the mod is enabled for `prevhost.exe` (not `*`).
* Confirm the association is registered:
  `install-user.ps1 -Diagnose` prints
  `<ext> -> <ProgID> -> <CLSID>` and must show the PDF handler CLSID, not
  "(no preview handler)".
* Confirm a PDF itself still previews — if `.pdf` does not preview, the problem
  is the PDF handler, not LOPreview.
* Check that DbgViewMini is capturing *all* processes (Windhawk mod logs go
  through `Wh_Log`, which DbgViewMini shows as `[WH] [local\@lo-explorer-preview]`).

## Explorer was reopened and previews stopped working

The broker lives inside `explorer.exe`. When Explorer restarts, Windhawk
re-injects and the broker registers itself again within a second or two; the
heartbeat in `broker.json` proves it. If previews do not come back, check the
broker log — Windhawk reports a failed mod load there.

## Last-resort diagnostics (not recommended)

`DisableLowILProcessIsolation` would make `prevhost.exe` run at medium
integrity so LibreOffice could be started straight from the preview host:

```
HKEY_CLASSES_ROOT\CLSID\{<PDF preview handler CLSID>}\DisableLowILProcessIsolation = 1 (DWORD)
```

Reasons not to do it, and why LOPreview does not:

* Explorer reads this value only from **HKLM** (`HKCR` merged view), so it needs
  **Administrator rights**, contradicting "no admin required".
* It weakens the isolation Microsoft applies to a handler that is not ours
  (the PDF handler becomes able to write to your profile from an untrusted
  stream), and Microsoft documents it as "not recommended".
* It changes behaviour for *all* PDF previews, not just LibreOffice documents.

If you enable it purely to compare behaviour, remove it afterwards and restart
Explorer; the LOPreview architecture does not depend on it.

## Windhawk reports "critical system processes"

The inclusion list for the broker should be exactly `explorer.exe` and for the
client exactly `prevhost.exe`. Both mods also check at runtime that they really
are in the expected process (`explorer.exe` must own the shell window), so a
broad pattern such as `*` cannot cause the broker to start in unrelated
processes.

## Disabling and removing

1. Disable both mods in Windhawk (or delete them).
2. `installer\uninstall-user.ps1` restores the previous preview-handler values.
3. `installer\uninstall-user.ps1 -RemoveData` additionally deletes
   `%LOCALAPPDATA%\LOPreview` and `%USERPROFILE%\AppData\LocalLow\LOPreview`.
4. Restart Explorer.

Your "open with" associations are never modified, so LibreOffice documents keep
opening in LibreOffice throughout.
