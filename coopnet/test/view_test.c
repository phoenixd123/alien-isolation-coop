/* Offline test of the overlay projection (coopnet/view.h), using conventions measured in game. */
#include <stdio.h>
#include <math.h>
#include "../view.h"

static int pass, fail;
#define CHECK(cond, ...) do { if (cond) pass++; else { fail++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void yawq(float deg, float q[4]) { float h = deg * 3.14159265f / 360.f; q[0] = 0; q[1] = sinf(h); q[2] = 0; q[3] = cosf(h); }

int main(void) {
    const int W = 1280, H = 720;
    float id[4] = {0, 0, 0, 1}, q[4];
    float cam[3] = {37.27f, 5.08f, -9.46f};

    /* identity faces +Z: a point straight ahead is at screen centre */
    float ahead[3] = {cam[0], cam[1], cam[2] + 5};
    ViewMark m = view_project(cam, id, ahead, W, H, 1);
    CHECK(m.on && abs(m.sx - W / 2) <= 1 && abs(m.sy - H / 2) <= 1, "ahead -> centre, got on=%d (%d,%d)", m.on, m.sx, m.sy);
    CHECK(fabsf(m.dist - 5) < 1e-3f, "dist 5, got %f", m.dist);

    /* +X is right, +Y is up */
    float right[3] = {cam[0] + 1, cam[1], cam[2] + 5}, up[3] = {cam[0], cam[1] + 1, cam[2] + 5};
    m = view_project(cam, id, right, W, H, 1); CHECK(m.on && m.sx > W / 2 + 50, "right of view -> right half, sx=%d", m.sx);
    m = view_project(cam, id, up, W, H, 1);    CHECK(m.on && m.sy < H / 2 - 50, "above view -> upper half, sy=%d", m.sy);

    /* behind -> off screen */
    float behind[3] = {cam[0], cam[1], cam[2] - 5};
    m = view_project(cam, id, behind, W, H, 1); CHECK(!m.on && m.cam[2] < 0, "behind -> off, on=%d z=%f", m.on, m.cam[2]);

    /* measured: yaw -72 deg walked along (sin -72, 0, cos -72): that direction must be screen centre */
    yawq(-72.f, q);
    float t = -72.f * 3.14159265f / 180.f, walk[3] = {cam[0] + 4 * sinf(t), cam[1], cam[2] + 4 * cosf(t)};
    m = view_project(cam, q, walk, W, H, 1);
    CHECK(m.on && abs(m.sx - W / 2) <= 2 && abs(m.sy - H / 2) <= 2, "yaw -72 walk dir -> centre, got (%d,%d)", m.sx, m.sy);

    /* turning right (mouse +x) raised yaw: an object at yaw +30 must appear right of centre when facing yaw 0 */
    float r30[3] = {cam[0] + 4 * sinf(0.5236f), cam[1], cam[2] + 4 * cosf(0.5236f)};
    m = view_project(cam, id, r30, W, H, 1); CHECK(m.on && m.sx > W / 2, "yaw +30 object -> right, sx=%d", m.sx);

    /* real pair from a test run: p2 at spawn facing +Z sees p1 ahead, slightly right */
    float p1[3] = {39.34f, 5.07f, -2.32f};
    m = view_project(cam, id, p1, W, H, 1);
    CHECK(m.on && m.sx > W / 2 && m.sx < W * 3 / 4, "p1 from p2 spawn: ahead-right, got on=%d sx=%d", m.on, m.sx);

    /* measured look-up quaternion: forward tilts to +Y, so a point level with the eye drops below centre */
    float lq[4] = {-0.16777f, -0.57503f, -0.12190f, 0.79141f};
    float lvl[3] = {cam[0] + 4 * sinf(t), cam[1], cam[2] + 4 * cosf(t)};
    /* (the measured pitch is ~24 deg, just over half the 47 deg vfov, so the point sits at the bottom edge) */
    m = view_project(cam, lq, lvl, W, H, 1); CHECK(m.cam[1] < 0 && m.cam[2] > 0 && m.sy > H - 40, "looking up -> level point at bottom, sy=%d", m.sy);

    /* fwd_sign -1 flips ahead/behind */
    m = view_project(cam, id, ahead, W, H, -1); CHECK(!m.on, "fwd_sign -1 flips");

    printf("view_test: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
