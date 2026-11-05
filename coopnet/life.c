/*
 * Co-op death: when you die, you watch your partner until they die too, then both games go back to their last
 * checkpoint.
 *
 * Observed in game (2026-11-04): a death runs TriggerDeath on the player's Health_Damage_Death script, then the engine
 * shows its game-over menu ("Load Current Save" selected) and waits there; nothing reloads until a choice is made.
 * probe.c reports the death; life_on_gameover() decides:
 *   - partner alive and connected: state DEAD. The game stays on its menu and the HUD shows the partner.
 *   - partner already down, or no partner: state RESTART, and coopnet picks "Load Current Save". The partner sees
 *     RESTART (or DEAD) and picks it too, so both reload their checkpoint at the same time.
 * A waiting player also goes back if the partner disconnects for LIFE_GONE_RELEASE_MS, or gives up (F9, or picking a
 * save on the menu themselves: life_on_respawn).
 * Epochs keep an old RESTART packet from releasing a later death.
 */
#include <string.h>
#include "life.h"

static void set_state(Life *L, int s, uint32_t now) { L->state = s; L->t_state = now; }

void life_init(Life *L, uint32_t now) { memset(L, 0, sizeof *L); L->t_state = now; }

int life_peer_live(const Life *L, uint32_t now) { return L->peer_known && now - L->peer_t < LIFE_PEER_TIMEOUT_MS; }

static void go_restart(Life *L, uint32_t now) { L->epoch++; set_state(L, LIFE_RESTART, now); }

int life_on_gameover(Life *L, uint32_t now) {
    if (L->state == LIFE_DEAD) return 1;   /* already holding one; hold repeats too */
    if (life_peer_live(L, now) && L->peer_state == LIFE_ALIVE) {
        set_state(L, LIFE_DEAD, now);
        L->peer_epoch_at_death = L->peer_epoch;
        return 1;
    }
    if (life_peer_live(L, now) && L->peer_state == LIFE_DEAD) L->ev_both_down = 1;
    go_restart(L, now);
    return 0;
}

void life_on_peer(Life *L, int state, uint32_t epoch, uint32_t now) {
    if (L->peer_known && state == LIFE_DEAD && L->peer_state != LIFE_DEAD) L->ev_peer_died = 1;
    if (!L->peer_known && state == LIFE_DEAD) L->ev_peer_died = 1;
    L->peer_known = 1; L->peer_state = state; L->peer_epoch = epoch; L->peer_t = now;
}

void life_on_respawn(Life *L, uint32_t now) {
    if (L->state == LIFE_DEAD) go_restart(L, now);
}

int life_tick(Life *L, uint32_t now, int give_up) {
    if (L->state == LIFE_RESTART && now - L->t_state >= LIFE_RESTART_MS) { set_state(L, LIFE_ALIVE, now); return 0; }
    if (L->state != LIFE_DEAD) return 0;
    int release = 0;
    if (L->peer_known && L->peer_state == LIFE_DEAD && life_peer_live(L, now)) release = 1;                 /* both down */
    if (L->peer_known && L->peer_state == LIFE_RESTART && L->peer_epoch != L->peer_epoch_at_death) release = 1;   /* they went back */
    if (!life_peer_live(L, now) && now - (L->peer_known ? L->peer_t : L->t_state) >= LIFE_GONE_RELEASE_MS) release = 1;
    if (give_up) release = 1;
    if (!release) return 0;
    if (L->peer_state == LIFE_DEAD || L->peer_state == LIFE_RESTART) L->ev_both_down = 1;
    go_restart(L, now);
    return 1;
}
