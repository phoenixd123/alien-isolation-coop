/* Steam P2P transport for coopnet (ISteamNetworking005 of the game's own steam_api.dll).
 * Traffic goes by SteamID through Valve's relay when needed: no IP, no port forwarding. */
#pragma once
#include <stdint.h>

typedef void (*steamnet_log_fn)(const char *, ...);

/* peer: SteamID64 to talk to, or 0 to accept the first player who connects (host).
   Returns 1 once Steam is up and the transport is ready; call again until it does. */
int steamnet_try_init(uint64_t peer, steamnet_log_fn logf);
int steamnet_send(const void *data, uint32_t n);
/* Returns the packet length (0 if none). Packets from anyone but the peer are dropped. */
int steamnet_recv(void *buf, uint32_t cap);
void steamnet_set_peer(uint64_t id);   /* fix the partner after init (tests) */
uint64_t steamnet_self(void);   /* own SteamID64, 0 until ready */
uint64_t steamnet_peer(void);   /* current peer, 0 while a host waits */
const char *steamnet_persona(void);   /* own Steam name, "" until ready */
/* Calls Steam's callbacks deliver: once per net tick, on the net thread. */
void steamnet_poll(void);
