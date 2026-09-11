# LOPreview architecture

## 1. The requirement, restated

Explorer must keep using **its own preview pane and the PDF preview handler
that is already installed**. LOPreview may not become a preview handler, may
not render PDFs itself, and may not replace anything in the shell. All it may do
is make sure the existing PDF handler *receives a PDF* when the user selects a
LibreOffice document.

The chain is therefore fixed:

```
IStream(ODF)  ->  [ convert ]  ->  IStream(PDF)  ->  IInitializeWithStream::Initialize
                                                        on the real PDF handler
```

Everything below is about *where* the conversion runs and *how* the two halves
find each other.

## 2. Integrity levels decide the design

The Windows shell hosts preview handlers in `prevhost.exe`, which runs at **low
integrity** by default. Two documented consequences matter here:

1. A low-integrity process must not write to objects that carry a higher
   integrity label. `%LOCALAPPDATA%`, the user's profile and `%TEMP%` are
   medium, so the shell hands preview handlers a stream *precisely because* the
   handler cannot safely touch the file system.
2. A process created by a low-integrity process **inherits that token**. So
   `CreateProcess(soffice.exe)` from `prevhost.exe` succeeds, but LibreOffice
   then cannot create `%APPDATA%\LibreOffice`, cannot write its temporary
   files, and cannot write the PDF — the conversion fails, and if it did not
   fail, a preview would silently depend on a low-integrity process writing
   into the user's profile.

The escape hatches and why they are not used:

* `HKCR\CLSID\{PDF handler}\DisableLowILProcessIsolation = 1` would make
  `prevhost.exe` run at medium integrity. It must be set under **HKLM**
  (Explorer only reads the machine hive for this), so it needs Administrator
  rights, it weakens the isolation of a handler that Microsoft ships, and it
  changes behaviour for every PDF preview — not only ours. Requirements forbid
  weakening security to make the prototype work, so this is documented in
  `TROUBLESHOOTING.md` as a *diagnostic* only, never as part of the install.
* A separate COM server / own surrogate host: more registration surface, still
  low integrity, still needs the same conversion problem solved.

**Conclusion: the conversion has to happen in a medium-integrity process that is
already running as the user.** `explorer.exe` is exactly that. The broker mod
therefore runs inside the shell process — no extra service, no autostart entry,
no scheduled task, no elevation.

## 3. Why the hand-off is file based, not pipe based

The obvious design is a named pipe between prevhost and the broker. It has a
subtle dead end:

* The **broker cannot be the pipe server**. A named pipe created by a
  medium-integrity process is a medium object, and a low-integrity client cannot
  *write* to a higher-integrity object (mandatory policy "no-write-up"). The
  reverse direction works, which is why sandboxed browsers create their IPC
  pipes inside the sandboxed process.
* That would make the **client the server**. It works, but it has to be
  discovered by the broker (several prevhost instances means several pipe
  instances of the same name, and `CreateFile` may attach the broker's answer to
  *another* prevhost instance), plus overlapped connect/read timeouts and
  instance lifetime handling.

Files in a **low-labelled folder** have none of those problems, because the
integrity rules are directional and permissive in exactly the direction we need:

| Direction | Operation | Allowed? |
|---|---|---|
| low → medium | read `%LOCALAPPDATA%\...\cache\<key>.pdf` | yes (read-up is allowed) |
| medium → low | write `...\LocalLow\...\scratch\<key>.err` | yes (write-down is allowed) |
| low → low | create `<key>.req` in the scratch folder | yes |
| low → medium | write anything under `%LOCALAPPDATA%` | **no** (by design) |

And `%USERPROFILE%\AppData\LocalLow` is documented by Microsoft as carrying an
*inheritable* low mandatory label, "intended as the top-level folder that is
writable by default by low-integrity applications" — so subfolders created there
by either side inherit low integrity. That is the entire channel.

### The protocol on disk

```
%LOCALAPPDATA%\LOPreview\                 (medium: broker only)
    config.ini                            read by both roles
    broker.json                           heartbeat: pid, tick, soffice path, counters
    cache\<key>.pdf                       converted PDFs (published atomically)
    logs\lopreview.log                    broker log (client falls back to LocalLow)
    tmp\<key>\input.<ext>                 private copy LibreOffice actually opens
    tmp\<key>\out\<name>.pdf              LibreOffice output before publishing
    tmp\lo-profile\p0, p1                 throw-away profiles, one per worker

%USERPROFILE%\AppData\LocalLow\LOPreview\  (low: writable by prevhost)
    prevhost.log                          fallback log for the low side
    scratch\
        <key>.req                         request, written atomically by the client
        <key>.err                         failure status written by the broker
        <key>.odt|<ext>                   document copy when the host gave no path
        out\, lo-profile\, tmp\           LibreOffice scratch for the direct fallback
```

`<key>` is 16 hex digits (FNV-1a 64 over the canonical lower-cased path, size
and last-write time; content hash only for the path-less case). Both sides
compose paths from a bare name plus their own known root, and the hex-only key
rule is enforced while parsing, so a hostile peer cannot escape the directory.

### Request/response turns

1. Client: cache hit (`cache\<key>.pdf` validates: size ≥ 64, `%PDF-` header,
   `%%EOF` in the last 1 KiB) → hand it straight to the PDF handler. No broker,
   no LibreOffice.
2. Miss → client writes `scratch\<key>.req` (fully formed message, atomic
   rename) and polls `cache\<key>.pdf` / `scratch\<key>.err` every 50 ms.
3. Broker watches `scratch` with `FindFirstChangeNotificationW` (1 s safety-net
   poll) and enqueues the request.
4. Worker copies the document into `tmp\<key>\input.<ext>`, converts with
   LibreOffice, validates the output, and `MoveFileEx`es it to
   `cache\<key>.pdf` (atomic publish → the client never sees a partial file).
5. On failure the worker writes `scratch\<key>.err` with a human-readable
   reason, which the client turns into the visible diagnostic page.

Latency is dominated by LibreOffice startup (1–4 s); the file round trip adds
well under 100 ms in practice.

## 4. Concurrency, cancellation, timeouts

* Two workers by default (`max_concurrent`), a queue of 16, everything else is
  rejected with "the preview queue is busy".
* **Per-document dedup**: the in-flight key set means A.odt requested twice
  converts once; the second request is answered by the same file.
* **Rapid selection changes**: requests carry `(clientPid, seq)`. When a newer
  sequence arrives from the same client, older queued *and running* jobs are
  marked superseded and their LibreOffice process tree is killed through its job
  object — the new selection does not queue behind an abandoned one.
* **Explorer-side cancellation**: the shell calls `IPreviewHandler::Unload()`
  when the selection changes or the pane closes. The wrapped handler forwards
  it, the proxy signals its cancel event, the wait loop returns immediately and
  the handler answers `HRESULT_FROM_WIN32(ERROR_CANCELLED)`.
* **Timeouts**: `timeout_seconds` (default 120) per conversion, enforced with a
  job object carrying `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, so `soffice.bin` and
  any helper it spawned die with it — no orphaned LibreOffice processes, even if
  Explorer or prevhost is killed mid-conversion.
* **Bounded memory**: the mods never load a document into memory; the client
  streams, the broker copies on disk, and the PDF is opened as a file stream.

## 5. LibreOffice invocation

```
<soffice.exe> --headless --nologo --nodefault --nofirststartwizard
              --norestore --nolockcheck
              -env:UserInstallation=file:///<url-encoded throw-away profile>
              --convert-to pdf --outdir "<tmp\<key>\out>" "<tmp\<key>\input.<ext>>"
```

* `-env:UserInstallation` (percent-encoded file URL, UTF-8) keeps the user's
  real LibreOffice profile untouched.
* The profile is pre-seeded with `registrymodifications.xcu`:
  `DisableMacrosExecution=true`, `MacroSecurityLevel=3`, update checks off,
  proxy mode "manual" pointing at `127.0.0.1:9` so any outbound fetch fails
  immediately instead of reaching the network.
* A document is never opened where the user keeps it: LibreOffice creates
  `.~lock.<name>#` next to whatever it opens, so the broker always converts a
  private copy (`convert_original=0` is the default; the setting exists for
  large files on slow disks and is documented as a workspace polluter).
* The extension the file is given matters: a renamed or flat ODF is written as
  `input<sniffed ext>` (`.fodt`, `.fods`, …) so LibreOffice picks the right
  import filter.
* Output is verified before it is published: exists, ≥ 64 bytes, `%PDF-` header
  and `%%EOF` trailer. A LibreOffice exit code of 0 with no output (password
  protected, corrupt, unsupported) is treated as a failure and reported.
* The exit code and trimmed `stdout`/`stderr` are logged.

## 6. How the client identifies a document

In order of reliability:

1. **The preview site object** (`IObjectWithSite::SetSite` → `IShellItem` →
   `SIGDN_FILESYSPATH`). This is documented for preview handlers; if a host does
   not provide it, we fall back.
2. **The stream name** (`IStream::Stat().pwcsName`), which Explorer normally
   fills with the file name or path.
3. **The content**: the first 8 KiB are sniffed. `%PDF-` passes straight
   through; a ZIP whose *first* entry is the uncompressed `mimetype` string is
   identified as ODF from its media type (this is what the ODF specification
   guarantees, so no ZIP parser is needed); flat XML ODF is recognised from
   `office:mimetype`; anything else passes through untouched.

If the content says ODF but the document is not in `extensions`, or the content
is not ODF at all, the proxy behaves exactly like the handler it wraps: the
original stream is forwarded unchanged.

A LibreOffice extension whose content is not identifiable is still attempted
(the association already routed it to us) so that the user gets a diagnostic
page instead of a blank pane.

**Self-healing associations**: if the document *is* an ODF file but Explorer
routed it to a different preview handler (because the association registration
was never applied or was overwritten), the proxy creates the real PDF preview
handler itself and forwards to it. The hook only wraps objects that answer
`IPreviewHandler`, so unrelated COM objects are never touched, and the mod's
process list is `prevhost.exe`.

## 7. Failure handling

| Failure | Result |
|---|---|
| LibreOffice missing | diagnostic PDF: "LibreOffice was not found", with the paths that were checked |
| Unsupported/corrupt/password-protected document | diagnostic PDF with LibreOffice's own exit code/output |
| Conversion timeout / crash | job object kill, diagnostic PDF, next selection works normally |
| Disk full / locked file / deleted scratch folder | diagnostic PDF naming the Win32 error; folders are recreated on demand |
| Broker not running | client falls back to a direct (low-integrity) conversion attempt, and the reason is logged either way |
| Corrupt cache entry | validation fails → entry is ignored, regenerated and replaced |
| Explorer restart mid-conversion | broker restarts, stale `.req` files are picked up or dropped after 5 minutes, work dirs older than a day are removed |

Nothing in the failure path throws, blocks the shell's UI thread, or returns an
uninitialised handler: the worst case is a normal "no preview available" after a
clean failure.

## 8. What deliberately did **not** change

* No new preview handler, no CLSID of our own, no `IPreviewHandler` registered
  in the registry, no shell extension DLL.
* No replacement of the user's PDF handler; the CLSID is discovered from the
  `.pdf` registration (user hive first, then `UserChoice`, then
  `SystemFileAssociations`, then the machine hive) at mod load and used as is.
* The `.pdf` handler registration itself is never modified; only the LibreOffice
  ProgIDs get a `ShellEx\{8895b1c6-…}` value, which only affects the *preview*
  handler for those five extensions. The default "open with" application is
  untouched.
