/* lobbysteam - Steam lobbies for the lobby window (lobbysteam.c): host a friends-only lobby, see which friends are
 * hosting one, join, invite, and carry the lobby's own packets and the save copy over Steam P2P. */
#pragma once
#include <stdint.h>
#include "lobbyproto.h"
#include "lobbynet.h"

enum { LBS_OFF, LBS_IDLE, LBS_CREATING, LBS_JOINING, LBS_IN, LBS_FAILED };

typedef struct { uint64_t id, lobby; char name[32]; int online, hosting; } LbsFriend;

int  lbs_init(const char *game_dir, void (*logf)(const char *, ...));   /* 1 if Steam is up for us */
void lbs_shutdown(void);              /* leave the lobby and let go of Steam (before the game starts) */
int  lbs_ready(void);
uint64_t lbs_self(void);
void lbs_tick(void);                  /* callbacks, P2P sessions, friends list; call every frame */

int  lbs_host(void);                  /* create a friends-only lobby for two */
int  lbs_join(uint64_t lobby);
void lbs_leave(void);
int  lbs_state(void);
uint64_t lbs_lobby(void);
uint64_t lbs_owner(void);             /* the host's Steam ID once in a lobby */
uint64_t lbs_take_invite(void);       /* a Steam invite the player accepted (lobby ID), 0 if none */
int  lbs_invite(uint64_t friend_id);
int  lbs_friends(LbsFriend *out, int max);   /* online friends, the ones hosting first */

/* Lobby packets over P2P: a Steam account as an LbAddr (port LBS_PORT marks it). */
#define LBS_PORT 0x5354u
static inline LbAddr lbs_addr(uint64_t id) { LbAddr a = {(uint32_t)id, LBS_PORT}; return a; }
static inline uint64_t lbs_id(LbAddr a) { return 0x0110000100000000ull | a.ip; }
static inline int lbs_is_steam(LbAddr a) { return a.port == LBS_PORT; }
void lbs_send(void *ctx, LbAddr dest, const void *buf, int len);
int  lbs_recv(LbAddr *from, void *buf, int cap);

/* Save copy over P2P, same format as the LAN copy. */
void lbs_serve_saves(const char *folder, uint32_t lobby);   /* host: answer requests while in a lobby */
int  lbs_fetch_saves(uint64_t host, uint32_t lobby, const char *folder, const char *backup_root, LbCopy *c);
