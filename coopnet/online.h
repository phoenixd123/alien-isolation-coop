/*
 * Lobbies/invites - not implemented yet, just the interface (plan in docs/online.md).
 * Replaces "send your friend your Steam ID" with Steam's lobby + invite flow, plus a handshake before syncing.
 */
#pragma once
#include <stdint.h>

typedef enum {
    ONLINE_IDLE = 0,
    ONLINE_CREATING_LOBBY,   /* host: ISteamMatchmaking::CreateLobby(friends-only, 2 members) */
    ONLINE_IN_LOBBY,         /* waiting for the partner; rich presence "connect" lets friends join from Steam */
    ONLINE_HANDSHAKE,        /* both sides compare build, mod version, level and save checkpoint */
    ONLINE_PLAYING,
    ONLINE_FAILED
} OnlineState;

typedef struct {
    uint32_t mod_version;    /* AIC_VERSION and a build number */
    uint32_t exe_hash;       /* first 4 bytes of AI.exe's SHA-256: both must run the same build */
    uint32_t level_hash;     /* hash of the loaded level name, 0 on the menu */
    uint32_t checkpoint;     /* save slot checksum: both must continue from the same save */
} OnlineHello;

/* Host: create a friends-only lobby and open the Steam overlay's invite dialog. */
int online_host(void);
/* Join a lobby (from an invite, or from the +connect_lobby command line Steam passes on "Join game"). */
int online_join_lobby(uint64_t lobby_id);
/* Called every network tick: runs Steam callbacks for lobby events, then the handshake. */
void online_tick(void);
OnlineState online_state(void);
/* Short text for the overlay ("Inviting...", "Joined Ripley's game", "Different save: load the host's save"). */
const char *online_status(void);
void online_leave(void);
