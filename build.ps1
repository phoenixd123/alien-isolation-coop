# Builds steamwrap (steam_api.dll) and coopnet.dll, runs every offline test, and assembles the player package.
# Needs Zig 0.16.0 (https://ziglang.org/download/). Pass its path with -Zig, or have zig on PATH.
param([string]$Zig = "zig")
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
Z -shared -O2 -Wall -Wextra -o "$out\coopnet.dll" "$root\coopnet\coopnet.c" "$root\coopnet\probe.c" "$root\coopnet\puppet.c" "$root\coopnet\steamnet.c" "$root\coopnet\replay.c" "$root\coopnet\worldsync.c" "$root\coopnet\life.c" "$root\coopnet\sfx.c" "$root\coopnet\ui.c" -lws2_32 -lgdi32 -luser32 -lwinmm -ldwmapi -lgdiplus
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
Z -O2 -Wall -Wextra -o "$out\hud_test.exe" "$root\coopnet\test\hud_test.c"
& "$out\hud_test.exe"; if ($LASTEXITCODE) { throw "hud_test failed" }
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
Copy-Item "$out\steam_api.dll", "$out\coopnet.dll", "$root\dist\install.ps1", "$root\dist\play-coop.bat", "$root\README.md", "$root\LICENSE" $pkg
Copy-Item -Recurse "$root\dist\couchlink" (Join-Path $pkg "couchlink")
$zip = Join-Path $out "alien-isolation-coop.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path "$pkg\*" -DestinationPath $zip
Get-ChildItem $pkg | ForEach-Object { "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash, $_.Name } | Set-Content "$out\SHA256SUMS.txt"
"package: $zip"
Get-Content "$out\SHA256SUMS.txt"
