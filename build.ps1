# Builds steamwrap (steam_api.dll) and coopnet.dll, runs every offline test, and assembles the player package.
# Needs Zig 0.16.0 (https://ziglang.org/download/). Pass its path with -Zig, or have zig on PATH.
# With the .NET 8 SDK (-Dotnet, or dotnet on PATH) it also publishes tools/skinprep into the package.
param([string]$Zig = "zig", [string]$Dotnet = "dotnet")
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$out = Join-Path $root "build"
New-Item -ItemType Directory -Force $out | Out-Null
function Z { & $Zig cc -target x86-windows-gnu @args; if ($LASTEXITCODE) { throw "zig cc failed: $args" } }

# --- steamwrap + its ABI test (fake real DLL + MSVC-thiscall harness)
Z -shared -O2 -Wall -Wextra -o "$out\steam_api.dll" "$root\steamwrap\steamwrap.c" "$root\steamwrap\steamwrap.def"
$t = Join-Path $out "test"
if (Test-Path $t) { Remove-Item -Recurse -Force $t }
New-Item -ItemType Directory -Force $t | Out-Null
Z -shared -O2 -o "$t\steam_api_real.dll" "$root\steamwrap\test\fake_real.c"
Z -O1 -o "$t\harness.exe" "$root\steamwrap\test\harness.c"
Copy-Item "$out\steam_api.dll" $t
Push-Location $t
try { & .\harness.exe; $rc = $LASTEXITCODE } finally { Pop-Location }
if ($rc) { throw "steamwrap offline test failed" }

# --- coopnet + its tests (wire protocol over real UDP, projection math)
Z -shared -O2 -Wall -Wextra -o "$out\coopnet.dll" "$root\coopnet\coopnet.c" "$root\coopnet\probe.c" "$root\coopnet\puppet.c" "$root\coopnet\steamnet.c" "$root\coopnet\replay.c" "$root\coopnet\worldsync.c" "$root\coopnet\life.c" "$root\coopnet\sfx.c" "$root\coopnet\ui.c" "$root\coopnet\skin.c" -lws2_32 -lgdi32 -luser32 -lwinmm -ldwmapi -lgdiplus
# overlay screens rendered offscreen (check the images in build\ui\)
New-Item -ItemType Directory -Force "$out\ui" | Out-Null
Z -O2 -Wall -Wextra -o "$out\ui_preview.exe" "$root\coopnet\test\ui_preview.c" "$root\coopnet\ui.c" -lgdiplus -lgdi32
& "$out\ui_preview.exe" "$out\ui" | Out-Null; if ($LASTEXITCODE) { throw "ui_preview failed" }
Z -O2 -Wall -Wextra -o "$out\proto_test.exe" "$root\coopnet\test\proto_test.c" -lws2_32
& "$out\proto_test.exe"; if ($LASTEXITCODE) { throw "proto_test failed" }
Z -O2 -Wall -Wextra -o "$out\view_test.exe" "$root\coopnet\test\view_test.c"
& "$out\view_test.exe"; if ($LASTEXITCODE) { throw "view_test failed" }
# co-op death rules (spectate until both are down) with the life/alien packets; motion-tracker math
Z -O2 -Wall -Wextra -o "$out\life_test.exe" "$root\coopnet\test\life_test.c" "$root\coopnet\life.c"
& "$out\life_test.exe"; if ($LASTEXITCODE) { throw "life_test failed" }
Z -O2 -Wall -Wextra -o "$out\skin_test.exe" "$root\coopnet\test\skin_test.c"
& "$out\skin_test.exe"; if ($LASTEXITCODE) { throw "skin_test failed" }
# automatic world sync table (coopnet\syncmap.h) and the joiner's alien playback; the table generator's own rules
Z -O2 -Wall -Wextra -o "$out\sync_test.exe" "$root\coopnet\test\sync_test.c"
& "$out\sync_test.exe"; if ($LASTEXITCODE) { throw "sync_test failed" }
if (Get-Command python -ErrorAction SilentlyContinue) { & python -I "$root\tools\syncmap\gen_syncmap.py" --selftest; if ($LASTEXITCODE) { throw "gen_syncmap selftest failed" } }
Z -O2 -Wall -Wextra -o "$out\hud_test.exe" "$root\coopnet\test\hud_test.c"
& "$out\hud_test.exe"; if ($LASTEXITCODE) { throw "hud_test failed" }
# lobby: launcher window, its screens rendered to images (build\lobby\), and the protocol/LAN test on loopback
Z -O2 -Wall -Wextra "-Wl,--subsystem,windows" -o "$out\lobby.exe" "$root\lobby\lobby.c" "$root\lobby\lobbyui.c" "$root\lobby\lobbynet.c" -lgdiplus -lgdi32 -luser32 -lws2_32
New-Item -ItemType Directory -Force "$out\lobby" | Out-Null
Z -O2 -Wall -Wextra -o "$out\lobby_preview.exe" "$root\lobby\test\lobby_preview.c" "$root\lobby\lobbyui.c" "$root\lobby\lobbynet.c" -lgdiplus -lgdi32 -lws2_32
& "$out\lobby_preview.exe" "$out\lobby" | Out-Null; if ($LASTEXITCODE) { throw "lobby_preview failed" }
Z -O2 -Wall -Wextra -o "$out\lobby_test.exe" "$root\lobby\test\lobby_test.c" "$root\lobby\lobbynet.c" -lws2_32
& "$out\lobby_test.exe"; if ($LASTEXITCODE) { throw "lobby_test failed" }
# skin cards for the README (build\skins\; copy to docs\img\skins\ when skins change)
New-Item -ItemType Directory -Force "$out\skins" | Out-Null
Z -O2 -Wall -Wextra -o "$out\skincards.exe" "$root\tools\skincards\skincards.c" -lgdiplus -lgdi32
& "$out\skincards.exe" "$out\skins" | Out-Null; if ($LASTEXITCODE) { throw "skincards failed" }
# Steam P2P transport against a fake steam_api that checks the MSVC argument layout
$st = Join-Path $out "stest"
if (Test-Path $st) { Remove-Item -Recurse -Force $st }
New-Item -ItemType Directory -Force $st | Out-Null
Z -shared -O2 -o "$st\steam_api_real.dll" "$root\coopnet\test\fake_steam.c"
Z -O2 -Wall -Wextra -o "$st\steamnet_test.exe" "$root\coopnet\test\steamnet_test.c" "$root\coopnet\steamnet.c"
Push-Location $st
try { & .\steamnet_test.exe; $rc = $LASTEXITCODE } finally { Pop-Location }
if ($rc) { throw "steamnet_test failed" }

# --- player package
$pkg = Join-Path $out "alien-isolation-coop"
if (Test-Path $pkg) { Remove-Item -Recurse -Force $pkg }
New-Item -ItemType Directory -Force $pkg | Out-Null
Copy-Item "$out\steam_api.dll", "$out\coopnet.dll", "$out\lobby.exe", "$root\dist\install.ps1", "$root\dist\play-coop.bat", "$root\README.md", "$root\LICENSE" $pkg
Copy-Item -Recurse "$root\dist\couchlink" (Join-Path $pkg "couchlink")
Copy-Item -Recurse "$root\third_party" (Join-Path $pkg "third_party")
# skinprep (optional skins step of the installer); needs the .NET 8 runtime on the player's PC
$dn = Get-Command $Dotnet -ErrorAction SilentlyContinue
if ($dn) {
    & $Dotnet publish "$root\tools\skinprep" -c Release -o (Join-Path $pkg "skinprep") --self-contained false -p:DebugType=None -p:DebugSymbols=false -v q -nologo | Out-Null
    if ($LASTEXITCODE) { throw "skinprep publish failed" }
    Remove-Item (Join-Path $pkg "skinprep\*.pdb") -ErrorAction SilentlyContinue
} else { "note: no .NET SDK, package built without skinprep" }
$zip = Join-Path $out "alien-isolation-coop.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path "$pkg\*" -DestinationPath $zip
Get-ChildItem $pkg -File | ForEach-Object { "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash, $_.Name } | Set-Content "$out\SHA256SUMS.txt"
"package: $zip"
Get-Content "$out\SHA256SUMS.txt"
