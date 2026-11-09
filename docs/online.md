# Online play: plan

Status: the **LAN lobby is built** and works in game on one PC. **Steam lobbies are built** (`lobby/lobbysteam.c`), not run with Steam yet.

## LAN lobby (done)

`lobby.exe` is a launcher window. `lobby/lobbyproto.h` is the protocol and state machine, with no sockets, so the
Steam version can reuse it; `lobby/lobbynet.c` is the LAN transport.

- The host broadcasts a beacon on UDP 45501 every second, to 255.255.255.255 and to each network adapter's own
  broadcast address (VPN LANs often drop the first). *Join by address* sends a probe to that address and the
  host answers with its beacon.
- The joiner sends HELLO every 0.5 s and the host answers with STATE (both players, ready flags, countdown).
  6 s of silence counts as leaving. Two players per lobby; a third is told it's full. The host can kick.
- Each side sends fingerprints: a hash of `coopnet.dll`, the `AI.exe` build (PE timestamp and image size),
  and a hash of the co-op save folder. Different mod or game builds block Start; different saves give a warning
  and a **Copy host's save** button (TCP; the joiner's own save is moved to `coop_saves\backup\` first).
- Start runs a 3 s countdown on both sides, then each lobby launches `AI.exe` with the same settings
  `play-coop.bat` would set (`AIC_COOP_MODE`, `AIC_COOP_PEER`, name, skin).
- Every packet carries a checksum and a protocol version; anything else is dropped.

`lobby/test/lobby_test.c` runs the state machine with three players on a fake network, the real UDP path on
loopback, and a real save copy over TCP with a backup check.
Not tested yet: two PCs, and a game started from the lobby.

## What already works online

`play-coop.bat steam-host` / `steam-join <SteamID64>` runs every co-op packet over Steam P2P (`coopnet/steamnet.c`).
Steam relays the traffic, so nobody has to forward ports. I tested it on one PC: one account talking to itself
(loopback), then two copies over UDP on localhost. I haven't tested it on two PCs with two accounts yet.

## Steam lobbies (built, not run with Steam yet)

`lobby/lobbysteam.c`. The lobby window loads the game's own `steam_api` (`steam_api_real.dll` once the mod is
installed) and starts it with the game's app ID, so Steam sees you playing Alien: Isolation while it's open.

- **Host:** `CreateLobby` (friends-only, two members), tagged `aic=1`.
- **Finding it:** a friend in one of the game's lobbies shows it in their game info (`GetFriendGamePlayed`), so the
  Internet tab lists online friends with the ones hosting first, each with **Join**. No lobby browser and no IDs.
- **Invites:** the lobby screen lists online friends with **Invite** (`InviteUserToLobby`). Accepting the invite
  in Steam while the lobby window is open joins it (`GameLobbyJoinRequested_t`). Steam's own invite dialog isn't
  used: it's part of the overlay, which doesn't draw on the lobby's plain window.
- **Then the same lobby protocol as on a LAN** (`lobbyproto.h`), carried over Steam P2P on channel 6: a Steam
  account is an `LbAddr` with port `LBS_PORT`, so one state machine handles both, and LAN broadcasts stay on the
  LAN. The joiner sends the host a probe over Steam and joins the beacon that comes back, like *Join by address*.
- **Save copy** uses the LAN copy's format and code (`lbnet_serve_stream` / `lbnet_fetch_stream`) over a reliable
  P2P stream on channel 5, with the same backup of the joiner's save.
- **Start:** both windows leave the Steam lobby and shut Steam down, then start the game in Steam mode connected
  each other over Steam (the joiner to the host's Steam ID in `AIC_COOP_PEER_STEAM`), the same as
  `play-coop.bat steam-host` / `steam-join`.

Method slots come from the Steamworks headers for the interface versions the game's `steam_api.dll` has:
SteamMatchMaking009, SteamFriends014, SteamNetworking005.

## Still planned

1. **Joining from an invite with the lobby closed.** Steam then starts the game itself with `+connect_lobby <id>`;
   coopnet could pick that up and open the lobby window. For now, open `lobby.exe` first.
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
