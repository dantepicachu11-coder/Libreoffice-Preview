# LOPreview v0.4 test
1. Compile `lo-explorer-preview.wh.cpp` as a local Windhawk mod.
2. Run `installer/install-user.ps1` from an already-open PowerShell window.
3. Restart Explorer, enable Preview Pane, test ODT/ODS.

Expected log: `Converting ...` then `Forwarding generated PDF ...`.

This is a synchronous proof-of-concept. If `prevhost.exe` cannot launch LibreOffice due to integrity restrictions, the log will show `CreateProcess failed` and the next build should move conversion into a medium-integrity broker.
