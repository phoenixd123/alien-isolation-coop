/* Offline checks for automatic world sync (the generated syncmap.h) and the joiner's alien playback. */
#include <stdio.h>
#include <math.h>
#include "../syncmap.h"
#include "../alien_interp.h"

static int fails, passes;
#define CHECK(c, m) do { if (c) passes++; else { fails++; printf("FAIL: %s\n", m); } } while (0)
static int near3(const float *a, float x, float y, float z) { return fabsf(a[0] - x) < 1e-3f && fabsf(a[1] - y) < 1e-3f && fabsf(a[2] - z) < 1e-3f; }

int main(void) {
    /* table: sorted, unique, every entry found by the lookup, both kinds present */
    int sorted = 1, found = 1, k1 = 0, k2 = 0;
    for (int i = 0; i < SYNCMAP_N; i++) {
        if (i && !(SYNCMAP_KEY[i - 1][0] < SYNCMAP_KEY[i][0] || (SYNCMAP_KEY[i - 1][0] == SYNCMAP_KEY[i][0] && SYNCMAP_KEY[i - 1][1] < SYNCMAP_KEY[i][1]))) sorted = 0;
        if (syncmap_kind(SYNCMAP_KEY[i][0], SYNCMAP_KEY[i][1]) != SYNCMAP_KIND[i]) found = 0;
        k1 += SYNCMAP_KIND[i] == 1; k2 += SYNCMAP_KIND[i] == 2;
    }
    CHECK(SYNCMAP_N > 1000, "table has the level scripts' entities");
    CHECK(sorted, "sorted and unique");
    CHECK(found, "every entry found");
    CHECK(k1 > 100 && k2 > 1000, "both kinds");
    /* known entities */
    CHECK(syncmap_kind(0x143a5b75u, 0x26dbce33u) == 1, "Door_Mechanism IsCuttingCompleted (persistent) is STATE");
    CHECK(syncmap_kind(0x143a5b75u, 0x09de34aeu) == 0, "Door_Mechanism IsLocked (not persistent) isn't automatic");
    CHECK(syncmap_kind(0x37849153u, 0x9ba70bc8u) == 0, "KeyPadHacking LogicOnce_3 (reset on each use) left out");
    CHECK(syncmap_kind(0xef3722dcu, 0x92d9160au) == 0, "SwitchLevel itself isn't automatic (it has its own rule)");
    /* misses */
    int miss = 1;
    for (uint32_t x = 1; x < 2000; x++) if (syncmap_kind(x * 2654435761u, x * 40503u) && syncmap_kind(x * 2654435761u, x * 40503u) != 0) { miss = 0; break; }
    CHECK(miss, "random IDs are not shared");
    CHECK(syncmap_kind(0, 0) == 0 && syncmap_kind(0xFFFFFFFFu, 0xFFFFFFFFu) == 0, "edges");

    /* alien playback */
    AlienTrack a; memset(&a, 0, sizeof a); float o[3];
    CHECK(!alien_track_at(&a, 100, o), "no samples, no position");
    float p0[3] = {0, 0, 0}, p1[3] = {1, 0, 2}, p2[3] = {2, 0, 4};
    alien_track_push(&a, 1000, p0); alien_track_push(&a, 1033, p1); alien_track_push(&a, 1066, p2);
    CHECK(alien_track_at(&a, 1000, o) && near3(o, 0, 0, 0), "on a sample");
    CHECK(alien_track_at(&a, 1016, o) && fabsf(o[0] - 16.f / 33.f) < 1e-3f && fabsf(o[2] - 32.f / 33.f) < 1e-3f, "between samples");
    CHECK(alien_track_at(&a, 1050, o) && o[0] > 1.f && o[0] < 2.f, "second span");
    CHECK(alien_track_at(&a, 5000, o) && near3(o, 2, 0, 4), "newer than all: newest, no guessing ahead");
    CHECK(alien_track_at(&a, 10, o) && near3(o, 0, 0, 0), "older than all: oldest");
    float back[3] = {9, 9, 9};
    alien_track_push(&a, 1040, back);
    CHECK(alien_track_at(&a, 5000, o) && near3(o, 2, 0, 4), "late sample ignored");
    /* a vent jump: no sliding across the map */
    float far[3] = {40, 0, 4};
    alien_track_push(&a, 1100, far);
    CHECK(alien_track_at(&a, 1090, o) && near3(o, 2, 0, 4), "jump: stays put until the new sample's time");
    CHECK(alien_track_at(&a, 1100, o) && near3(o, 40, 0, 4), "jump: then snaps");
    /* buffer wraps */
    for (int i = 0; i < 20; i++) { float q[3] = {(float)i, 1, 0}; alien_track_push(&a, 2000u + (uint32_t)i * 33u, q); }
    CHECK(a.n == ALIEN_SAMPLES, "buffer wraps");
    CHECK(alien_track_at(&a, 2000u + 19u * 33u - 16u, o) && o[0] > 18.f && o[0] < 19.f, "after wrap");
    /* clock wrap */
    AlienTrack w; memset(&w, 0, sizeof w);
    alien_track_push(&w, 0xFFFFFFF0u, p0); alien_track_push(&w, 0x00000010u, p2);
    CHECK(alien_track_at(&w, 0, o) && fabsf(o[0] - 1.f) < 1e-3f, "across the 49-day tick wrap");

    printf("sync_test: %d/%d checks passed\n", passes, passes + fails);
    return fails != 0;
}
