# Alien: Isolation co-op (work in progress)

An attempt to add online co-op to *Alien: Isolation* (2014, Creative Assembly). Built
with the [coop-anything](https://github.com/cranebeaker/coop-anything) playbook.

> **Status: foundation only. You cannot play co-op with this yet.**

## What works (tested 2026-11-01)

| | Status |
|---|---|
| Game boots from a separate test copy, so the real install is never touched | ✅ tested |
| Two copies of the game running at once on one PC | ✅ tested (both reached the menu) |
| Saves go to a local folder per player, never to Steam Cloud | ✅ tested |
| Achievement and stat writes are blocked in test copies | ✅ offline test |
| Seeing the other player | ❌ not built yet |
| Shared alien, doors, items, story | ❌ not built yet (unsolved) |

Tested only with **Steam build 4940658**, where `AI.exe` has SHA-256
`DDE66521798AAE8C39ED4E7A3400F561FCECFFF83619D1426B9E0CEDF5819A90`. Any other version
is untested.

## What's here

`steamwrap/` builds a replacement `steam_api.dll` for a *copy* of the game:

- Forwards every Steam call to the real DLL (renamed `steam_api_real.dll`) through jump
  thunks, so pass-through calls can't corrupt the stack.
- `SteamAPI_RestartAppIfNecessary` returns false, so the copy doesn't relaunch the
  installed game. You still need to own the game, and Steam must be running.
- `ISteamRemoteStorage012` file and cloud calls go to `<copy>\coop_saves\<profile>\`.
- `ISteamUserStats011` writes (stats, achievements) are no-ops.
- With `AIC_PROFILE=<name>` set, the game's single-instance lock gets a per-profile name,
  so two copies can run side by side. `AI.exe` itself is not modified.
- Loads `coopnet.dll` from the game folder if present. This is the future co-op layer and
  doesn't exist yet.

## Build and test

Needs [Zig 0.16.0](https://ziglang.org/download/).

```powershell
.\build.ps1 -Zig C:\path\to\zig.exe
```

This builds `build\steam_api.dll` and runs the offline test (no game needed). The last
run printed `40 passed, 0 failed`.

## Try it (developers only)

1. Copy the whole game folder somewhere else. Don't use your real install.
2. In the copy: rename `steam_api.dll` to `steam_api_real.dll`, put `build\steam_api.dll`
   there, and create `steam_appid.txt` containing `214490`.
3. With Steam running: `$env:AIC_PROFILE="p1"; Start-Process .\AI.exe -WorkingDirectory .`
   (repeat with `p2` for a second copy).
4. Logs are in `coop_logs\`.

## Known issues

- Fullscreen copies minimize when they lose focus. Use windowed mode.
- The first launch of a new profile shows the brightness calibration screen.

## Credits and prior work

- Interface slot order comes from the versioned Steamworks headers in the
  [Goldberg emulator](https://gitlab.com/Mr_Goldberg/goldberg_emulator).
- Engine research (Cathode, memory layout) by another coop-anything contributor:
  [issue #85](https://github.com/cranebeaker/coop-anything/issues/85).

No game files are included. You need your own copy of the game.

## License

MIT, see [LICENSE](LICENSE).
