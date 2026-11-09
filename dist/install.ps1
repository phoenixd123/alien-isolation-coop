<#
Alien: Isolation co-op: install / uninstall.

  install:    powershell -ExecutionPolicy Bypass -File install.ps1 [-GameDir "<game dir>"] [-Skins]
  uninstall:  powershell -ExecutionPolicy Bypass -File install.ps1 -Uninstall [-GameDir "<game dir>"]

What it does (install):
  1. Finds the game (Steam library folders) unless -GameDir is given.
  2. Refuses to continue unless AI.exe is the tested build (SHA-256 below), unless -Force.
  3. Backs up the original steam_api.dll as steam_api_real.dll (the wrapper forwards to it).
  4. Copies steam_api.dll (wrapper) and coopnet.dll next to AI.exe, writes steam_appid.txt.
  5. Copies (never moves) your existing Steam saves into <game dir>\coop_saves\default, so co-op
     starts where you are. Co-op progress stays there and never touches your Steam Cloud saves.
  6. Copies lobby.exe (the co-op launcher) and play-coop.bat into the game folder.
  7. If v0.2.0 / v0.3.0's -Skins changed your level files, puts the originals back (those levels didn't load:
     CathodeLib rewrote the level's model files).
  8. With -Skins (needs the .NET 8 runtime): runs skinprep, which copies five story characters' models into every
     level so your partner can show up as one. It keeps each level's model files byte for byte and adds the new
     models at the end. Changed files are backed up to <game dir>\coop_backup\skins first.
Uninstall puts the original steam_api.dll and level files back and removes the added files. coop_saves is kept.
#>
param([string]$GameDir, [switch]$Uninstall, [switch]$Force, [switch]$NoSaveImport, [switch]$Skins)
$ErrorActionPreference = "Stop"
$TestedExe = "DDE66521798AAE8C39ED4E7A3400F561FCECFFF83619D1426B9E0CEDF5819A90"   # Steam build 4940658
$here = $PSScriptRoot

function Find-Game {
    $steam = (Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction SilentlyContinue).SteamPath
    if (-not $steam) { return $null }
    $libs = @($steam)
    $vdf = Join-Path $steam "steamapps\libraryfolders.vdf"
    if (Test-Path $vdf) { $libs += (Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"').Matches | ForEach-Object { $_.Groups[1].Value -replace '\\\\', '\' } }
    foreach ($l in $libs | Select-Object -Unique) {
        $acf = Join-Path $l "steamapps\appmanifest_214490.acf"
        if (Test-Path $acf) {
            $dir = (Select-String -Path $acf -Pattern '"installdir"\s+"([^"]+)"').Matches[0].Groups[1].Value
            $p = Join-Path $l "steamapps\common\$dir"
            if (Test-Path (Join-Path $p "AI.exe")) { return $p }
        }
    }
    return $null
}

if (-not $GameDir) { $GameDir = Find-Game }
if (-not $GameDir -or -not (Test-Path (Join-Path $GameDir "AI.exe"))) { throw "Game not found. Pass -GameDir `"<folder that contains AI.exe>`"." }
$GameDir = (Resolve-Path $GameDir).Path
"Game folder: $GameDir"
if (Get-Process AI -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$GameDir*" }) { throw "Close the game first." }

$api = Join-Path $GameDir "steam_api.dll"; $real = Join-Path $GameDir "steam_api_real.dll"
# Put level files back from a backup folder (<LEVEL>\... per level). A ".new" marker means the file didn't exist before:
# delete it. "levels" holds v0.2.0 / v0.3.0 -Skins backups (their levels crash, always restored); "skins" holds the
# current skinprep's (restored on uninstall).
function Restore-Levels([string]$GameDir, [string]$Folder = "levels") {
    $bk = Join-Path $GameDir ("coop_backup\" + $Folder)
    if (-not (Test-Path $bk)) { return }
    $prod = Join-Path $GameDir "DATA\ENV\PRODUCTION"
    $n = 0
    foreach ($lvDir in Get-ChildItem $bk -Directory) {
        $target = Join-Path $prod $lvDir.Name
        if (-not (Test-Path $target) -and $lvDir.Name -like "DLC_*") { $target = Join-Path $prod ("DLC\" + $lvDir.Name.Substring(4)) }
        if (-not (Test-Path $target)) { "  can't find the level folder for $($lvDir.Name), left as is"; continue }
        foreach ($f in Get-ChildItem $lvDir.FullName -Recurse -File) {
            $rel = $f.FullName.Substring($lvDir.FullName.Length + 1)
            if ($rel.EndsWith(".new")) { $t = Join-Path $target $rel.Substring(0, $rel.Length - 4); if (Test-Path $t) { Remove-Item $t } }
            else { Copy-Item $f.FullName (Join-Path $target $rel) -Force }
            $n++
        }
    }
    Rename-Item $bk ("$Folder-restored-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
    $sk = Join-Path $GameDir "coop_skins.txt"; if (Test-Path $sk) { Remove-Item $sk }
    "restored $n original level files that -Skins had changed"
}
$added = "coopnet.dll", "steam_appid.txt", "play-coop.bat", "lobby.exe", "lobby.ini"

if ($Uninstall) {
    if (Test-Path $real) { Move-Item $real $api -Force; "restored original steam_api.dll" } else { "no steam_api_real.dll found; steam_api.dll left as is" }
    Restore-Levels $GameDir
    Restore-Levels $GameDir "skins"
    foreach ($f in $added) { $p = Join-Path $GameDir $f; if (Test-Path $p) { Remove-Item $p; "removed $f" } }
    "Uninstalled. Your co-op saves are still in $GameDir\coop_saves (delete it yourself if you don't want them)."
    return
}

$hash = (Get-FileHash (Join-Path $GameDir "AI.exe") -Algorithm SHA256).Hash
if ($hash -ne $TestedExe -and -not $Force) { throw "AI.exe is a different build ($hash). This mod is only tested on Steam build 4940658. Use -Force to try anyway." }

foreach ($f in "steam_api.dll", "coopnet.dll", "play-coop.bat", "lobby.exe") { if (-not (Test-Path (Join-Path $here $f))) { throw "missing $f next to install.ps1" } }

# Back up the original once. If steam_api_real.dll exists we're re-installing over our own wrapper.
if (-not (Test-Path $real)) { Copy-Item $api $real; "backed up original steam_api.dll -> steam_api_real.dll" }
Copy-Item (Join-Path $here "steam_api.dll") $api -Force
Copy-Item (Join-Path $here "coopnet.dll") (Join-Path $GameDir "coopnet.dll") -Force
Copy-Item (Join-Path $here "play-coop.bat") (Join-Path $GameDir "play-coop.bat") -Force
Copy-Item (Join-Path $here "lobby.exe") (Join-Path $GameDir "lobby.exe") -Force
Set-Content -Path (Join-Path $GameDir "steam_appid.txt") -Value "214490" -Encoding ascii -NoNewline
"installed wrapper, coopnet.dll, steam_appid.txt, play-coop.bat, lobby.exe"

$saves = Join-Path $GameDir "coop_saves\default"
if (-not $NoSaveImport -and -not (Test-Path $saves)) {
    $steam = (Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction SilentlyContinue).SteamPath
    $src = if ($steam) { Get-ChildItem (Join-Path $steam "userdata\*\214490\remote") -Directory -ErrorAction SilentlyContinue | Select-Object -First 1 }
    New-Item -ItemType Directory -Force $saves | Out-Null
    if ($src) { Copy-Item (Join-Path $src.FullName "*") $saves; "copied your Steam saves into coop_saves\default (originals untouched)" }
    else { "no Steam saves found; co-op starts a new game" }
}
Restore-Levels $GameDir
$skinprep = Join-Path $here "skinprep\skinprep.exe"
function Have-Net8 { try { return [bool]((& dotnet --list-runtimes 2>$null) -match "^Microsoft\.NETCore\.App 8\.") } catch { return $false } }
if ($Skins) {
    if (-not (Test-Path $skinprep)) { "skins: skinprep isn't in this package, skipped" }
    elseif (-not (Have-Net8)) { "skins: needs the .NET 8 runtime (https://dotnet.microsoft.com/download/dotnet/8.0), skipped. Run the installer again with -Skins after installing it." }
    else {
        "skins: copying character models into every level (a few minutes, originals backed up to coop_backup\skins)"
        & $skinprep $GameDir
        if ($LASTEXITCODE -eq 3) { "skins: some levels failed and were left unchanged; the others are done" }
        elseif ($LASTEXITCODE) { "skins: skinprep stopped (code $LASTEXITCODE); your level files are unchanged or restored per level" }
    }
}
"Done. Start co-op with lobby.exe in the game folder (or play-coop.bat, see README)."
