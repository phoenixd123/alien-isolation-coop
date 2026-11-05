/* Offline test of the co-op death rules (life.c) and the life/alien packets: two simulated players exchange
   state through a lossy "network" (every packet sent three times, some dropped). */
#include <stdio.h>
#include <string.h>
#include "../life.h"
#include "../proto.h"

static int fails, checks;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

typedef struct { Life L; int held, ran; } P;   /* held: a game over is waiting; ran: game overs that ran */

static void exchange(P *a, P *b, uint32_t now, int drop_a_to_b) {
    if (!drop_a_to_b) life_on_peer(&b->L, a->L.state, a->L.epoch, now);
    life_on_peer(&a->L, b->L.state, b->L.epoch, now);
}
static void die(P *p, uint32_t now) { if (life_on_gameover(&p->L, now)) p->held = 1; else p->ran++; }
static void tick(P *p, uint32_t now, int give_up) { if (life_tick(&p->L, now, give_up) && p->held) { p->held = 0; p->ran++; } }
static void run(P *a, P *b, uint32_t *now, uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 500) { *now += 500; exchange(a, b, *now, 0); tick(a, *now, 0); tick(b, *now, 0); }
}

int main(void) {
    uint32_t now = 1000;
    P a, b;

    /* 1. A dies, B is alive: A spectates until B dies, then both go back. */
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b); life_init(&a.L, now); life_init(&b.L, now);
    run(&a, &b, &now, 2000);
    die(&a, now);
    CHECK(a.held && a.L.state == LIFE_DEAD, "A holds game over while B lives");
    run(&a, &b, &now, 30000);
    CHECK(a.held && a.ran == 0, "A keeps spectating while B lives, however long");
    CHECK(b.L.ev_peer_died, "B is told that A died");
    die(&b, now);
    CHECK(!b.held && b.ran == 1 && b.L.ev_both_down, "B's game over runs at once: both down");
    run(&a, &b, &now, 1000);
    CHECK(!a.held && a.ran == 1, "A's held game over runs once B went back");
    CHECK(a.L.state == LIFE_RESTART && b.L.state == LIFE_RESTART, "both restarting");
    run(&a, &b, &now, LIFE_RESTART_MS + 1000);
    CHECK(a.L.state == LIFE_ALIVE && b.L.state == LIFE_ALIVE, "both alive after the reload");

    /* 2. Both die in the same tick (each still thinks the other is alive): both hold, then both release. */
    die(&a, now); die(&b, now);
    CHECK(a.held && b.held, "simultaneous deaths: both hold first");
    run(&a, &b, &now, 1000);
    CHECK(!a.held && !b.held && a.ran == 2 && b.ran == 2, "simultaneous deaths: both go back");
    run(&a, &b, &now, LIFE_RESTART_MS + 1000);

    /* 3. Partner disconnects while we spectate: back to the checkpoint after the timeout, not before. */
    die(&a, now);
    CHECK(a.held, "A holds");
    uint32_t t0 = now;
    while (now - t0 < LIFE_GONE_RELEASE_MS - 1000) { now += 500; tick(&a, now, 0); }
    CHECK(a.held, "not released before the partner timeout");
    while (now - t0 < LIFE_GONE_RELEASE_MS + LIFE_PEER_TIMEOUT_MS + 1000) { now += 500; tick(&a, now, 0); }
    CHECK(!a.held && a.ran == 3, "released after the partner is gone");
    run(&a, &b, &now, LIFE_RESTART_MS + 1000);
    CHECK(a.L.state == LIFE_ALIVE, "A alive again");

    /* 4. Give up while spectating. */
    die(&b, now); CHECK(b.held, "B holds");
    now += 500; tick(&b, now, 1);
    CHECK(!b.held && b.ran == 3, "give-up releases");
    run(&a, &b, &now, LIFE_RESTART_MS + 1000);

    /* 5. An old RESTART from the partner must not release a later death. */
    die(&a, now); CHECK(a.held, "A holds");
    life_on_peer(&a.L, LIFE_RESTART, b.L.epoch, now);   /* stale: same epoch B had when A died */
    now += 500; tick(&a, now, 0);
    CHECK(a.held, "stale RESTART ignored");
    life_on_peer(&a.L, LIFE_RESTART, b.L.epoch + 1, now);
    now += 500; tick(&a, now, 0);
    CHECK(!a.held, "newer RESTART releases");
    run(&a, &b, &now, LIFE_RESTART_MS + 1000);

    /* 6. Solo (no partner ever seen): game over runs normally. */
    P s; memset(&s, 0, sizeof s); life_init(&s.L, now);
    die(&s, now);
    CHECK(!s.held && s.ran == 1, "solo: game over runs at once");

    /* 7. A partner who is restarting counts as coming back: we hold and watch them. */
    memset(&a, 0, sizeof a); life_init(&a.L, now);
    life_on_peer(&a.L, LIFE_RESTART, 5, now);
    die(&a, now);
    CHECK(!a.held, "partner restarting (not alive yet): our game over runs too");

    /* 8. The player picks a save on the game-over menu while spectating: counts as going back, partner keeps playing. */
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b); life_init(&a.L, now); life_init(&b.L, now);
    run(&a, &b, &now, 2000);
    die(&a, now); CHECK(a.held, "A waits on the menu");
    life_on_respawn(&a.L, now); a.held = 0;
    CHECK(a.L.state == LIFE_RESTART, "respawn on our own: restarting");
    run(&a, &b, &now, LIFE_RESTART_MS + 1000);
    CHECK(a.L.state == LIFE_ALIVE && b.L.state == LIFE_ALIVE && b.ran == 0, "partner unaffected, both alive");
    life_on_respawn(&a.L, now);
    CHECK(a.L.state == LIFE_ALIVE, "respawn while alive is ignored");

    /* 9. Packets. */
    LifePkt lp; proto_fill_life(&lp, LIFE_DEAD, 7, 1234, 3);
    CHECK(proto_check_life(&lp, sizeof lp), "life packet round trip");
    CHECK(!proto_check_life(&lp, sizeof lp - 1), "life packet: wrong length rejected");
    lp.state = 9; CHECK(!proto_check_life(&lp, sizeof lp), "life packet: bad state rejected");
    proto_fill_life(&lp, LIFE_DEAD, 7, 1234, 3); lp.epoch ^= 1;
    CHECK(!proto_check_life(&lp, sizeof lp), "life packet: corruption rejected");
    AlienPkt ap; float pos[3] = {1.5f, 2.f, -3.f};
    proto_fill_alien(&ap, 9, pos, 1);
    CHECK(proto_check_alien(&ap, sizeof ap) && ap.flags == 1 && ap.pos[2] == -3.f, "alien packet round trip");
    ap.pos[0] = 0.f / 0.f;
    CHECK(!proto_check_alien(&ap, sizeof ap), "alien packet: NaN rejected");
    PresencePkt pp; float q[4] = {0, 0, 0, 1};
    proto_fill(&pp, 1, pos, q, 1, "x");
    CHECK(!proto_check_life((const LifePkt *)&pp, sizeof pp) && !proto_check_alien((const AlienPkt *)&pp, sizeof pp),
          "presence packet is not taken for life/alien");

    printf("life_test: %d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
