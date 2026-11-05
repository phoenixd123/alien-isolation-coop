/* Run a level-script method on an entity named by its level-file path (see replay.c). */
#pragma once
#include <stdint.h>

/* path[0] = ID of the composite-instance entity that owns the target, path[1] its owner, ... up to the root. */
typedef struct {
    uint32_t method, eid, type; uint32_t path[8]; int npath;
    int (*owner_ok)(uint32_t owner_type);   /* optional: the owner composite's type must pass */
    int via_hook;                           /* tests: dispatch through the hook so capture sees it */
} ReplayKey;
void replay_set_hook(uint32_t(__attribute__((thiscall)) *hook)(void *, void *, void *, void *));

void replay_init(void (*logf)(const char *, ...));
int replay_request(const ReplayKey *k);   /* resolve in the background, then run on the next dispatch */
uint32_t replay_resolve(const ReplayKey *k);
int replay_post_raw(uint32_t hb, const uint32_t raw[10], uint32_t want_type);   /* re-run a held message */
int replay_active(void);                  /* 1 while a replayed call runs (echo guard) */
void replay_pump(void *self, uint32_t(__attribute__((thiscall)) *tramp)(void *, void *, void *, void *), const void *cur_msg);
