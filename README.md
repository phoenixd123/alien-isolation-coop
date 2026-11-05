# Alien: Isolation co-op (work in progress)

An attempt to add online co-op to *Alien: Isolation* (2014, Creative Assembly). Built
with the [coop-anything](https://github.com/cranebeaker/coop-anything) playbook.

> **Status: prototype.** You see each other on a shared motion tracker. Doors, picked-up items,
> keycards, tools and story steps are shared. When you die you see through your partner's eyes until
> they die too, then you both go back to the last checkpoint. The host's alien director (when the alien
> comes out, retreats or changes behaviour) drives the joiner's alien. Each player still runs their
> own copy of the campaign; the mod replays the other player's actions in your game.
> **Many of the new parts have not run in a level yet**; the table says which.

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
| **Keycards and tools**: a keycard or tool (cutting torch etc.) your friend picks up is given to you too | ⚠️ built, offline tests only. Not run in a level yet |
| **Alien director (host owns it)**: when the host's alien is withdrawn into the vents, reconfigured or changes stage, the joiner's alien does the same | ⚠️ built, offline tests only. No save with the alien was available |
| Alien position: the same alien in the same spot for both | ⚠️ partial. Each game streams where its alien is, and a spectator sees the partner's alien. Moving the joiner's alien to the host's spot (`AIC_ALIEN_PIN=1`) is experimental and untested; which character-table tag is the alien is a guess (3) |
| **Death and spectating**: when you die, your own game shows your partner's first-person view ("SPECTATING <name>") until they die too, then both games go back to the last checkpoint | ✅ tested in game with real deaths (two copies on one PC, below). The deaths came from the mission's own kill command, not from the alien |
| **Split screen on one PC** with [Couchlink](https://github.com/cranebeaker/couchlink) | ⚠️ the recipe is written, and seats were tested with two copies on one PC as Couchlink would start them (below). Not run inside Couchlink yet, and the recipe hasn't been checked with `check_recipe` |
| **Co-op horror HUD**: motion tracker, pings, heartbeat, partner-down sting and red flash | ⚠️ drawn and checked offscreen at the menu; not seen in a level yet |
| Online lobbies and Steam invites | 📝 planned only: [docs/online.md](docs/online.md). Joining by Steam ID works today |

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

### Split screen on one PC (Couchlink)

[Couchlink](https://github.com/cranebeaker/couchlink) runs one copy of the game per player on one PC,
each with its own part of the screen and its own controller or keyboard and mouse.

1. Install the mod (above), with your saves copied into `coop_saves\default`.
2. Add `couchlink\alien-isolation-coop.json` from the mod zip to Couchlink as a recipe.
3. Two players take a seat, press Play.

Each copy gets `AIC_COUCH_SEAT`. Seat 0 hosts and seat 1 joins `127.0.0.1:45500`. Each seat keeps its own
saves in `coop_saves\seat<N>`; an empty seat starts from a copy of `coop_saves\default`. One Steam account
is enough. In couch mode the overlay draws whenever the window is visible. When you die you see your
partner's screen in your half.

## Playing it like a horror game

- **Motion tracker** (bottom left). It shows movement, like the game's own tracker: any character that
  moved in the last second within 25 m, plus your partner in green. Pings get faster and higher as a
  contact closes in; under 6 m you hear a heartbeat. "PARTNER SIGNAL WEAK" flashes when you are more
  than 40 m apart.
- **Death.** When you die, your own game switches to your partner's eyes: it renders the level from their
  camera, in first person, with "SPECTATING <name>" at the top. The game-over menu, the fade to black and the
  death blur are held back meanwhile. Your partner gets a sting, a red flash and "<you> IS DOWN - they are
  watching you. Stay alive." When they die too, both games show the game-over menu and pick "Load Current
  Save", so you both go back to the last checkpoint. If they disconnect, you go back after 10 s; hold F9 for
  2 s to go back on your own. If the partner's camera isn't arriving (not in a level yet), you get the
  bio-monitor instead (online) or their window (same PC).
- **One alien director.** The host's game decides when the alien comes out and when it retreats;
  the joiner's alien follows those decisions.
- Sounds are generated by the mod (no files). `AIC_SFX=0` mutes them.

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
  loopback test), `AIC_COOP_PORT`, `AIC_COOP_NAME`, `AIC_PROFILE`, `AIC_COUCH_SEAT=<n>` (split screen),
  `AIC_SEED_SAVES=1` (an empty profile starts from `coop_saves\default`), `AIC_SFX=0`,
  `AIC_ALIEN_TAG=<n>` (character-table tag of the alien, default 3), `AIC_ALIEN_PIN=1` (experimental),
  `AIC_SYNC=0`, `AIC_SPECTATE_CAM=0` (no first-person spectating), `AIC_HOLD_MENU=0`, `AIC_DEATH_NOFADE=0`. QA only: `AIC_SPECTATE_TEST=1` (show the spectate view), `AIC_LIFE_TEST=<s>` (a simulated
  death after s seconds).
- `coopnet/life.c`: the death rules (pure logic, tested offline). `coopnet/hud.h`: tracker math.
  `coopnet/sfx.c`: the sounds. `coopnet/online.h` and `docs/online.md`: the online plan (not built).

## Build and test

Needs [Zig 0.16.0](https://ziglang.org/download/).

```powershell
.\build.ps1 -Zig C:\path\to\zig.exe
```

Builds both DLLs, runs every offline test (no game needed) and writes the player package
`build\alien-isolation-coop.zip` plus `build\SHA256SUMS.txt`. Last run (2026-11-04):

```
steamwrap ABI test:   40 passed, 0 failed
protocol over UDP:    18 passed, 0 failed  (476/476 packets, loss and reorder simulated; world packets)
overlay projection:   10 passed, 0 failed  (cases taken from in-game measurements)
death rules:          31 passed, 0 failed  (two simulated players: spectate, both down, disconnect, give up,
                                            picking a save early, stale packets, solo; life and alien packets)
motion tracker:       14 passed, 0 failed  (yaw, pitch ignored, motion window, teleports, ping rate)
steam transport:      24 passed, 0 failed  (fake steam_api checks the MSVC argument layout both ways)
```

Couch seats and death rules in game (2026-11-04, two copies on one PC at the main menu, started the way
the Couchlink recipe starts them, no bot input):
- Seat 0 became host and seat 1 joined `127.0.0.1:45500`. The host learned the joiner from its life
  packets. Both empty seat profiles were seeded with the 5 save files from `coop_saves\default`.
- `AIC_SPECTATE_TEST=1` on seat 1: it found seat 0's game window and showed it live
  (`DwmRegisterThumbnail` returned S_OK). The offscreen overlay render shows the spectate banner.
- `AIC_LIFE_TEST`: seat 1 "died" first. It held its game over and spectated, and seat 0 logged "partner
  died" 65 ms later. Seat 0 "died" 27 s later: its game over ran ("both down"), and seat 1 released its
  held game over 98 ms after that.

Real deaths in a level (2026-11-04, HAB_AIRPORT save, two copies on one PC, bot-driven). The kill came from
replaying the mission's own `TriggerBindCharacter_114`, which runs `CMD_Die.kill` on the player:
- First run of the original design: the player died and the engine showed its game-over menu, but the
  mod's hold never fired. A real death does not trigger the `GameOver` script node; that node is only for
  "you killed an ally". The engine runs `TriggerDeath` on the player's `Health_Damage_Death` script, then
  shows its own menu and waits there. It did not reload on its own in 130 s.
- A posted Enter (a window message: no focus, no real keyboard) picks the menu's default "Load Current
  Save"; the checkpoint loaded within 4 s.
- New design, same test:
  - p1 died: its game waited on the menu with the spectate view, and p2 got "p1 IS DOWN" 64 ms later.
  - p2 died 65 s later: both games pressed Load Current Save, and both were back at the checkpoint
    0.7 s after their press.
  - The reverse order (p2 first) passed too.
  - The engine reports each death twice; the mod now ignores the repeat.

First-person spectating (2026-11-04, same setup; p2 turned away from p1 so the views differ):
- Writing the player's camera record does nothing: the player camera is rebuilt from the character every
  frame (a test lifted it 3 m; the view did not move).
- A script camera does take over. `activate_camera` on a level `CameraResource` switched the view, and
  writing a pose into its camera records (quaternion, position, FOV, 0.05, 1000; several copies) moved the
  view to that pose. While dead, the death camera (`KillCamera`) is such a camera.
- The game-over menu is opened by one vtable slot of the UI state class (exe `0x1300224`,
  `FUN_00833af0`); the mod holds that call while spectating. Script dispatch stops while dead, so the held
  menu is shown from coopnet's own thread when both are down.
- Result: p1 died, and p1's own game rendered p2's view, matching p2's screen. It followed p2's 180° turn.
  It was sharp once the death depth-of-field (`Death DOF` in `systemic_post_processing`, started 0.3 s
  before `TriggerDeath`) was kept from starting. When p2 died, both were back at the checkpoint within 5 s.
  The final build passed the whole cycle again.
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
- World sync covers doors, pickups, keycards, tools, the alien director and mission `LogicOnce`
  steps. Hacking unlocks, lifts, cutscenes and anything else a mission does outside these are not
  synced yet, so a long session can still drift. Load the same save on both PCs.
- Each game still has its own alien. They make the same director decisions (host's), but between
  those they hunt on their own, so they can be in different places.
- Only deaths that run `TriggerDeath` (and the "killed an ally" game over) are caught. Deaths from the
  alien have not been seen yet. If one isn't caught, that player just gets the normal game-over menu; look
  for `life: player died` in the log.
- Spectating shows the partner's view at the rate their camera arrives (20 updates a second), so fast turns
  look slightly stepped. The partner's HUD and their own body are not shown.
- The spectate camera is found by matching camera records at the death camera's position; if more than 12
  match, the mod writes none and you see your death camera instead (logged as "too many").
- The held game-over menu is shown from coopnet's thread (the game's script thread is idle while you are
  dead). It worked in every test; it is still a call from outside the game thread.
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
| a `keycard` pickup fires `SetPlayerHasKeycard` | the same: they get the card |
| a `Cutting_Tool` fires `SetPlayerHasGatingTool` | the same: they get the tool |
| **host only**: `NPC_WithdrawAlien`, `NPC_AlienConfig` (mission scripts, `BackstagePlayerDetection`) or `NPC_SetAlienDevelopmentStage` fires | the same on the joiner's alien. The host never accepts these |
| you die (`TriggerDeath` on `Health_Damage_Death`) | not replayed: the death rules above |

The other game finds the entity by its path (about 0.1 s) and runs the method on the game thread.
Only methods in this table are accepted from the network. A change that came from your friend is not
sent back. These archetypes have the same IDs in every story level. `AIC_SYNC=0` turns world sync off.

## Roadmap

1. Test across two PCs with two Steam accounts (needs two people or two machines).
2. A visible character puppet instead of the overlay marker.
3. The alien: load a save where it is present (M06 or later). Confirm its character-table tag
   (`chars:` lines, guessed 3 = `ALLIANCE_GROUP.ALIEN`; the airport save's 5 and 7 fit CIVILIAN and
   DEAD). Then check whether writing the record moves it (`AIC_ALIEN_PIN=1`); if not, find the
   character's transform.
4. A death by the alien (check it runs `TriggerDeath` too); then a whole mission played through with
   two players.
5. Run the recipe inside Couchlink and check it with `check_recipe`.
6. Online lobbies and invites: [docs/online.md](docs/online.md).
7. More world sync: hacking unlocks, lifts, terminals, cutscenes.

## Credits and prior work

- Interface slot order comes from the versioned Steamworks headers in the
  [Goldberg emulator](https://gitlab.com/Mr_Goldberg/goldberg_emulator).
- Engine research (Cathode, memory layout) by another coop-anything contributor:
  [issue #85](https://github.com/cranebeaker/coop-anything/issues/85).

No game files are included. You need your own copy of the game.

## License

MIT, see [LICENSE](LICENSE).
