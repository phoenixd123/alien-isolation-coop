/* World sync by replaying level-script methods (see worldsync.c). */
#pragma once
#include <stdint.h>

typedef struct { uint32_t method, eid, type; uint32_t path[6]; int npath; } WorldEvt;

void ws_init(void (*logf)(const char *, ...));
void ws_set_host(int host);   /* host: sends alien-director events, never accepts them */
void ws_on_call(const uint32_t *msg, const uint32_t *target);   /* from the call_method hook */
int ws_pop(WorldEvt *ev);                                        /* next local change to send */
void ws_apply(const WorldEvt *ev);                               /* a change from the partner */
void ws_set_cutscene_cb(void (*cb)(void));
void ws_pause(int paused);   /* builds differ: neither send nor accept world events */
void ws_settle(uint32_t ms); /* a level just started: don't send for a while (its start-up scripts run on both sides) */   /* called (game thread) when a cutscene starts in our game */
