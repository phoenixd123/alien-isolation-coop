/* Camera math for the overlay. Pure functions, shared by the DLL and the offline tests.
 *
 * Convention, measured in game (build 4940658, 2026-11-01, see NOTES):
 *   rec[0..3] is the view rotation quaternion (x,y,z,w); identity looks along world +Z, +Y is up, +X is right.
 *   Yaw is a rotation about Y: q=(0,sin(t/2),0,cos(t/2)) faces (sin t, 0, cos t). Looking up adds an X component.
 */
#pragma once
#include <math.h>

/* rotate v by conj(q): world -> camera */
static inline void view_rot_inv(const float q[4], const float v[3], float o[3]) {
    float x = -q[0], y = -q[1], z = -q[2], w = q[3];
    float tx = 2.f * (y * v[2] - z * v[1]), ty = 2.f * (z * v[0] - x * v[2]), tz = 2.f * (x * v[1] - y * v[0]);
    o[0] = v[0] + w * tx + (y * tz - z * ty);
    o[1] = v[1] + w * ty + (z * tx - x * tz);
    o[2] = v[2] + w * tz + (x * ty - y * tx);
}

#define VIEW_AY 2.299843f   /* 1/tan(47deg/2): vertical FOV from rec[7] */

typedef struct { int on; int sx, sy; float cam[3]; float dist; } ViewMark;

/* Project a world point for a camera at pos/q onto a W x H screen. fwd_sign flips the forward axis (debug). */
static inline ViewMark view_project(const float pos[3], const float q[4], const float target[3], int W, int H, int fwd_sign) {
    ViewMark m = {0};
    float d[3] = {target[0] - pos[0], target[1] - pos[1], target[2] - pos[2]};
    m.dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    view_rot_inv(q, d, m.cam);
    m.cam[2] *= (float)fwd_sign;
    if (m.cam[2] > 0.05f) {
        float ax = VIEW_AY * (float)H / (float)W;
        float nx = ax * m.cam[0] / m.cam[2], ny = VIEW_AY * m.cam[1] / m.cam[2];
        m.sx = (int)((nx * 0.5f + 0.5f) * (float)W);
        m.sy = (int)((0.5f - ny * 0.5f) * (float)H);
        m.on = m.sx >= 0 && m.sx < W && m.sy >= 0 && m.sy < H;
    }
    return m;
}
