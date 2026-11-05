/* Co-op death rules (see life.c). Pure logic: no Windows calls, so test/life_test.c runs it offline. */
#pragma once
#include <stdint.h>

enum { LIFE_ALIVE = 0, LIFE_DEAD = 1, LIFE_RESTART = 2 };   /* also the wire values in LifePkt.state */

#define LIFE_PEER_TIMEOUT_MS  3000    /* no life packet for this long: partner is gone */
#define LIFE_GONE_RELEASE_MS 10000    /* spectating a partner who is gone this long: go back to the checkpoint */
#define LIFE_RESTART_MS       6000    /* after our game over runs, count as alive again after this */

typedef struct {
    int state;                 /* ours */
    uint32_t epoch;            /* times we went back to a checkpoint (sent to the partner) */
    uint32_t t_state;          /* when `state` last changed */
    int peer_known;
    int peer_state;
    uint32_t peer_epoch, peer_t;
    uint32_t peer_epoch_at_death;   /* partner's epoch when we died: a newer RESTART means they went back too */
    int ev_peer_died;          /* one-shot: partner just died (for the HUD and sound) */
    int ev_both_down;          /* one-shot: both are down, going back to the checkpoint */
} Life;

void life_init(Life *L, uint32_t now);
int life_peer_live(const Life *L, uint32_t now);
/* The player died. The game then waits on its own game-over menu. Returns 1 while we should keep waiting there
   and spectate the partner, 0 to go back to the checkpoint now ("Load Current Save"). */
int life_on_gameover(Life *L, uint32_t now);
/* The game reloaded on its own while we were spectating (the player picked a save on the menu). */
void life_on_respawn(Life *L, uint32_t now);
void life_on_peer(Life *L, int state, uint32_t epoch, uint32_t now);
/* Returns 1 when a waiting player must go back to the checkpoint now (both down, partner gone, or gave up). */
int life_tick(Life *L, uint32_t now, int give_up);
