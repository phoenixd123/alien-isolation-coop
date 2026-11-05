/* Offline test of the motion-tracker math (hud.h). */
#include <stdio.h>
#include <math.h>
#include "../hud.h"

static int fails, checks;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-3f)

int main(void) {
    float f[2], x, y, me[3] = {10, 2, 10};
    float id[4] = {0, 0, 0, 1};
    hud_forward(id, f);
    CHECK(NEAR(f[0], 0) && NEAR(f[1], 1), "identity faces +Z");
    float ahead[3] = {10, 5, 20};
    float d = hud_track(me, f, ahead, &x, &y);
    CHECK(NEAR(d, 10) && NEAR(x, 0) && NEAR(y, 10 / TRACK_RANGE), "point ahead is up on the tracker, height ignored");
    float right[3] = {15, 2, 10};
    hud_track(me, f, right, &x, &y);
    CHECK(NEAR(x, 5 / TRACK_RANGE) && NEAR(y, 0), "+X is right when facing +Z");
    /* yaw 90 deg: q = (0, sin45, 0, cos45) faces +X (view.h convention) */
    float s = sinf(3.14159265f / 4), c = cosf(3.14159265f / 4), yaw[4] = {0, s, 0, c};
    hud_forward(yaw, f);
    CHECK(NEAR(f[0], 1) && NEAR(f[1], 0), "yaw 90 faces +X");
    hud_track(me, f, right, &x, &y);
    CHECK(NEAR(x, 0) && NEAR(y, 5 / TRACK_RANGE), "facing +X, a point at +X is ahead");
    hud_track(me, f, ahead, &x, &y);
    CHECK(NEAR(x, -10 / TRACK_RANGE) && NEAR(y, 0), "facing +X, +Z is to the left");
    /* pitch only (looking up 30 deg) keeps the flat forward */
    float p30[4] = {-sinf(0.2618f), 0, 0, cosf(0.2618f)};
    hud_forward(p30, f);
    CHECK(NEAR(f[0], 0) && NEAR(f[1], 1), "pitch does not turn the tracker");

    TrackSlot t = {0}; float p[3] = {0, 0, 0};
    hud_motion_update(&t, p, 1000);
    CHECK(!hud_motion_visible(&t, 1000), "new contact is not motion yet");
    p[0] = 0.1f; hud_motion_update(&t, p, 2100);
    CHECK(!hud_motion_visible(&t, 2100), "creeping 0.1 m/s does not show");
    p[0] = 1.1f; hud_motion_update(&t, p, 3200);
    CHECK(hud_motion_visible(&t, 3200), "walking shows");
    CHECK(hud_motion_visible(&t, 4600) && !hud_motion_visible(&t, 4800), "blip fades 1.5 s after the last move");
    p[0] = 200.f; hud_motion_update(&t, p, 4400);
    CHECK(t.moved == 0, "teleport is not motion");

    CHECK(hud_ping_ms(TRACK_RANGE + 1) == 0, "out of range: silent");
    CHECK(hud_ping_ms(2) < hud_ping_ms(20), "closer pings faster");
    printf("hud_test: %d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
