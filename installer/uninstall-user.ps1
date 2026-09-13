<#
    LOPreview - per-user uninstaller
    =================================

    Restores the preview-handler associations that install-user.ps1 changed and
    (optionally) removes the generated data folders.  The two Windhawk mods must
    be removed in Windhawk itself - this script prints a reminder.

    Usage (from an existing PowerShell window):

        powershell -ExecutionPolicy Bypass -File .\uninstall-user.ps1
        powershell -ExecutionPolicy Bypass -File .\uninstall-user.ps1 -RemoveData
        powershell -ExecutionPolicy Bypass -File .\uninstall-user.ps1 -KeepAssociations
#>

[CmdletBinding()]
param(
    [switch]$RemoveData,
    [switch]$KeepAssociations,
    [switch]$Pause
)

$ErrorActionPreference = 'Stop'
$script:Version = '0.5.3'
$script:PreviewHandlerGuid = '{8895b1c6-b41f-4c1c-a562-0d564250836f}'
$script:Problems = New-Object System.Collections.Generic.List[string]

function Write-Head([string]$text) {
    Write-Host ''
    Write-Host ("=== " + $text + " " + ('=' * [Math]::Max(0, 66 - $text.Length))) -ForegroundColor Cyan
}
function Write-Ok([string]$t)   { Write-Host ("  [ok]   " + $t) -ForegroundColor Green }
function Write-Info([string]$t) { Write-Host ("  [info] " + $t) }
function Write-Warn([string]$t) { Write-Host ("  [warn] " + $t) -ForegroundColor Yellow }
function Write-Bad([string]$t) {
    Write-Host ("  [FAIL] " + $t) -ForegroundColor Red
    $script:Problems.Add($t)
}

function Get-DataRoot { Join-Path $env:LOCALAPPDATA 'LOPreview' }
function Get-LowRoot  { Join-Path (Join-Path $env:USERPROFILE 'AppData') 'LocalLow\LOPreview' }
function Get-BackupPath { Join-Path (Get-DataRoot) 'preview-associations.json' }

# Default (unnamed) registry values are what the shell reads; the .NET
# RegistryKey API handles them correctly, including deletion.
function Open-HkcuKey([string]$SubPath, [bool]$Writable, [bool]$Create = $false) {
    if ($Create) {
        return [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey(
            $SubPath, [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree)
    }
    return [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($SubPath, $Writable)
}
function Set-HkcuDefault {
    param([string]$SubPath, [string]$Value)
    $key = Open-HkcuKey $SubPath $true $true
    if (-not $key) { throw "cannot open/create HKCU\$SubPath" }
    try { $key.SetValue('', $Value, [Microsoft.Win32.RegistryValueKind]::String) }
    finally { $key.Close() }
}
function Remove-HkcuValue {
    param([string]$SubPath, [string]$Name)
    $key = Open-HkcuKey $SubPath $true
    if ($key) {
        try { $key.DeleteValue($Name, $false) } catch { } finally { $key.Close() }
    }
}
function Get-HandlerSubPath([string]$ProgId) {
    return "Software\Classes\$ProgId\ShellEx\$script:PreviewHandlerGuid"
}

function Get-RegValue {
    param([string]$Path, [string]$Name)
    try {
        $item = Get-ItemProperty -Path $Path -ErrorAction Stop
        if ($null -eq $item) { return $null }
        if ([string]::IsNullOrEmpty($Name)) {
            return (Get-Item -Path $Path -ErrorAction Stop).GetValue('')
        }
        $prop = $item.PSObject.Properties[$Name]
        if ($null -eq $prop) { return $null }
        return $prop.Value
    } catch { return $null }
}

function Restore-Associations {
    $path = Get-BackupPath
    if (-not (Test-Path -LiteralPath $path)) {
        Write-Warn 'no association backup found - nothing to restore'
        return
    }
    $backup = $null
    try {
        $backup = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    } catch {
        Write-Bad ("the association backup is unreadable: " + $_.Exception.Message)
        return
    }
    $entries = @($backup.Entries)
    if ($entries.Count -eq 0) {
        Write-Warn 'the association backup contains no entries'
        return
    }
    foreach ($entry in $entries) {
        $shellExPath = "HKCU:\Software\Classes\$($entry.ProgId)\ShellEx\$script:PreviewHandlerGuid"
        $subPath = Get-HandlerSubPath $entry.ProgId
        try {
            if ([string]::IsNullOrWhiteSpace($entry.PreviousHandler)) {
                # Remove the default value (what the shell uses) and the stray
                # named value left by builds 0.5.0/0.5.1, if present.
                Remove-HkcuValue -SubPath $subPath -Name ''
                Remove-HkcuValue -SubPath $subPath -Name $script:PreviewHandlerGuid
                Write-Ok ("$($entry.Extension): preview handler registration removed (there was none before LOPreview)")
            } else {
                Remove-HkcuValue -SubPath $subPath -Name $script:PreviewHandlerGuid
                Set-HkcuDefault -SubPath $subPath -Value $entry.PreviousHandler
                Write-Ok ("$($entry.Extension): restored " + $entry.PreviousHandler)
            }
            $entry.Restored = $true
            $entry.RestoredAt = (Get-Date).ToString('o')
        } catch {
            Write-Bad ("could not restore $($entry.Extension): " + $_.Exception.Message)
        }
    }
    try {
        $utf8 = New-Object System.Text.UTF8Encoding($false)
        [System.IO.File]::WriteAllText($path, ($backup | ConvertTo-Json -Depth 6), $utf8)
    } catch { }
}

Write-Host ''
Write-Host ("LOPreview " + $script:Version + " uninstaller") -ForegroundColor White

if (-not $KeepAssociations) {
    Write-Head 'Restoring file associations'
    Restore-Associations
} else {
    Write-Info 'file associations left untouched (-KeepAssociations)'
}

if ($RemoveData) {
    Write-Head 'Removing data folders'
    foreach ($dir in @((Get-DataRoot), (Get-LowRoot))) {
        if (Test-Path -LiteralPath $dir) {
            try {
                Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction Stop
                Write-Ok ("removed " + $dir)
            } catch {
                Write-Warn ("could not remove " + $dir + ": " + $_.Exception.Message +
                            " (a preview may still be open - close Explorer and retry)")
            }
        } else {
            Write-Info ("not present: " + $dir)
        }
    }
} else {
    Write-Head 'Data folders'
    Write-Info ("kept: " + (Get-DataRoot) + "   (use -RemoveData to delete)")
    Write-Info ("kept: " + (Get-LowRoot))
}

Write-Head 'Still to do manually'
Write-Host @'
  * Remove or disable the two mods in Windhawk:
        lo-explorer-preview   (prevhost.exe)
        lo-preview-broker     (explorer.exe)
  * Restart Explorer (or sign out and back in) so the preview pane stops using
    the previously registered handler.

  Note: the registry values restored here are per user and only affect the
  preview pane.  The default "open with" application was never changed.
'@

Write-Host ''
if ($script:Problems.Count -gt 0) {
    Write-Host ("finished with " + $script:Problems.Count + " problem(s)") -ForegroundColor Yellow
    if ($Pause) { Read-Host 'Press Enter to close' }
    exit 2
}
Write-Host 'uninstall complete' -ForegroundColor Green
if ($Pause) { Read-Host 'Press Enter to close' }
exit 0
