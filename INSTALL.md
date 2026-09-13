# Installing LOPreview 0.5.3

LibreOffice documents (`.odt`, `.ods`, `.odp`, `.odg`, `.odf`) preview in the
**normal Windows 11 Explorer preview pane** (`Alt+P`), exactly like PDFs.
LibreOffice renders the document to PDF on your machine, and the **PDF preview
handler Windows already uses** displays it. Nothing is uploaded anywhere,
Microsoft Office is not used, and no new preview program is installed.

You install three things, once, in this order:

1. prerequisites (LibreOffice + Windhawk),
2. the two Windhawk mods,
3. the per-user file association registration (PowerShell script).

Everything works without Administrator rights.

------------------------------------------------------------------------

## 0. Prerequisites

* Windows 11 64-bit (22H2, 23H2, 24H2 or newer).
* **LibreOffice** installed for your user (default installer settings work).
* **Windhawk** installed (https://windhawk.net/).
* Explorer's preview pane enabled (`Alt+P`).
* A working PDF preview: open any `.pdf` in Explorer with the preview pane - it
  must preview. LOPreview uses that exact handler, so PDF preview must work
  first.

If you previously installed an older LOPreview build (0.5.1 or earlier),
replace BOTH mods with the files from this release and press *Compile mod*
again, then re-run the installer (it repairs the registry registration too).

------------------------------------------------------------------------

## 1. Register the file associations

From an **already open PowerShell window** in the unzipped release folder:

```powershell
cd .\LOPreview-0.5.3\installer
powershell -ExecutionPolicy Bypass -File .\install-user.ps1
```

Expected result - five `[ok]` lines, one per extension (the CLSID is discovered
automatically; it will not be hard-coded):

```
=== Preview handler associations =========================================
  [ok]   .odt -> LibreOffice.WriterDocument.1   -> {....}
  [ok]   .ods -> LibreOffice.CalcDocument.1     -> {....}
  [ok]   .odp -> LibreOffice.ImpressDocument.1  -> {....}
  [ok]   .odg -> LibreOffice.DrawDocument.1     -> {....}
  [ok]   .odf -> LibreOffice.MathDocument.1     -> {....}
```

The script also:

* prints diagnostics for Windows, LibreOffice, the PDF handler and Windhawk,
* creates `%LOCALAPPDATA%\LOPreview` (cache, logs, config),
* creates the low-integrity hand-off folder under
  `%USERPROFILE%\AppData\LocalLow\LOPreview\scratch`,
* backs up any previous association to
  `%LOCALAPPDATA%\LOPreview\preview-associations.json`.

> **Important (0.5.3 fix):** the shell reads the preview-handler CLSID from the
> registry key's **default** value. The 0.5.0/0.5.1 installer wrote a named
> value instead. The 0.5.3 installer writes the default value correctly,
> verifies it by read-back, and cleans up the old value. If you do not see the
> five `[ok]` lines with a matching CLSID, stop and read the printed error.

Read-only check (changes nothing):

```powershell
powershell -ExecutionPolicy Bypass -File .\install-user.ps1 -Diagnose
```

------------------------------------------------------------------------

## 2. Install the two Windhawk mods

For **each** of the two `.wh.cpp` files at the root of this release:

1. Open Windhawk.
2. **Advanced** tab (top right) -> **Create new mod**.
3. Open the `.wh.cpp` file in Notepad, select all (`Ctrl+A`), copy, and paste
   over the whole editor template.
4. Click **Compile mod**. Wait for "Compilation was successful".
   If compilation fails, Windhawk writes the error in the editor (and in
   `%ProgramData%\Windhawk\EditorWorkspace\compiler_errors.log`).
5. Click **Save and exit**, then make sure the mod is **enabled**.
6. In the mod's settings (**Details / Settings**), set the
   **process inclusion list** to exactly the process shown below - do not use
   `*`:

| File to paste | Process inclusion list | Runs inside |
|---|---|---|
| `lo-explorer-preview.wh.cpp` | `prevhost.exe` | the preview handler host (low integrity) |
| `lo-preview-broker.wh.cpp`   | `explorer.exe` | Explorer itself (normal integrity, runs LibreOffice) |

The metadata block at the top of each file already contains the correct
`@include` line; setting it in the UI guarantees Windhawk applies it.

Both mods are needed. The broker (in `explorer.exe`) is the one that actually
runs `soffice --headless`, because the preview host `prevhost.exe` runs at low
integrity and cannot create a LibreOffice profile or write PDF files.

------------------------------------------------------------------------

## 3. Restart Explorer

Log out and back in (simplest), or restart Explorer:

```powershell
Stop-Process -Name explorer -Force
```

Explorer restarts itself. The first Explorer launch after that starts the
broker mod (check its log, step 5).

------------------------------------------------------------------------

## 4. Use it

1. Open Explorer, press **Alt+P** to show the preview pane.
2. Click `MyDocument.odt` (or `.ods`/`.odp`/`.odg`/`.odf`).
3. First preview of a document converts it (typically a few seconds, no
   window appears). Every later selection of the same, unchanged document is
   instant - it is read from the local PDF cache.

The rendered preview supports scrolling and multipage documents exactly like
a PDF, because it **is** displayed by the PDF preview handler.

------------------------------------------------------------------------

## 5. If something does not work - where to look

Run the diagnostics first:

```powershell
powershell -ExecutionPolicy Bypass -File .\install-user.ps1 -Diagnose
```

Logs (include these if you report a problem):

| What | Location |
|---|---|
| Broker log (LibreOffice discovery, conversion, exit codes) | `%LOCALAPPDATA%\LOPreview\logs\lopreview.log` |
| Client log (stream identity, hand-off, forwarding) | `%USERPROFILE%\AppData\LocalLow\LOPreview\prevhost.log` |
| Windhawk log for each mod | Windhawk -> the mod -> **Show log** (or DbgViewMini) |
| Configuration | `%LOCALAPPDATA%\LOPreview\config.ini` |

The first log line of each mod must say **0.5.3** with a build digest:

```
[I 0.5.3 prevhost] LOPreview 0.5.3 build 95977752 in prevhost.exe (...)
[I 0.5.3 broker]   LOPreview broker 0.5.3 build 71b89b5d starting in explorer.exe (...)
```

If an older version is shown, the old compiled mod is still loaded - paste the
new file and press *Compile mod* again.

When the broker cannot convert a document, the preview pane itself shows a
**diagnostic PDF page** with the reason and the log locations - Explorer never
crashes and never stays blank.

See `docs/TROUBLESHOOTING.md` for the full failure catalogue.

------------------------------------------------------------------------

## 6. Uninstall

1. Disable/remove both mods in Windhawk (Advanced tab).
2. Restore the previous associations:

```powershell
cd .\LOPreview-0.5.3\installer
powershell -ExecutionPolicy Bypass -File .\uninstall-user.ps1
```

   Add `-RemoveData` to also delete the cache, logs and scratch folders. The
   default "Open with" application was never changed; LibreOffice still opens
   these files normally.

------------------------------------------------------------------------

## Privacy / security notes

* Fully local: no network requests, no telemetry, no cloud services, no
  Microsoft Office.
* LibreOffice runs with a private throw-away profile per worker
  (`%LOCALAPPDATA%\LOPreview\tmp\lo-profile\...`) with macros disabled
  (`MacroSecurityLevel=3`, `DisableMacrosExecution`), no first-run wizard, no
  update checks, and a dead proxy.
* LibreOffice converts a **private copy** of each document in an isolated work
  folder; it never writes lock files next to your documents.
* Conversions run in a Windows job object that kills the whole
  `soffice.exe`/`soffice.bin` tree on timeout/cancel, with bounded concurrency
  and a queue.
* Only the normal user token is used; no services, no scheduled tasks, no
  Administrator rights.
