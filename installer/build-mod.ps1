<#
    LOPreview - mod assembler (PowerShell version of tools/assemble.py)

    Windhawk compiles exactly one file, so the shared sources in src/ have to be
    inlined into lo-explorer-preview.wh.cpp and lo-preview-broker.wh.cpp.
    Run this after editing anything in src/.

    Usage:
        powershell -ExecutionPolicy Bypass -File .\build-mod.ps1
        powershell -ExecutionPolicy Bypass -File .\build-mod.ps1 -Check
        powershell -ExecutionPolicy Bypass -File .\build-mod.ps1 -OutDir C:\temp
#>

[CmdletBinding()]
param(
    [string]$OutDir,
    [switch]$Check,
    [switch]$Pause
)

$ErrorActionPreference = 'Stop'
$installerDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $installerDir
$srcDir = Join-Path $repoRoot 'src'
if (-not $OutDir) { $OutDir = $repoRoot }

$roles = @(
    @{ Role = 'prevhost-mod.wh.cpp'; Out = 'lo-explorer-preview.wh.cpp' },
    @{ Role = 'broker-mod.wh.cpp';   Out = 'lo-preview-broker.wh.cpp' }
)
$headers = @{
    'lop_core.h' = @{ File = (Join-Path $srcDir 'lop_core.h'); Guard = 'LOPREVIEW_CORE_H' }
    'lop_win.h'  = @{ File = (Join-Path $srcDir 'lop_win.h');  Guard = 'LOPREVIEW_WIN_H' }
}

function Read-Text([string]$Path) {
    return [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
}

function Write-Text([string]$Path, [string]$Text) {
    $utf8 = New-Object System.Text.UTF8Encoding($false)   # no BOM
    [System.IO.File]::WriteAllText($Path, $Text, $utf8)
}

function Remove-Guard([string]$Text, [string]$Guard) {
    $lines = $Text -split "`n"
    $kept = foreach ($line in $lines) {
        $t = $line.Trim()
        if ($t -eq "#ifndef $Guard" -or $t -eq "#define $Guard") { continue }
        if ($t.StartsWith('#endif') -and $t.Contains($Guard)) { continue }
        $line
    }
    return ($kept -join "`n")
}

function Remove-InternalIncludes([string]$Text) {
    return [regex]::Replace($Text, '(?m)^[ \t]*#include\s+"lop_[a-z_]+\.h"[^\n]*\n', '')
}

function Get-IncludeNames([string]$Text) {
    return [regex]::Matches($Text, '(?m)^\s*#include\s+"(lop_[a-z_]+\.h)"') |
        ForEach-Object { $_.Groups[1].Value }
}

function Assemble([string]$RoleFile) {
    $rolePath = Join-Path $srcDir $RoleFile
    if (-not (Test-Path -LiteralPath $rolePath)) { throw "missing $rolePath" }
    $text = Read-Text $rolePath
    $banner = @"
// ---------------------------------------------------------------------------
// GENERATED FILE - do not edit here.
// Assembled by installer/build-mod.ps1 from src/$RoleFile, src/lop_core.h and
// src/lop_win.h.  Edit those files and re-run the assembler instead.
// ---------------------------------------------------------------------------
"@
    $out = New-Object System.Text.StringBuilder
    $inserted = @{}
    foreach ($line in ($text -split "`n")) {
        $m = [regex]::Match($line, '^\s*#include\s+"(lop_[a-z_]+\.h)"')
        if (-not $m.Success) { [void]$out.Append($line).Append("`n"); continue }
        $name = $m.Groups[1].Value
        if ($inserted.ContainsKey($name)) { continue }
        $deps = @(Get-IncludeNames (Read-Text $headers[$name].File)) + @($name)
        foreach ($dep in $deps) {
            if ($inserted.ContainsKey($dep)) { continue }
            if (-not $headers.ContainsKey($dep)) { throw "unknown internal header $dep" }
            $inserted[$dep] = $true
            $body = Read-Text $headers[$dep].File
            $body = Remove-Guard $body $headers[$dep].Guard
            $body = Remove-InternalIncludes $body
            [void]$out.Append("`n// ==== inlined from src/$dep ====`n").Append($body)
        }
    }
    $body = $out.ToString()
    $marker = "// ==/WindhawkMod==`n"
    $index = $body.IndexOf($marker)
    if ($index -ge 0) {
        $body = $body.Substring(0, $index + $marker.Length) + "`n" + $banner + $body.Substring($index + $marker.Length)
    } else {
        $body = $banner + $body
    }
    if ($body -match '#include\s+"lop_') { throw "internal include left in $RoleFile output" }
    return $body
}

try {
    foreach ($role in $roles) {
        $body = Assemble $role.Role
        $target = Join-Path $OutDir $role.Out
        if ($Check) {
            $current = if (Test-Path -LiteralPath $target) { Read-Text $target } else { '' }
            if ($current -ne $body) {
                Write-Host ("stale: " + $role.Out) -ForegroundColor Yellow
                exit 1
            }
            Write-Host ("up to date: " + $role.Out) -ForegroundColor Green
        } else {
            Write-Text $target $body
            Write-Host ("wrote " + $target + " (" + (($body -split "`n").Count) + " lines)") -ForegroundColor Green
        }
    }
} catch {
    Write-Host ("assembly failed: " + $_.Exception.Message) -ForegroundColor Red
    if ($Pause) { Read-Host 'Press Enter to close' }
    exit 1
}
if ($Pause) { Read-Host 'Press Enter to close' }
exit 0
