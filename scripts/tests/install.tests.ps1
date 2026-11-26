# Self-check for scripts/install.ps1 (no frameworks): run with
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/tests/install.tests.ps1
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\install.ps1')

$script:failed = 0
function Assert([bool]$cond, [string]$msg) {
    if (-not $cond) { Write-Host "FAIL: $msg"; $script:failed++ } else { Write-Host "ok:   $msg" }
}

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("gd-install-test-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    # Fake game + release payload.
    $game = Join-Path $tmp 'Game'
    $bin = Join-Path $game 'LEGOBatmanLotDK\Binaries\Win64'
    New-Item -ItemType Directory -Path $bin -Force | Out-Null
    Set-Content (Join-Path $bin 'LEGOBatmanLotDK-Win64-Shipping.exe') 'exe'
    $src = Join-Path $tmp 'payload'
    New-Item -ItemType Directory -Path $src | Out-Null
    Set-Content (Join-Path $src 'version.dll') 'dll-v1'
    Set-Content (Join-Path $src 'gothamduo.ini') 'default-ini'

    # Treat any dll whose text starts with "dll-" as ours (real builds are checked by ProductName).
    function Test-IsGothamDuoDll([string]$Path) { (Get-Content -LiteralPath $Path -Raw) -like 'dll-*' }

    # Folder resolution from root / game folder / bin folder; rejects wrong folders.
    $expect = (Resolve-Path $bin).ProviderPath
    Assert ((Resolve-BinDir $game) -eq $expect) 'resolves from install root'
    Assert ((Resolve-BinDir (Join-Path $game 'LEGOBatmanLotDK')) -eq $expect) 'resolves from LEGOBatmanLotDK'
    Assert ((Resolve-BinDir $bin) -eq $expect) 'resolves from Binaries\Win64'
    Assert ($null -eq (Resolve-BinDir $tmp)) 'rejects folder without the shipping exe'

    # Steam libraryfolders.vdf parsing (escaped backslashes).
    $vdf = '"libraryfolders" { "0" { "path" "C:\\Program Files (x86)\\Steam" } "1" { "path" "D:\\Games\\SteamLibrary" } }'
    $paths = @(Get-VdfLibraryPaths -VdfText $vdf)
    Assert ($paths.Count -eq 2 -and $paths[1] -eq 'D:\Games\SteamLibrary') 'parses Steam library paths'

    # Fresh install copies dll and default ini.
    Install-GothamDuo -BinDir $bin -Source $src 6>$null
    Assert ((Get-Content (Join-Path $bin 'version.dll') -Raw).Trim() -eq 'dll-v1') 'installs version.dll'
    Assert ((Get-Content (Join-Path $bin 'gothamduo.ini') -Raw).Trim() -eq 'default-ini') 'installs default ini'

    # Reinstall must preserve user edits, upgrade the dll.
    Set-Content (Join-Path $bin 'gothamduo.ini') 'user-edited'
    Set-Content (Join-Path $src 'version.dll') 'dll-v2'
    Install-GothamDuo -BinDir $bin -Source $src 6>$null
    Assert ((Get-Content (Join-Path $bin 'gothamduo.ini') -Raw).Trim() -eq 'user-edited') 'reinstall keeps user ini'
    Assert ((Get-Content (Join-Path $bin 'version.dll') -Raw).Trim() -eq 'dll-v2') 'reinstall upgrades dll'

    # -UpdateConfig replaces the ini but keeps a backup.
    Install-GothamDuo -BinDir $bin -Source $src -UpdateConfig 6>$null
    Assert ((Get-Content (Join-Path $bin 'gothamduo.ini') -Raw).Trim() -eq 'default-ini') 'UpdateConfig replaces ini'
    Assert ((Get-Content (Join-Path $bin 'gothamduo.ini.bak') -Raw).Trim() -eq 'user-edited') 'UpdateConfig keeps .bak'

    # Uninstall keeps ini by default, removes dll + log; -RemoveConfig deletes ini.
    Set-Content (Join-Path $bin 'gothamduo.log') 'log'
    Uninstall-GothamDuo -BinDir $bin 6>$null
    Assert (-not (Test-Path (Join-Path $bin 'version.dll'))) 'uninstall removes dll'
    Assert (-not (Test-Path (Join-Path $bin 'gothamduo.log'))) 'uninstall removes log'
    Assert (Test-Path (Join-Path $bin 'gothamduo.ini')) 'uninstall keeps ini by default'
    Uninstall-GothamDuo -BinDir $bin -RemoveConfig 6>$null
    Assert (-not (Test-Path (Join-Path $bin 'gothamduo.ini'))) '-RemoveConfig deletes ini'

    # A foreign version.dll (another mod) is never overwritten or removed.
    Set-Content (Join-Path $bin 'version.dll') 'OTHER-MOD'
    $threw = $false
    try { Install-GothamDuo -BinDir $bin -Source $src 6>$null } catch { $threw = $true }
    Assert $threw 'install refuses to overwrite a foreign version.dll'
    Uninstall-GothamDuo -BinDir $bin 3>$null 6>$null
    Assert ((Get-Content (Join-Path $bin 'version.dll') -Raw).Trim() -eq 'OTHER-MOD') 'uninstall leaves a foreign version.dll'
}
finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

if ($script:failed -gt 0) { Write-Host "$script:failed check(s) failed."; exit 1 }
Write-Host 'All install.ps1 checks passed.'
