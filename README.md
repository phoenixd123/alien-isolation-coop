# Alien: Isolation co-op (work in progress)

An attempt to add online co-op to *Alien: Isolation* (2014, Creative Assembly). Built
with the [coop-anything](https://github.com/cranebeaker/coop-anything) playbook.

> **Status: prototype. You see each other, and doors, picked-up items and story steps are
> shared. The alien is not.** Each player still runs their own copy of the campaign; the mod
> replays the other player's door openings, pickups and mission steps in your game.

## What works (tested 2026-11-01)

| | Status |
|---|---|
| Game boots with the mod; the original `AI.exe` is not modified | ✅ tested in game |
| Two copies of the game running at once on one PC (for testing) | ✅ tested in game |
| Co-op saves go to `coop_saves\`, never to your Steam Cloud saves | ✅ tested in game |
| Achievement and stat writes are blocked while modded | ✅ offline test |
| **Seeing the other player** (marker with name and distance, arrow when off-screen) | ✅ tested in game, both directions |
| Host / join over UDP (host doesn't need the joiner's IP) | ✅ tested in game (two copies, one PC) |
| **Host / join over Steam** (by Steam ID, no IP, no port forwarding) | ✅ tested in game with the real Steam client (loopback to own ID, 1168/1168 packets; failed-connect callback). Two different accounts: not tested yet |
| Install / uninstall script | ✅ tested on a fake game folder |
| Two different PCs over the internet | ⚠️ not tested yet. Use the Steam mode; the UDP mode needs UDP 45500 open on the host |
| A visible 3D character for your friend | ❌ not built. The marker is a 2D overlay and shows through walls |
| **Doors**: a door your friend opens opens for you too (and closes on its own) | ✅ tested in game (two copies, one PC) |
| **Items**: an item your friend picks up disappears from your world (you don't get a copy) | ✅ tested in game; the pickup was started by the test tool, not by aiming at it |
| **Story**: mission steps your friend reaches run in your game too (your own mission script continues) | ✅ tested in game on one mission step (M04 crowbar); not a whole mission |
| Shared alien | ❌ not built. Its record in the engine's character table is found (see below), but no save with the alien was available to test |

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
   - host: `play-coop.bat steam-host`. Your Steam ID (a 17-digit number) appears at the top
     left of the game window and in `coop_logs\my_steam_id.txt`. Send it to your friend.
   - friend: `play-coop.bat steam-join <host's Steam ID>`
   Names default to your Steam names. No IP addresses or port forwarding are needed: Steam
   relays the traffic. You each need your own Steam account and copy of the game.
   Prefer direct IP? `play-coop.bat host` / `play-coop.bat join <host IP>` (the host must
   open UDP port 45500).
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
- Two transports, same 60-byte packet. UDP, or Steam P2P (`coopnet/steamnet.c`): the
  game's own `ISteamNetworking005` (`SendP2PPacket` on channel 7, `AcceptP2PSessionWithUser`
  on `P2PSessionRequest_t`, `AllowP2PPacketRelay(true)`). The host takes the first player who
  connects and ignores everyone else; a joiner only accepts packets from the host's ID.
- Settings (environment): `AIC_COOP_MODE=host|join|steam-host|steam-join`,
  `AIC_COOP_PEER=<ip>:<port>`, `AIC_COOP_PEER_STEAM=<SteamID64>` (or `self` for a one-account
  loopback test), `AIC_COOP_PORT`, `AIC_COOP_NAME`, `AIC_PROFILE`.

## Build and test

Needs [Zig 0.16.0](https://ziglang.org/download/).

```powershell
.\build.ps1 -Zig C:\path\to\zig.exe
```

Builds both DLLs, runs every offline test (no game needed) and writes the player package
`build\alien-isolation-coop.zip` plus `build\SHA256SUMS.txt`. Last run (2026-11-01):

```
steamwrap ABI test:   40 passed, 0 failed
protocol over UDP:    18 passed, 0 failed  (476/476 packets, loss and reorder simulated; world packets)
overlay projection:   10 passed, 0 failed  (cases taken from in-game measurements)
steam transport:      24 passed, 0 failed  (fake steam_api checks the MSVC argument layout both ways)
```

World sync in game (2026-11-03, HAB_AIRPORT save, two copies on one PC over localhost, bot-driven):
- Door: p2 walked into `Door_SML_9`; p1 received the event 38 ms later and p1's own trace shows the
  door's full opening sequence while p1 stood still.
- Item: p2 collected `molotov_cocktail_1` (started with the replay test tool); p1 ran the molotov's
  `hide_pickup` (model suspended, selection box disabled).
- Story: p2 fired `LogicOnce_give_crowbar_1` in M04_Pt01; p1's mission script then ran
  `WEAPON_GiveToCharacter_give_crowbar` and gave p1 the crowbar.

Steam transport with the real Steam client (2026-11-03, one copy at the main menu):
`steam-join self` sent 1168 and received 1168 packets through `ISteamNetworking005`, and
`steam-join` to an offline account delivered `P2PSessionConnectFail_t` (error 4, timeout) to
our callback object with the game still running. Two accounts on two PCs: not tested yet.

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
- UDP mode has no NAT traversal (use the Steam mode instead).
- Steam mode: both players need their own Steam account. Two copies on one account can't
  talk to each other (Steam has one ID per account), so local testing uses UDP or `self`.
- Fullscreen copies minimize when they lose focus. Use windowed mode.
- The first launch of a new profile shows the brightness calibration screen.
- World sync covers doors, pickups and mission `LogicOnce` steps only. Keycard/hacking unlocks,
  lifts, cutscenes and anything else a mission does outside a `LogicOnce` are not synced yet, so a
  long session can still drift. Load the same save on both PCs.
- At level load each game reports doors that the save left open; harmless, the other game opens them too.

## How world sync works

Cathode runs level scripts as messages: "run method M on entity E". Every entity is named by a
path of IDs from the level files (for example `Door_Package_1 / Door_SML_9 / ENVIRONMENT_Airport_1`),
and that path is the same on both PCs. coopnet hooks the engine's `EntityManager::call_method`
(RVA 0x526680) and watches for a few messages:

| When your game does this | Your friend's game does this |
|---|---|
| a `Door_Package` sets `IsOpen` | `request_open` on that door's `Door` (opens and auto-closes; locked doors stay locked) |
| a `pickup_common` fires `SetPersistentIntegerValue` (item collected) | the same trigger plus `IntegerEquals_1`: the load-time "already taken" path, which hides the item |
| a `LogicOnce` inside one of the 35 story mission composites fires | the same `LogicOnce` (fires once by design, so it can't double up); their mission script carries on |

The other game finds the entity by its path (about 0.1 s) and runs the method on the game thread.
Only methods in this table are accepted from the network. A change that came from your friend is not
sent back. These archetypes have the same IDs in every story level. `AIC_SYNC=0` turns world sync off.

## Roadmap

1. Test across two PCs with two Steam accounts (needs two people or two machines).
2. A visible character puppet instead of the overlay marker.
3. The alien: the engine keeps live character positions in a static table (`AI.exe + 0x1307AC8`,
   stride 0xB0, one record per character, a tag after the position). coopnet logs it every 5 s
   (`chars:` lines). Next: load a save where the alien is present, find its record, then make the
   host own it and drive the client's alien (or hide it and show the host's).
4. More world sync: keycard/hacking unlocks, lifts, terminals, cutscenes, and a whole mission played
   through with two players.

## Credits and prior work

- Interface slot order comes from the versioned Steamworks headers in the
  [Goldberg emulator](https://gitlab.com/Mr_Goldberg/goldberg_emulator).
- Engine research (Cathode, memory layout) by another coop-anything contributor:
  [issue #85](https://github.com/cranebeaker/coop-anything/issues/85).

No game files are included. You need your own copy of the game.

## License

MIT, see [LICENSE](LICENSE).
