/* lobbynet - the LAN side of the lobby: UDP sockets, broadcast, fingerprints, save copy over TCP, game launch. */
#pragma once
#include <stdint.h>
#include "lobbyproto.h"

typedef struct {
    uintptr_t sock;        /* all lobby traffic (ephemeral port) */
    uintptr_t disc;        /* listens for beacons on the discovery port (0 if that failed) */
    uintptr_t tcp;         /* host: serves the save folder on the same port number as `sock` */
    uint16_t port, disc_port;
    int local_only;        /* tests: bind 127.0.0.1 and broadcast only to 127.0.0.1 */
    uint32_t bcast[16]; int nbcast;
} LbNet;

int  lbnet_open(LbNet *n, uint16_t disc_port, int local_only);   /* 0 on failure */
void lbnet_close(LbNet *n);
void lbnet_send(void *ctx, LbAddr dest, const void *buf, int len);   /* LbSend for lobbyproto.h */
int  lbnet_recv(LbNet *n, LbAddr *from, void *buf, int cap);         /* one packet from either socket, 0 if none */
int  lbnet_parse_ip(const char *s, uint32_t *ip);                     /* "192.168.1.20" */
void lbnet_ip_str(uint32_t ip, char *out, int cap);
uint32_t lbnet_random(void);
uint32_t lbnet_local_ip(void);                                      /* first LAN address, 0 if none */

/* Fingerprints for the compatibility check (0 if the file or folder is missing). */
uint32_t lb_hash_file(const char *path);
uint32_t lb_game_build(const char *ai_exe);            /* PE timestamp and image size of AI.exe */
uint32_t lb_hash_saves(const char *folder);           /* names and contents of the save files */

/* Save copy. The host serves its co-op save folder on TCP (same port number as the lobby socket); the joiner
 * fetches it, after moving its own saves to coop_saves\backup\<profile>-<time>\.
 * Only plain file names (letters, digits, '_', '.'), at most 32 files of at most 32 MB each. */
int  lbnet_serve_saves(LbNet *n, const char *folder, uint32_t lobby); /* host: start serving (thread) */
void lbnet_stop_saves(LbNet *n);
typedef struct { volatile int state; volatile int files, done_files; volatile uint32_t bytes, total; char msg[96]; } LbCopy;
enum { LBC_IDLE, LBC_RUNNING, LBC_DONE, LBC_FAILED };
/* The copy over any byte stream (TCP here, Steam P2P in lobbysteam.c). send/recv: all n bytes or 0. */
typedef struct LbStream { int (*send)(struct LbStream *, const void *, int); int (*recv)(struct LbStream *, void *, int); void *ctx; } LbStream;
void lbnet_serve_stream(LbStream *st, const char *folder, uint32_t lobby);
void lbnet_fetch_stream(LbStream *st, uint32_t lobby, const char *folder, const char *backup, LbCopy *c);
/* joiner: copy from host into `folder`, backing up to `backup_root`. Runs on a thread; watch c->state. */
int  lbnet_fetch_saves(LbAddr host, uint32_t lobby, const char *folder, const char *backup_root, LbCopy *c);
int  lb_save_name_ok(const char *s);

/* Internet play over Steam: your own Steam ID (from the running Steam client, 0 if it isn't running) and a
   friend's typed in (17 digits starting 7656119). */
uint64_t lbnet_my_steam_id(void);
int  lb_parse_steam_id(const char *s, uint64_t *id);

/* Launch AI.exe from `game_dir` with the co-op settings in its environment. 0 on failure.
   steam: over Steam P2P instead of the LAN (host, or join `peer_steam`). */
typedef struct { int host; uint32_t host_ip; const char *name, *skin, *profile; int skins; int steam; uint64_t peer_steam; } LbLaunch;
int  lb_launch_game(const char *game_dir, const LbLaunch *l, char *err, int errcap);
