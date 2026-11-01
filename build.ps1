# Builds steamwrap (steam_api.dll) and runs its offline test.
# Needs Zig 0.16.0 (https://ziglang.org/download/). Pass its path with -Zig, or have zig on PATH.
param([string]$Zig = "zig")
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$out = Join-Path $root "build"
New-Item -ItemType Directory -Force $out | Out-Null

& $Zig cc -target x86-windows-gnu -shared -O2 -Wall -Wextra -o "$out\steam_api.dll" "$root\steamwrap\steamwrap.c" "$root\steamwrap\steamwrap.def"
if ($LASTEXITCODE) { throw "steamwrap build failed" }

$t = Join-Path $out "test"
if (Test-Path $t) { Remove-Item -Recurse -Force $t }
New-Item -ItemType Directory -Force $t | Out-Null
& $Zig cc -target x86-windows-gnu -shared -O2 -o "$t\steam_api_real.dll" "$root\steamwrap\test\fake_real.c"
if ($LASTEXITCODE) { throw "fake build failed" }
& $Zig cc -target x86-windows-gnu -O1 -o "$t\harness.exe" "$root\steamwrap\test\harness.c"
if ($LASTEXITCODE) { throw "harness build failed" }
Copy-Item "$out\steam_api.dll" $t
Push-Location $t
try { & .\harness.exe; $rc = $LASTEXITCODE } finally { Pop-Location }
if ($rc) { throw "offline test failed" }
"built $out\steam_api.dll"
