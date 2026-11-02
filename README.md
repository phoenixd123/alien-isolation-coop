# Alien: Isolation co-op (work in progress)

An attempt to add online co-op to *Alien: Isolation* (2014, Creative Assembly). Built
with the [coop-anything](https://github.com/cranebeaker/coop-anything) playbook.

> **Status: early prototype. You can see each other, nothing else is shared yet.**
> Each player plays their own copy of the campaign. Your friend appears as a marker on your
> screen, but the alien, doors, items and story are **not** synced.

## What works (tested 2026-11-01)

| | Status |
|---|---|
| Game boots with the mod; the original `AI.exe` is not modified | ✅ tested in game |
| Two copies of the game running at once on one PC (for testing) | ✅ tested in game |
| Co-op saves go to `coop_saves\`, never to your Steam Cloud saves | ✅ tested in game |
| Achievement and stat writes are blocked while modded | ✅ offline test |
| **Seeing the other player** (marker with name and distance, arrow when off-screen) | ✅ tested in game, both directions |
| Host / join over UDP (host doesn't need the joiner's IP) | ✅ tested in game (two copies, one PC) |
| Install / uninstall script | ✅ tested on a fake game folder |
| Two different PCs over the internet | ⚠️ not tested yet. Host must allow UDP 45500 (port forward or VPN such as Tailscale / ZeroTier) |
| A visible 3D character for your friend | ❌ not built. The marker is a 2D overlay and shows through walls |
| Shared alien, doors, items, story | ❌ not built (unsolved, months of work) |

Tested only with **Steam build 4940658**, where `AI.exe` has SHA-256
`DDE66521798AAE8C39ED4E7A3400F561FCECFFF83619D1426B9E0CEDF5819A90`. The installer
refuses other builds unless you pass `-Force`.

## Play it (players)

1. Download `alien-isolation-coop.zip` from Releases (or build it, below) and unzip it anywhere.
2. Close the game. In the unzipped folder run:
   `powershell -ExecutionPolicy Bypass -File install.ps1`
   It finds the game through Steam, checks the version, backs up `steam_api.dll`, and copies
   your current saves into `coop_saves\default` (copies only, your Steam saves are untouched).
3. With Steam running, in the game folder:
   - host: `play-coop.bat host YourName` and give your friend your IP (UDP port 45500 must be reachable)
   - friend: `play-coop.bat join <host IP> TheirName`
4. Both pick **Continue Game**. Your friend shows up as a green marker.

The overlay only draws while the game window is focused. Play in windowed or borderless
mode: exclusive fullscreen can hide it.

Uninstall: `powershell -ExecutionPolicy Bypass -File install.ps1 -Uninstall` restores the
original `steam_api.dll`. Your co-op saves stay in `coop_saves`.

## What's here

`steamwrap/` builds a replacement `steam_api.dll`:

- Forwards every Steam call to the real DLL (renamed `steam_api_real.dll`) through jump
  thunks, so pass-through calls can't corrupt the stack.
- `SteamAPI_RestartAppIfNecessary` returns false, so the game runs from its folder without
  relaunching. You still need to own the game, and Steam must be running.
- `ISteamRemoteStorage012` file and cloud calls go to `<game>\coop_saves\<profile>\`.
- `ISteamUserStats011` writes (stats, achievements) are no-ops.
- With `AIC_PROFILE=<name>` set, the game's single-instance lock gets a per-profile name,
  so two copies can run side by side.
- Loads `coopnet.dll` from the game folder.

`coopnet/` is the co-op layer:

- Reads the player camera record each frame (`mgr=[exe+0x12F0C88]`, `blk=[mgr+0x58]`,
  record at `blk+0x78C`, checked against its signature `47 / 0.05 / 1000`).
- Measured in game: `rec[0..3]` is the view quaternion (x,y,z,w), identity looks along
  world **+Z**, **+Y** is up, **+X** is right; `rec[4..6]` is the camera position.
- Sends position and rotation at 20 Hz (60-byte UDP packet with checksum), draws the
  peer on a click-through overlay. Logs go to `coop_logs\`.
- Settings (environment): `AIC_COOP_MODE=host|join`, `AIC_COOP_PEER=<ip>:<port>`,
  `AIC_COOP_PORT`, `AIC_COOP_NAME`, `AIC_PROFILE`.

## Build and test

Needs [Zig 0.16.0](https://ziglang.org/download/).

```powershell
.\build.ps1 -Zig C:\path\to\zig.exe
```

Builds both DLLs, runs every offline test (no game needed) and writes the player package
`build\alien-isolation-coop.zip` plus `build\SHA256SUMS.txt`. Last run (2026-11-01):

```
steamwrap ABI test:   40 passed, 0 failed
protocol over UDP:    12 passed, 0 failed  (476/476 packets, loss and reorder simulated)
overlay projection:   10 passed, 0 failed  (cases taken from in-game measurements)
```

In-game test (2026-11-01, two copies on one PC, driven by a bot: menus, walking, turning):
player 2 walked 5.7 m down a corridor; player 1's overlay put the marker at the
predicted screen spot (camera-space x=-1.26, z=5.60), and the reverse direction matched
too. Host/join: the host learned the joiner's address from its first packet; packets
sent and received matched (tx 1874, rx 1872 at the end).

## Try it (developers only)

1. Copy the whole game folder somewhere else. Don't use your real install.
2. In the copy: rename `steam_api.dll` to `steam_api_real.dll`, put `build\steam_api.dll`
   there, and create `steam_appid.txt` containing `214490`.
3. With Steam running: `$env:AIC_PROFILE="p1"; Start-Process .\AI.exe -WorkingDirectory .`
   (repeat with `p2` for a second copy).
4. Logs are in `coop_logs\`.

## Known issues

- The marker is drawn over the game, so it shows through walls. A real in-game character
  needs the engine's character spawning, which isn't understood yet.
- No NAT traversal: the host must open UDP 45500 or both players use a VPN.
- Fullscreen copies minimize when they lose focus. Use windowed mode.
- The first launch of a new profile shows the brightness calibration screen.

## Roadmap

1. Test across two PCs (needs two people or two machines).
2. Steam networking (ISteamNetworking005 is available) so no port forwarding is needed.
3. A visible character puppet instead of the overlay marker.
4. Host-owned alien and NPCs, doors, story flags (see coop-anything issue #85 for the
   engine notes this would build on).

## Credits and prior work

- Interface slot order comes from the versioned Steamworks headers in the
  [Goldberg emulator](https://gitlab.com/Mr_Goldberg/goldberg_emulator).
- Engine research (Cathode, memory layout) by another coop-anything contributor:
  [issue #85](https://github.com/cranebeaker/coop-anything/issues/85).

No game files are included. You need your own copy of the game.

## License

MIT, see [LICENSE](LICENSE).
