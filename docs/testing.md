# Test log

What I ran, on what, and what came out. Most of my in-game tests use two copies of the game on one PC
(Steam build 4940658), driven by a bot script. Newest at the bottom of each section.

## Offline suite

`build.ps1` runs these on every build (no game needed). Last run 2026-11-09:

```
steamwrap ABI test:   40 passed, 0 failed
protocol over UDP:    18 passed, 0 failed  (476/476 packets, loss and reorder simulated; world packets)
overlay projection:   10 passed, 0 failed  (cases taken from in-game measurements)
death rules:          31 passed, 0 failed  (spectate, both down, disconnect, give up, early load, stale packets, solo)
skins:                38 passed, 0 failed  (spawn request matches AI-MP's layout, level names, skin choice, packet)
automatic sync:       83 passed, 0 failed  (generated table sorted and complete, known entities in/out, alien playback incl. vent jumps and
                                            clock wrap; cutscene tables, landing spot, who moves, cutscene packet;
                                            regroup, teleport fade, build check packet; the joiner's alien: walk,
                                            stop, turn, snap, vents, full alien packet; carry-along: what counts as
                                            a scripted move, who goes along, carry packet)
                      + gen_syncmap selftest (what counts as saved state, once-only, or a player's own)
lobby:                62 passed, 0 failed  (state machine on a fake clock, then real UDP join/ready/start and TCP save copy on loopback;
                                            Steam ID parsing; the save copy now runs on the stream code the Steam copy uses)
motion tracker:       14 passed, 0 failed  (yaw, pitch ignored, motion window, teleports, ping rate)
steam transport:      24 passed, 0 failed  (fake steam_api checks the MSVC argument layout both ways)
```

## skinprep's model writer (files only, no game), 2026-11-09

On the test copy's level files. `skinprep --roundtrip` loads a level and saves it with nothing added; `--check`
adds the skins, writes the level to a staging folder, reads it back and checks it, then deletes it (nothing
installed).

- BSP_TORRENS round trip: `LEVEL_MODELS.PAK` and `MODELS_LEVEL.BIN` byte-identical to the originals (CathodeLib's
  writer made them 87 KB and 178 KB smaller on HAB_AIRPORT), 0 records re-pointed.
- BSP_TORRENS (+ AXEL, RICARDO, MARLOW) and HAB_AIRPORT (+ TAYLOR, SAMUELS, RICARDO, MARLOW): written and read
  back; every added character's scripts resolve to models, materials, shaders and textures in the written level,
  with the same mesh data as in its source level.
- Not done yet: loading those levels in the game.

## Installer

2026-11-06, on the test copy of the game (Steam build 4940658), game closed, the packaged `install.ps1`:

- `-Uninstall` on a copy that had skins: put back the original `steam_api.dll`, removed the mod files, and ran
  `skinprep --restore`. I compared all 304 backed-up level files with the game's files afterwards: 0 differ, and
  no file skinprep had added was left behind.
- `-Skins`: installed the mod, `lobby.exe` and `skinprep`, then changed 19 levels with 0 failed (TECH_COMMS
  skipped on purpose) in 132 s, and wrote `coop_skins.txt`.

## Sync rules across levels

Not a game test: a check of the level files. I dumped every level's script IDs (`levelmod ids`) and looked for
the entities each world-sync rule listens to and replays. "yes" means the archetype is in that level with the
same IDs the mod uses, so the rule can fire there. 2026-11-06:

| Level | doors | locks/cutting | pickups | keycards | tools | level change | story composites |
|---|---|---|---|---|---|---|---|
| BSP_LV426_PT01 | - | - | yes | yes | yes | yes | 1 |
| BSP_LV426_PT02 | - | - | yes | yes | yes | yes | 1 |
| BSP_TORRENS | yes | yes | yes | yes | yes | yes | 2 |
| DLC_CHALLENGEMAP16 | yes | yes | yes | yes | yes | yes | 0 |
| ENG_ALIEN_NEST | yes | yes | yes | yes | yes | yes | 1 |
| ENG_REACTORCORE | yes | yes | yes | yes | yes | yes | 2 |
| ENG_TOWPLATFORM | yes | yes | yes | yes | yes | yes | 1 |
| HAB_AIRPORT | yes | yes | yes | yes | yes | yes | 3 |
| HAB_CORPORATEPENT | yes | yes | yes | yes | yes | yes | 1 |
| HAB_SHOPPINGCENTRE | yes | yes | yes | yes | yes | yes | 3 |
| SCI_ANDROIDLAB | yes | yes | yes | yes | yes | yes | 2 |
| SCI_HOSPITALLOWER | yes | yes | yes | yes | yes | yes | 2 |
| SCI_HOSPITALUPPER | yes | yes | yes | yes | yes | yes | 1 |
| SCI_HUB | yes | yes | yes | yes | yes | yes | 4 |
| TECH_COMMS | yes | yes | yes | yes | yes | yes | 2 |
| TECH_HUB | yes | yes | yes | yes | yes | yes | 4 |
| TECH_MUTHRCORE | yes | yes | yes | yes | yes | yes | 1 |
| TECH_RND | yes | yes | yes | yes | yes | yes | 1 |
| TECH_RND_HZDLAB | yes | yes | yes | yes | yes | yes | 2 |
| SOLACE | yes | yes | yes | yes | yes | yes | 1 |

The two LV426 levels have no `Door_Package` or `Door_Mechanism` doors (the derelict uses its own), so door and lock
sync doesn't reach them. The Survivor map has no story mission composites, as expected.

## In game, 2026-11-09 (v0.4.0)

Two copies on one PC (Steam build 4940658), bot-driven, host p1 and joiner p2 over UDP on localhost.

- **Why the `-Skins` levels never loaded.** skinprep's new model writer (byte-identical round trip) still made the
  game quit while loading, silently and with exit code 0. Bisected on HAB_AIRPORT: a full CathodeLib round trip
  loads; one character's materials, textures and shaders alone load; adding any model, even a copy of one of the
  airport's own props under a new name, quits. Logged the game's exit with `AIC_DIAG=1` (a new diagnostic in
  coopnet: serious exceptions, `ExitProcess`, and the game's quit request, each with the game-code return
  addresses on the stack) and followed it with Ghidra: `FILE_HASHES::verify_integrity` (RVA 0x374CA0) checks some
  data files against a table of sizes and hashes and requests quit when one differs. `MODELS_LEVEL.BIN` is one of
  them. OpenCAGE patches its two calls in AI.exe on disk; coopnet now points them at its own check in memory, which
  waives it only for files skinprep changed (those with a backup in `coop_backup\skins`). So it was never
  CathodeLib's writer that broke v0.2/v0.3's levels.
- **`-Skins` in game:** installed on 19 levels (0 failed; TECH_COMMS skipped as before). The airport with Taylor,
  Samuels, Ricardo and Marlow added loads. Two copies in the airport, host as Taylor and joiner as Samuels (neither
  is in the airport's own files): both bodies spawned, and the joiner saw the host as Taylor, rendered correctly.
  Survivor (The Basement, DLC_CHALLENGEMAP16) with skins: both bodies spawned there too.
- **Shared alien** (Survivor, the alien spawned by hand with `AIC_DEBUG_SPAWN`, a test-only switch that calls
  the generic `spawn` method on its script entity as OpenCAGE does):
  - The joiner's alien walks the host's alien's path with move orders: in one 3-minute run 27,922 move orders and
    40 teleports, the two aliens within about a metre at every 5-second sample (20:42:44: host's (-18.9, -21.2),
    joiner's (-18.9, -21.1) two seconds earlier).
  - Found and fixed: put into the vents by a teleport, the joiner's alien took no more teleports and stayed there
    (the vents need its own mind to leave). It now waits outside while the host's alien is in the vents, climbs
    out by itself if it is in the vents and the host's alien isn't (an alien spawns in the vents), and is respawned
    (despawn, spawn) if it is still stuck after 20 s.
  - Found and fixed: during the host alien's kill its behaviour updates stopped, the joiner's alien was let go,
    thought for itself and killed the joiner. The host now reads its alien straight from the character list when
    the AI-thread data is stale, and the joiner's alien never thinks for itself while the host is connected.
  - Found and fixed: the shared-alien role was set before the network side knew it, so the host ran as a joiner.
  - Not seen: a kill on the joiner's body in the host's game (the test lure brought the alien no closer: in this
    challenge the start room is shut until objectives are done).
- **Joiner crash on loading into a level the host was already playing:** an access violation in the game's script
  entity code 3 s after the level appeared, right after partner world events were replayed. Partner events are now
  held until our own level has been running 8 s, then applied in order (seen: 9 held, applied, no crash).
- **Carry-along false alarms:** the alien's kill animation and the menus jump the player back and forth; that sent
  thousands of carries and dragged the other player along. A move now counts only once it has settled (one jump,
  then 0.5 s in place), only in a running level, at most one every 8 s. 0 false carries in the next runs.
- Not exercised: a real lift ride (carry-along), a cutscene pull, a level change, Steam lobbies with two accounts,
  two PCs.

## In game, 2026-11-08 (v0.3.1)

Two copies on one PC (Steam build 4940658), bot-driven, host p1 and joiner p2 over UDP on localhost.

- **Level files from `-Skins` crash the game.** With skinprep's changed levels, Continue Game quit while loading
  (also without coopnet.dll, so not the mod's code). Narrowed down on HAB_AIRPORT: a CathodeLib load-and-save
  with nothing added already breaks it; with every round-tripped file except `LEVEL_MODELS.PAK` and
  `MODELS_LEVEL.BIN` (which come back 87 KB and 178 KB smaller) it loads. Same with CathodeLib staging c3efa4ea.
  `-Skins` is off; the installer restores levels changed by older versions. Checked: simulated a v0.3.0 `-Skins`
  install on two levels, installed v0.3.1 over it, 32 files restored, the game loaded the airport.
- **New player teleport (engine.c, the way OpenCAGE does it):** F7 regroup moved the joiner 9.3 m to the host,
  then (after the trail fix) 4.4 m to a spot 1.4 m behind the host. The game kept running. Before the fix, a host
  standing still made the joiner land inside them (the host got pushed 1.5 m).
- **Skins in a level that has the character:** airport, both games spawned the partner as AXEL (the picked
  SAMUELS / TAYLOR aren't in that level) and kept the body on the partner (checked on screen with the marker).
- **Start-up noise:** both games sent ~40 world events while a level loaded (start-up scripts of doors and
  terminals). Fixed: nothing is sent while not in a level and for 15 s after. A fired one-time step that keeps
  being triggered was re-sent every 30 s; now once per level session. Lookups that fail (part of the level not
  loaded) were retried at ~0.9 s of memory scanning each; now skipped for 20 s.
- **Reconnect bug:** after the joiner restarted, the host never saw them again (a restarted game counts its
  packets from 0). Fixed: a partner quiet for 2 s is taken fresh.
- **Lobby, end to end:** host lobby and joiner lobby on one PC; join by address 127.0.0.1 (on one PC the host's
  own listener takes the broadcast, as expected); "different saves" shown, Copy host's save copied and backed up
  (`coop_saves\backup\p2-...`), then "same save"; Ready, Start; both AI.exe started within 45 ms; names and roles
  from the lobby; connected.
- **New game from the start:** both games started a new campaign (Medium), played the opening and loaded into the
  Torrens side by side, connected; 8 world events each way during the opening. The bot can't play the mission
  past that point.
- **Survivor Mode (DLC_CHALLENGEMAP16):** both loaded and connected; no skin model in that level; the alien hadn't
  come out after 2 minutes without playing the objectives, so the shared alien wasn't exercised.
- Not exercised in game yet: a cutscene starting the partner move, level changes, the shared alien, the build
  check with two builds, internet play over Steam with two accounts.

## In game

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

Partner body (2026-11-05, HAB_AIRPORT, two copies on one PC, bot-driven):
- The body spawned at the partner and followed them: partner at (37.7, -12.8), body at (38.2, -12.4).
- It is visible in the other player's view.
- A body spawned on top of a standing player nudges them aside.
