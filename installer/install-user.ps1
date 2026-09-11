<#
    LOPreview - per-user installer / diagnostics
    ============================================

    Makes the normal Windows 11 Explorer preview pane render LibreOffice
    documents by pointing the LibreOffice document ProgIDs at the PDF preview
    handler Windows already uses.  The conversion itself is done by the two
    Windhawk mods (see ../README.md).

    This script:
      * reports Windows build, architecture, LibreOffice, PDF preview handler
        and Windhawk state (read-only),
      * creates the cache/log/scratch directories,
      * registers the existing PDF preview handler for .odt/.ods/.odp/.odg/.odf
        *per user* (HKCU\Software\Classes), backing up whatever was there,
      * never touches the default "Open with" application,
      * never needs Administrator rights.

    Run it from an existing PowerShell window so that output stays visible:

        cd <this folder>
        powershell -ExecutionPolicy Bypass -File .\install-user.ps1

    Useful switches:
        -Diagnose            report only, change nothing
        -RestoreAssociations undo the registry changes (uses the backup)
        -NoAssociations      create directories/config only
        -Extensions ".odt",".ods"   limit which extensions are registered
        -Force               register even if the ProgID is not LibreOffice's
        -Pause               wait for Enter before exiting (double-click use)
#>

[CmdletBinding()]
param(
    [switch]$Diagnose,
    [switch]$RestoreAssociations,
    [switch]$NoAssociations,
    [string[]]$Extensions = @('.odt', '.ods', '.odp', '.odg', '.odf'),
    [switch]$Force,
    [switch]$Pause
)

$ErrorActionPreference = 'Stop'
$script:Version = '0.5.0'
$script:PreviewHandlerGuid = '{8895b1c6-b41f-4c1c-a562-0d564250836f}'
$script:Problems = New-Object System.Collections.Generic.List[string]
$script:Warnings = New-Object System.Collections.Generic.List[string]

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

function Write-Head([string]$text) {
    Write-Host ''
    Write-Host ("=== " + $text + " " + ('=' * [Math]::Max(0, 66 - $text.Length))) -ForegroundColor Cyan
}
function Write-Ok([string]$text)   { Write-Host ("  [ok]   " + $text) -ForegroundColor Green }
function Write-Info([string]$text) { Write-Host ("  [info] " + $text) }
function Write-Warn([string]$text) {
    Write-Host ("  [warn] " + $text) -ForegroundColor Yellow
    $script:Warnings.Add($text)
}
function Write-Bad([string]$text) {
    Write-Host ("  [FAIL] " + $text) -ForegroundColor Red
    $script:Problems.Add($text)
}
function Write-Step([string]$text) { Write-Host ("  -> " + $text) }

function Get-RegValue {
    param([string]$Path, [string]$Name)
    try {
        $item = Get-ItemProperty -Path $Path -ErrorAction Stop
        if ($null -eq $item) { return $null }
        if ($null -eq $Name -or $Name -eq '') {
            return (Get-Item -Path $Path -ErrorAction Stop).GetValue('')
        }
        $value = $item.PSObject.Properties[$Name]
        if ($null -eq $value) { return $null }
        return $value.Value
    } catch {
        return $null
    }
}

function Set-RegString {
    param([string]$Path, [string]$Name, [string]$Value)
    if (-not (Test-Path -Path $Path)) { New-Item -Path $Path -Force | Out-Null }
    New-ItemProperty -Path $Path -Name $Name -Value $Value -PropertyType String -Force | Out-Null
}

function Remove-RegValue {
    param([string]$Path, [string]$Name)
    if (Test-Path -Path $Path) {
        Remove-ItemProperty -Path $Path -Name $Name -Force -ErrorAction SilentlyContinue
    }
}

function Get-DataRoot { Join-Path $env:LOCALAPPDATA 'LOPreview' }
function Get-LowRoot  { Join-Path (Join-Path $env:USERPROFILE 'AppData') 'LocalLow\LOPreview' }
function Get-BackupPath { Join-Path (Get-DataRoot) 'preview-associations.json' }

function New-DirIfMissing([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { return $false }
    if (Test-Path -LiteralPath $Path) { return $true }
    try {
        New-Item -ItemType Directory -Path $Path -Force | Out-Null
        return $true
    } catch {
        Write-Warn ("cannot create " + $Path + ": " + $_.Exception.Message)
        return $false
    }
}

# ---------------------------------------------------------------------------
# environment report
# ---------------------------------------------------------------------------

function Get-WindowsInfo {
    $info = [ordered]@{}
    try {
        $key = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
        $info.ProductName  = (Get-ItemProperty -Path $key -Name ProductName -ErrorAction SilentlyContinue).ProductName
        $info.DisplayVer   = (Get-ItemProperty -Path $key -Name DisplayVersion -ErrorAction SilentlyContinue).DisplayVersion
        if (-not $info.DisplayVer) {
            $info.DisplayVer = (Get-ItemProperty -Path $key -Name ReleaseId -ErrorAction SilentlyContinue).ReleaseId
        }
        $info.Build        = (Get-ItemProperty -Path $key -Name CurrentBuildNumber -ErrorAction SilentlyContinue).CurrentBuildNumber
        $info.UBR          = (Get-ItemProperty -Path $key -Name UBR -ErrorAction SilentlyContinue).UBR
    } catch { }
    $info.Is64 = [Environment]::Is64BitOperatingSystem
    $info.PSVersion = $PSVersionTable.PSVersion.ToString()
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $info.User = $identity.Name
    $info.IsAdmin = (New-Object Security.Principal.WindowsPrincipal($identity)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
    return $info
}

function Find-LibreOffice {
    $candidates = New-Object System.Collections.Generic.List[string]
    foreach ($root in @('HKCU:\Software\LibreOffice\UNO\InstallPath',
                        'HKLM:\SOFTWARE\LibreOffice\UNO\InstallPath',
                        'HKLM:\SOFTWARE\WOW6432Node\LibreOffice\UNO\InstallPath')) {
        $dir = Get-RegValue -Path $root -Name ''
        if ($dir) { $candidates.Add((Join-Path $dir 'soffice.exe')) }
    }
    foreach ($envName in @('ProgramW6432', 'ProgramFiles', 'ProgramFiles(x86)')) {
        $base = [Environment]::GetEnvironmentVariable($envName)
        if ($base) { $candidates.Add((Join-Path $base 'LibreOffice\program\soffice.exe')) }
    }
    $cmd = Get-Command soffice.exe -ErrorAction SilentlyContinue
    if ($cmd) { $candidates.Add($cmd.Source) }
    $candidates.Add((Join-Path $env:LOCALAPPDATA 'Programs\LibreOffice\program\soffice.exe'))
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) { return $candidate }
    }
    return $null
}

function Get-PdfPreviewHandler {
    function Read-Handler([string]$hive, [string]$subKey) {
        $value = Get-RegValue -Path (Join-Path $hive $subKey) -Name ''
        if ($value -and $value.StartsWith('{')) { return $value }
        return $null
    }
    $tries = New-Object System.Collections.Generic.List[object]
    $tries.Add(@('HKCU:', "Software\Classes\.pdf\ShellEx\$script:PreviewHandlerGuid", 'HKCU\Software\Classes\.pdf'))
    # the user's chosen default PDF app
    $progId = Get-RegValue -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.pdf\UserChoice' -Name 'ProgId'
    if ($progId) {
        $tries.Add(@('HKCU:', "Software\Classes\$progId\ShellEx\$script:PreviewHandlerGuid", "UserChoice $progId"))
        $tries.Add(@('HKLM:', "SOFTWARE\Classes\$progId\ShellEx\$script:PreviewHandlerGuid", "UserChoice $progId (machine)"))
    }
    $tries.Add(@('HKLM:', "SOFTWARE\Classes\SystemFileAssociations\.pdf\ShellEx\$script:PreviewHandlerGuid", 'HKLM SystemFileAssociations'))
    $tries.Add(@('HKLM:', "SOFTWARE\Classes\.pdf\ShellEx\$script:PreviewHandlerGuid", 'HKLM .pdf'))
    $tries.Add(@('HKCU:', "Software\Classes\SystemFileAssociations\.pdf\ShellEx\$script:PreviewHandlerGuid", 'HKCU SystemFileAssociations'))
    foreach ($t in $tries) {
        $value = Read-Handler $t[0] $t[1]
        if ($value) {
            $name = $null
            try {
                $name = (Get-Item -LiteralPath ("Registry::HKEY_CLASSES_ROOT\CLSID\" + $value) -ErrorAction Stop).GetValue('')
            } catch { }
            return [pscustomobject]@{ Clsid = $value; Source = $t[2]; Name = $name }
        }
    }
    return $null
}

function Find-Windhawk {
    $paths = @(
        (Join-Path $env:ProgramFiles 'Windhawk\windhawk.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'Windhawk\windhawk.exe')
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }
    $modDirs = @()
    foreach ($candidate in @((Join-Path $env:ProgramData 'Windhawk'),
                             (Join-Path $env:LOCALAPPDATA 'Windhawk'))) {
        if (Test-Path -LiteralPath $candidate) { $modDirs += $candidate }
    }
    $found = New-Object System.Collections.Generic.List[string]
    foreach ($dir in $modDirs) {
        try {
            Get-ChildItem -LiteralPath $dir -Recurse -Filter '*lo-*' -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -match 'lo-(explorer-preview|preview-broker)' } |
                ForEach-Object { $found.Add($_.FullName) } | Out-Null
        } catch { }
    }
    return [pscustomobject]@{ Exe = ($paths | Select-Object -First 1); Files = $found.ToArray() }
}

# ---------------------------------------------------------------------------
# directories + config
# ---------------------------------------------------------------------------

function Initialize-Directories {
    $root = Get-DataRoot
    $low = Get-LowRoot
    $scratch = Join-Path $low 'scratch'
    Write-Step ("data root     : " + $root)
    New-DirIfMissing $root | Out-Null
    New-DirIfMissing (Join-Path $root 'cache') | Out-Null
    New-DirIfMissing (Join-Path $root 'logs') | Out-Null
    New-DirIfMissing (Join-Path $root 'tmp') | Out-Null
    Write-Step ("low scratch   : " + $scratch)
    New-DirIfMissing $low | Out-Null
    New-DirIfMissing $scratch | Out-Null
    New-DirIfMissing (Join-Path $scratch 'out') | Out-Null
    New-DirIfMissing (Join-Path $scratch 'lo-profile') | Out-Null

    # The scratch folder must be writable by the *low integrity* prevhost
    # process.  %USERPROFILE%\AppData\LocalLow carries an inheritable low
    # mandatory label, so folders created below it inherit low integrity
    # automatically (documented by Microsoft).  Verify what we can and say so
    # honestly if it looks wrong.
    try {
        $probe = Join-Path $scratch ('.probe-' + [Guid]::NewGuid().ToString('N'))
        Set-Content -LiteralPath $probe -Value 'x' -NoNewline -ErrorAction Stop
        Remove-Item -LiteralPath $probe -Force -ErrorAction SilentlyContinue
        Write-Ok 'the scratch folder is writable'
    } catch {
        Write-Bad ("the scratch folder is not writable: " + $_.Exception.Message)
    }

    $configPath = Join-Path $root 'config.ini'
    if (-not (Test-Path -LiteralPath $configPath)) {
        $config = @'
; LOPreview configuration (read by the Windhawk mods on every use).
; Edit and save - no restart of Windows needed, but Explorer must be restarted
; for the broker mod, and a new preview starts a new value for the client mod.

enabled=1                 ; master switch for the preview proxy
use_broker=1              ; use the Explorer-hosted broker (recommended)
direct_fallback=1         ; if the broker is unavailable, try soffice from prevhost
redirect_any_handler=1    ; also route ODF documents whose association is missing
convert_original=0        ; 0 = always convert a private copy (keeps user folders clean)
extensions=.odt .ods .odp .odg .odf
soffice_path=             ; optional: full path to soffice.exe if not auto-detected
pdf_filter=pdf            ; e.g. pdf:writer_pdf_Export
timeout_seconds=120       ; LibreOffice conversion timeout
client_wait_ms=60000      ; how long prevhost waits for the broker
broker_max_age_ms=20000   ; heartbeat freshness required before using the broker
max_concurrent=2          ; simultaneous LibreOffice conversions in the broker
max_document_mb=512       ; refuse larger documents
cache_max_mb=512          ; PDF cache size on disk
cache_max_age_days=30     ; PDF cache age on disk
lower_priority=1          ; run LibreOffice at below-normal priority
log_level=2               ; 0=errors 1=warnings 2=info 3=debug
'@
        try {
            $utf8 = New-Object System.Text.UTF8Encoding($false)
            [System.IO.File]::WriteAllText($configPath, $config, $utf8)
            Write-Ok ("default config written: " + $configPath)
        } catch {
            Write-Warn ("cannot write " + $configPath + ": " + $_.Exception.Message)
        }
    } else {
        Write-Info ("config kept: " + $configPath)
    }
}

# ---------------------------------------------------------------------------
# file associations
# ---------------------------------------------------------------------------

function Get-ProgIdForExtension([string]$Extension) {
    $userChoice = Get-RegValue -Path ("HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\$Extension\UserChoice") -Name 'ProgId'
    if ($userChoice) { return [pscustomobject]@{ ProgId = $userChoice; Source = 'UserChoice' } }
    $hkcu = Get-RegValue -Path ("HKCU:\Software\Classes\$Extension") -Name ''
    if ($hkcu) { return [pscustomobject]@{ ProgId = $hkcu; Source = 'HKCU\Software\Classes' } }
    $hklm = Get-RegValue -Path ("HKLM:\SOFTWARE\Classes\$Extension") -Name ''
    if ($hklm) { return [pscustomobject]@{ ProgId = $hklm; Source = 'HKLM\SOFTWARE\Classes' } }
    return $null
}

function Test-LibreOfficeProgId([string]$ProgId) {
    if ([string]::IsNullOrWhiteSpace($ProgId)) { return $false }
    return ($ProgId -match '^(LibreOffice|OpenOffice|org\.openoffice|StarOffice)\.')
}

function Get-AssociationBackup {
    $path = Get-BackupPath
    if (-not (Test-Path -LiteralPath $path)) {
        return [pscustomobject]@{ Version = $script:Version; Created = (Get-Date).ToString('o'); Entries = @() }
    }
    try {
        $json = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
        if (-not $json.Entries) { $json | Add-Member -NotePropertyName Entries -NotePropertyValue @() -Force }
        return $json
    } catch {
        Write-Warn ("the previous association backup is unreadable (" + $path + "); a new one will be written")
        return [pscustomobject]@{ Version = $script:Version; Created = (Get-Date).ToString('o'); Entries = @() }
    }
}

function Save-AssociationBackup($backup) {
    $path = Get-BackupPath
    try {
        New-DirIfMissing (Get-DataRoot) | Out-Null
        $backup.Updated = (Get-Date).ToString('o')
        $json = $backup | ConvertTo-Json -Depth 6
        $utf8 = New-Object System.Text.UTF8Encoding($false)
        [System.IO.File]::WriteAllText($path, $json, $utf8)
        Write-Info ("association backup: " + $path)
    } catch {
        Write-Warn ("cannot write the association backup: " + $_.Exception.Message)
    }
}

function Register-Associations($pdfHandler) {
    $backup = Get-AssociationBackup
    $entries = @($backup.Entries)
    $registered = 0
    foreach ($extension in $Extensions) {
        $ext = $extension.ToLowerInvariant()
        if (-not $ext.StartsWith('.')) { $ext = '.' + $ext }
        $progInfo = Get-ProgIdForExtension $ext
        if (-not $progInfo) {
            Write-Warn ("$ext has no ProgID registered; install LibreOffice, open the file once, then re-run this script")
            continue
        }
        if (-not (Test-LibreOfficeProgId $progInfo.ProgId) -and -not $Force) {
            Write-Warn ("$ext is associated with '" + $progInfo.ProgId + "' which does not look like a LibreOffice ProgID; skipping (use -Force to override)")
            continue
        }
        $shellExPath = "HKCU:\Software\Classes\$($progInfo.ProgId)\ShellEx\$script:PreviewHandlerGuid"
        $previous = Get-RegValue -Path $shellExPath -Name ''

        $entry = $entries | Where-Object { $_.Extension -eq $ext } | Select-Object -First 1
        if (-not $entry) {
            $entry = [pscustomobject]@{
                Extension = $ext
                ProgId = $progInfo.ProgId
                ProgIdSource = $progInfo.Source
                PreviousHandler = $previous
                RegisteredHandler = $pdfHandler.Clsid
                RegisteredAt = (Get-Date).ToString('o')
            }
            $entries += $entry
        } else {
            # keep the *original* value so uninstall can always restore it
            if ($null -eq $entry.PreviousHandler) { $entry.PreviousHandler = $previous }
            $entry.RegisteredHandler = $pdfHandler.Clsid
            $entry.ProgId = $progInfo.ProgId
            $entry.RegisteredAt = (Get-Date).ToString('o')
        }
        try {
            Set-RegString -Path $shellExPath -Name $script:PreviewHandlerGuid -Value $pdfHandler.Clsid
            $check = Get-RegValue -Path $shellExPath -Name ''
            if ($check -ne $pdfHandler.Clsid) { throw "read-back mismatch ('$check')" }
            Write-Ok ("$ext -> $($progInfo.ProgId) -> " + $pdfHandler.Clsid)
            $registered++
        } catch {
            Write-Bad ("cannot register the preview handler for $ext : " + $_.Exception.Message)
        }
    }
    $backup.Entries = $entries
    Save-AssociationBackup $backup
    return $registered
}

function Restore-Associations {
    $backup = Get-AssociationBackup
    if (-not $backup.Entries -or @($backup.Entries).Count -eq 0) {
        Write-Warn 'no association backup found; nothing to restore'
        return 0
    }
    $restored = 0
    foreach ($entry in @($backup.Entries)) {
        $shellExPath = "HKCU:\Software\Classes\$($entry.ProgId)\ShellEx\$script:PreviewHandlerGuid"
        if ([string]::IsNullOrWhiteSpace($entry.PreviousHandler)) {
            Remove-RegValue -Path $shellExPath -Name $script:PreviewHandlerGuid
            Write-Ok ("$($entry.Extension): removed the preview handler registration (there was none before)")
        } else {
            Set-RegString -Path $shellExPath -Name $script:PreviewHandlerGuid -Value $entry.PreviousHandler
            Write-Ok ("$($entry.Extension): restored " + $entry.PreviousHandler)
        }
        $restored++
    }
    return $restored
}

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

function Main {
    Write-Host ''
    Write-Host ("LOPreview " + $script:Version + " - LibreOffice documents in the normal Explorer preview pane") -ForegroundColor White

    if (-not $Diagnose -and -not $RestoreAssociations) {
        Write-Head 'Directories and configuration'
        Initialize-Directories
    }

    Write-Head 'System'
    $win = Get-WindowsInfo
    Write-Info ("Windows      : " + $win.ProductName + " " + $win.DisplayVer + " (build " + $win.Build + "." + $win.UBR + ")")
    Write-Info ("Architecture : " + $(if ($win.Is64) { '64-bit' } else { '32-bit' }))
    Write-Info ("User         : " + $win.User + $(if ($win.IsAdmin) { ' (elevated)' } else { ' (standard)' }))
    Write-Info ("PowerShell   : " + $win.PSVersion)
    if ([int]$win.Build -lt 22000) {
        Write-Warn 'this mod is designed for Windows 11 (build 22000 or later)'
    }

    Write-Head 'LibreOffice'
    $soffice = Find-LibreOffice
    if (-not $soffice) {
        Write-Bad 'soffice.exe was not found. Install LibreOffice, or set soffice_path in config.ini.'
    } else {
        $version = (Get-Item -LiteralPath $soffice).VersionInfo.FileVersion
        Write-Ok ("soffice.exe   : " + $soffice)
        if ($version) { Write-Info ("file version  : " + $version) }
    }

    Write-Head 'PDF preview handler'
    $pdf = Get-PdfPreviewHandler
    if (-not $pdf) {
        Write-Bad ('no preview handler is registered for .pdf. Windows 11 normally has one via Microsoft Edge or Adobe Reader; without it there is nothing to render the converted PDF.')
    } else {
        Write-Ok ("handler CLSID : " + $pdf.Clsid)
        Write-Info ("discovered via: " + $pdf.Source)
        if ($pdf.Name) { Write-Info ("handler name  : " + $pdf.Name) }
    }

    Write-Head 'Windhawk'
    $windhawk = Find-Windhawk
    if (-not $windhawk.Exe) {
        Write-Warn 'Windhawk was not found under %ProgramFiles%\Windhawk'
    } else {
        Write-Ok ("windhawk.exe  : " + $windhawk.Exe)
    }
    if ($windhawk.Files.Count -eq 0) {
        Write-Info 'no local LOPreview mod files were found (this scan is best effort);'
        Write-Info 'compile them in Windhawk: Advanced -> Create new mod -> paste the .wh.cpp files'
    } else {
        foreach ($file in $windhawk.Files) { Write-Info ("mod file      : " + $file) }
    }

    if ($RestoreAssociations) {
        Write-Head 'Restoring the previous file associations'
        $count = Restore-Associations
        Write-Info ("$count extension(s) processed")
    } elseif (-not $Diagnose -and -not $NoAssociations) {
        Write-Head 'Preview handler associations'
        if (-not $pdf) {
            Write-Bad 'cannot register anything without a PDF preview handler'
        } else {
            $count = Register-Associations $pdf
            Write-Info ("$count extension(s) registered")
        }
    } elseif ($Diagnose) {
        Write-Head 'Preview handler associations (diagnose only, no changes)'
        foreach ($extension in $Extensions) {
            $progInfo = Get-ProgIdForExtension $extension
            if (-not $progInfo) {
                Write-Warn ("$extension has no ProgID")
                continue
            }
            $current = Get-RegValue -Path ("HKCU:\Software\Classes\$($progInfo.ProgId)\ShellEx\$script:PreviewHandlerGuid") -Name ''
            Write-Info ("$extension -> $($progInfo.ProgId) [$($progInfo.Source)] -> " + $(if ($current) { $current } else { '(no preview handler)' }))
        }
    }

    Write-Head 'Result'
    if ($script:Problems.Count -gt 0) {
        foreach ($p in $script:Problems) { Write-Host ("  problem: " + $p) -ForegroundColor Red }
    }
    if ($script:Warnings.Count -gt 0) {
        foreach ($w in $script:Warnings) { Write-Host ("  warning: " + $w) -ForegroundColor Yellow }
    }
    if ($script:Problems.Count -eq 0) { Write-Ok 'no blocking problems detected' }

    if (-not $Diagnose) {
        Write-Head 'Next steps'
        Write-Host @'
  1. Install the two mods in Windhawk (Windhawk -> Advanced -> Create new mod):
       lo-explorer-preview.wh.cpp   process inclusion list: prevhost.exe
       lo-preview-broker.wh.cpp     process inclusion list: explorer.exe
     Paste the whole file content, then press "Compile mod" and enable it.

  2. In each mod's settings make sure the process inclusion list contains the
     process above (not "*"), then restart Explorer (or sign out/in).

  3. Open Explorer, press Alt+P (preview pane), and select an .odt file.
     The first selection converts (a few seconds); later selections of the same
     unchanged file appear instantly from the cache.

  Diagnostics if nothing appears:
    %LOCALAPPDATA%\LOPreview\logs\lopreview.log     (broker + client attempts)
    %USERPROFILE%\AppData\LocalLow\LOPreview\prevhost.log  (low integrity client)
    Windhawk -> the mod -> "Show log"  (Wh_Log output from both processes)

  Registry changes made (all per user, reversible):
    HKCU\Software\Classes\<LibreOffice ProgID>\ShellEx\{8895b1c6-b41f-4c1c-a562-0d564250836f}
    = the PDF preview handler CLSID
  Undo with:  .\uninstall-user.ps1   (or  .\install-user.ps1 -RestoreAssociations)
'@
    }
}

try {
    Main
} catch {
    Write-Host ''
    Write-Host 'LOPreview installer stopped with an unexpected error:' -ForegroundColor Red
    Write-Host ("  " + $_.Exception.Message) -ForegroundColor Red
    Write-Host ("  at " + $_.InvocationInfo.PositionMessage) -ForegroundColor DarkGray
    if ($_.ScriptStackTrace) { Write-Host $_.ScriptStackTrace -ForegroundColor DarkGray }
    exit 1
}

if ($script:Problems.Count -gt 0) { exit 2 }
if ($Pause) { Write-Host ''; Read-Host 'Press Enter to close' }
exit 0
