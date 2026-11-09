/* One alien for both players (alien.c). */
#pragma once
#include <stdint.h>
#include "proto.h"

int  alien_install(void (*logf)(const char *, ...), int host);   /* hooks; 0 if this build doesn't match */
int  alien_read(AlienExPkt *out);             /* host: our alien as of the last frame (net thread sends it); 1 if found */
void alien_from_host(const AlienExPkt *k);    /* joiner: the host's alien (net thread) */
void alien_partner_at(const float pos[3]);    /* host: where the partner is (net thread), so their running is heard */
int  alien_following(void);                   /* joiner: our alien is following the host's now */
extern uint32_t (*alien_body_hook)(void);     /* the partner's body in our game (skin.c), 0 if none */
