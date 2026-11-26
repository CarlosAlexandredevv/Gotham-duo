<#
.SYNOPSIS
    Installs or uninstalls Gotham Duo for LEGO Batman: Legacy of the Dark Knight (Steam / Epic).

.DESCRIPTION
    Auto-detects the game's Binaries\Win64 folder (Steam libraries and Epic manifests), verifies
    LEGOBatmanLotDK-Win64-Shipping.exe exists there, then copies version.dll. An existing
    gothamduo.ini is never overwritten unless -UpdateConfig is given (a .bak copy is kept).
    A version.dll that is not Gotham Duo (another mod's proxy) is never overwritten or removed.

.EXAMPLE
    .\install.ps1                                   # interactive menu
    .\install.ps1 -Action Install -GamePath "D:\Games\LEGO Batman"
    .\install.ps1 -Action Uninstall -RemoveConfig -NoPrompt
#>
[CmdletBinding()]
param(
    [ValidateSet('Install', 'Uninstall')][string]$Action,
    [string]$GamePath,
    [string]$SourceDir = $PSScriptRoot,
    [switch]$UpdateConfig,
    [switch]$RemoveConfig,
    [switch]$NoPrompt
)

$ErrorActionPreference = 'Stop'
$script:ExeName = 'LEGOBatmanLotDK-Win64-Shipping.exe'
$script:RelBin = 'LEGOBatmanLotDK\Binaries\Win64'
$script:ProcessName = 'LEGOBatmanLotDK-Win64-Shipping'

function Resolve-BinDir {
    # Accepts the game root, the LEGOBatmanLotDK folder, or Binaries\Win64 itself.
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }
    $Path = $Path.Trim().Trim('"')
    $candidates = @(
        $Path,
        (Join-Path $Path $script:RelBin),
        (Join-Path $Path 'Binaries\Win64')
    )
    foreach ($c in $candidates) {
        if (Test-Path -LiteralPath (Join-Path $c $script:ExeName) -PathType Leaf) {
            return (Resolve-Path -LiteralPath $c).ProviderPath
        }
    }
    return $null
}

function Get-VdfLibraryPaths {
    # Extracts every "path" "<dir>" entry from Steam's libraryfolders.vdf.
    param([string]$VdfText)
    $paths = @()
    foreach ($m in [regex]::Matches($VdfText, '"path"\s+"([^"]+)"')) {
        $paths += ($m.Groups[1].Value -replace '\\\\', '\')
    }
    return $paths
}

function Get-SteamLibraryRoots {
    $roots = @()
    $steamDirs = @()
    foreach ($key in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam') {
        try {
            $p = Get-ItemProperty -Path $key -ErrorAction Stop
            if ($p.SteamPath) { $steamDirs += ($p.SteamPath -replace '/', '\') }
            if ($p.InstallPath) { $steamDirs += $p.InstallPath }
        } catch { }
    }
    foreach ($steam in ($steamDirs | Select-Object -Unique)) {
        $roots += $steam
        $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf) {
            $roots += Get-VdfLibraryPaths -VdfText (Get-Content -LiteralPath $vdf -Raw)
        }
    }
    return $roots | Select-Object -Unique
}

function Get-EpicInstallLocations {
    $dir = Join-Path $env:ProgramData 'Epic\EpicGamesLauncher\Data\Manifests'
    if (-not (Test-Path -LiteralPath $dir)) { return @() }
    $out = @()
    foreach ($f in Get-ChildItem -LiteralPath $dir -Filter '*.item' -ErrorAction SilentlyContinue) {
        try {
            $item = Get-Content -LiteralPath $f.FullName -Raw | ConvertFrom-Json
            if ($item.InstallLocation) { $out += $item.InstallLocation }
        } catch { }
    }
    return $out
}

function Find-GameBinDirs {
    $found = @()
    foreach ($root in Get-SteamLibraryRoots) {
        $common = Join-Path $root 'steamapps\common'
        if (-not (Test-Path -LiteralPath $common)) { continue }
        foreach ($d in Get-ChildItem -LiteralPath $common -Directory -ErrorAction SilentlyContinue) {
            $bin = Resolve-BinDir $d.FullName
            if ($bin) { $found += $bin }
        }
    }
    foreach ($loc in Get-EpicInstallLocations) {
        $bin = Resolve-BinDir $loc
        if ($bin) { $found += $bin }
    }
    return $found | Select-Object -Unique
}

function Test-IsGothamDuoDll {
    param([string]$Path)
    try { return (Get-Item -LiteralPath $Path).VersionInfo.ProductName -eq 'Gotham Duo' }
    catch { return $false }
}

function Find-SourceFile {
    param([string]$Dir, [string]$Name, [string[]]$Fallbacks)
    foreach ($p in @((Join-Path $Dir $Name)) + ($Fallbacks | ForEach-Object { Join-Path $Dir $_ })) {
        if (Test-Path -LiteralPath $p -PathType Leaf) { return $p }
    }
    return $null
}

function Install-GothamDuo {
    param([string]$BinDir, [string]$Source, [switch]$UpdateConfig)
    $dll = Find-SourceFile $Source 'version.dll' @('build\Release\version.dll', '..\build\Release\version.dll')
    if (-not $dll) { throw "version.dll not found next to the installer (looked in '$Source')." }
    $ini = Find-SourceFile $Source 'gothamduo.ini' @('config\gothamduo.ini', '..\config\gothamduo.ini')

    $destDll = Join-Path $BinDir 'version.dll'
    if ((Test-Path -LiteralPath $destDll) -and -not (Test-IsGothamDuoDll $destDll)) {
        throw "A different version.dll already exists in '$BinDir' (another mod?). Refusing to overwrite it."
    }
    Copy-Item -LiteralPath $dll -Destination $destDll -Force
    Write-Host "Installed version.dll -> $BinDir"

    if ($ini) {
        $destIni = Join-Path $BinDir 'gothamduo.ini'
        if (-not (Test-Path -LiteralPath $destIni)) {
            Copy-Item -LiteralPath $ini -Destination $destIni
            Write-Host 'Installed default gothamduo.ini'
        } elseif ($UpdateConfig) {
            Copy-Item -LiteralPath $destIni -Destination "$destIni.bak" -Force
            Copy-Item -LiteralPath $ini -Destination $destIni -Force
            Write-Host 'Updated gothamduo.ini (previous copy saved as gothamduo.ini.bak)'
        } else {
            Write-Host 'Kept your existing gothamduo.ini (use -UpdateConfig to replace it).'
        }
    }
}

function Uninstall-GothamDuo {
    param([string]$BinDir, [switch]$RemoveConfig)
    $destDll = Join-Path $BinDir 'version.dll'
    if (Test-Path -LiteralPath $destDll) {
        if (Test-IsGothamDuoDll $destDll) {
            Remove-Item -LiteralPath $destDll -Force
            Write-Host 'Removed version.dll'
        } else {
            Write-Warning "version.dll in '$BinDir' is not Gotham Duo; leaving it untouched."
        }
    }
    $log = Join-Path $BinDir 'gothamduo.log'
    if (Test-Path -LiteralPath $log) { Remove-Item -LiteralPath $log -Force; Write-Host 'Removed gothamduo.log' }
    $ini = Join-Path $BinDir 'gothamduo.ini'
    if ($RemoveConfig -and (Test-Path -LiteralPath $ini)) {
        Remove-Item -LiteralPath $ini -Force
        Write-Host 'Removed gothamduo.ini'
    } elseif (Test-Path -LiteralPath $ini) {
        Write-Host 'Kept gothamduo.ini (use -RemoveConfig to delete it).'
    }
}

function Select-BinDir {
    param([string]$ExplicitPath, [switch]$NoPrompt)
    if ($ExplicitPath) {
        $bin = Resolve-BinDir $ExplicitPath
        if (-not $bin) { throw "'$ExplicitPath' does not contain $script:ExeName (expected ...\$script:RelBin)." }
        return $bin
    }
    $found = @(Find-GameBinDirs)
    if ($found.Count -eq 1) { Write-Host "Detected game: $($found[0])"; return $found[0] }
    if ($NoPrompt) {
        if ($found.Count -eq 0) { throw 'Game not found. Pass -GamePath "<install folder>".' }
        throw 'Multiple installs found. Pass -GamePath to choose one.'
    }
    if ($found.Count -gt 1) {
        for ($i = 0; $i -lt $found.Count; $i++) { Write-Host "  [$($i + 1)] $($found[$i])" }
        $pick = Read-Host 'Choose an install number (or paste a folder path)'
        if ($pick -match '^\d+$' -and [int]$pick -ge 1 -and [int]$pick -le $found.Count) { return $found[[int]$pick - 1] }
        $bin = Resolve-BinDir $pick
        if ($bin) { return $bin }
        throw 'Invalid selection.'
    }
    $bin = Resolve-BinDir (Read-Host 'Game not found automatically. Paste the game install folder')
    if (-not $bin) { throw "That folder does not contain $script:ExeName." }
    return $bin
}

function Invoke-Main {
    if (Get-Process -Name $script:ProcessName -ErrorAction SilentlyContinue) {
        throw 'The game is running. Close it first so version.dll is not locked.'
    }
    $act = $Action
    if (-not $act) {
        if ($NoPrompt) { throw 'Specify -Action Install or -Action Uninstall.' }
        $choice = Read-Host 'Gotham Duo: [1] Install  [2] Uninstall'
        $act = if ($choice -eq '2') { 'Uninstall' } else { 'Install' }
    }
    $bin = Select-BinDir -ExplicitPath $GamePath -NoPrompt:$NoPrompt
    if ($act -eq 'Install') {
        Install-GothamDuo -BinDir $bin -Source $SourceDir -UpdateConfig:$UpdateConfig
    } else {
        $remove = $RemoveConfig
        if (-not $remove -and -not $NoPrompt -and (Test-Path -LiteralPath (Join-Path $bin 'gothamduo.ini'))) {
            $remove = (Read-Host 'Also delete gothamduo.ini? [y/N]') -match '^(y|yes)$'
        }
        Uninstall-GothamDuo -BinDir $bin -RemoveConfig:$remove
    }
    Write-Host 'Done.'
}

# Dot-sourcing (tests) only loads the functions.
if ($MyInvocation.InvocationName -ne '.') {
    try { Invoke-Main } catch { Write-Error $_.Exception.Message; exit 1 }
}
