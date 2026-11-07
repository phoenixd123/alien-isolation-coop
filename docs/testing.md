# Test log

What I ran, on what, and what came out. Most of my in-game tests use two copies of the game on one PC
(Steam build 4940658), driven by a bot script. Newest at the bottom of each section.

## Offline suite

`build.ps1` runs these on every build (no game needed). Last run 2026-11-06:

```
steamwrap ABI test:   40 passed, 0 failed
protocol over UDP:    18 passed, 0 failed  (476/476 packets, loss and reorder simulated; world packets)
overlay projection:   10 passed, 0 failed  (cases taken from in-game measurements)
death rules:          31 passed, 0 failed  (spectate, both down, disconnect, give up, early load, stale packets, solo)
skins:                38 passed, 0 failed  (spawn request matches AI-MP's layout, level names, skin choice, packet)
lobby:                57 passed, 0 failed  (state machine on a fake clock, then real UDP join/ready/start and TCP save copy on loopback)
motion tracker:       14 passed, 0 failed  (yaw, pitch ignored, motion window, teleports, ping rate)
steam transport:      24 passed, 0 failed  (fake steam_api checks the MSVC argument layout both ways)
```

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
