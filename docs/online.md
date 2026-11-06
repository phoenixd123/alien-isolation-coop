# Online play: plan

Status: **planned, not implemented, not tested.** The interface is in `coopnet/online.h` and isn't built.

## What already works online

`play-coop.bat steam-host` / `steam-join <SteamID64>` runs every co-op packet over Steam P2P (`coopnet/steamnet.c`).
Steam relays the traffic, so nobody has to forward ports. I tested it on one PC: one account talking to itself
(loopback), then two copies over UDP on localhost. I haven't tested it on two PCs with two accounts yet.

## What this step adds

1. **Lobbies and invites instead of Steam IDs.** The host creates a friends-only, 2-member lobby with
   `ISteamMatchmaking::CreateLobby` and opens the invite dialog (`ISteamFriends::ActivateGameOverlayInviteDialog`).
   The friend accepts in Steam. Steam starts the game with `+connect_lobby <id>` (or sends `GameLobbyJoinRequested_t`
   if it is already running), and coopnet joins the lobby and takes the owner's Steam ID as its peer. Rich presence
   `connect` lets friends join from the friends list too. These interfaces are in the same `steam_api.dll` that
   steamwrap already wraps, reached through the same flat `SteamAPI_*` exports `steamnet.c` uses.
2. **Handshake before sharing a world** (`OnlineHello`). Both games compare:
   - the mod version;
   - the `AI.exe` build;
   - the level they are in;
   - a checksum of the save they continue from.

   On a mismatch the overlay says what to fix, for example "Load the host's save", and no world events are applied.
   Right now nothing stops two players on different saves from replaying each other's events.
3. **Reliable channel for world events.** World events are sent 3 times on an unreliable channel today. Online,
   they move to `k_EP2PSendReliable` on a second channel. Presence, alien and life packets stay unreliable.
4. **Save hand-off.** The host can send its latest checkpoint save to the joiner. The saves in the test copy are 1 to 2 MB each. The joiner writes it into its own `coop_saves\<profile>`, never into Steam
   Cloud.
5. **Who owns what.** The host owns the alien director. The joiner applies the host's alien events and never sends
   its own (already true in `worldsync.c`). Each player owns their own life (`life.c`) and their own pickups.

## Tests to write before shipping it

- A fake `ISteamMatchmaking` in `coopnet/test/fake_steam.c`: create, join, leave, the owner changing, the lobby
  filling up.
- Handshake mismatch cases (offline, like `life_test.c`).
- A bad-network run: 200 ms latency and 5% loss, checking
  that doors, story steps and deaths still line up.
- A real two-PC, two-account session, logged on both sides.
