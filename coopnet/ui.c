/* ui - overlay drawing, on the shared drawing layer in gfx.h. */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "gfx.h"
#include "ui.h"
#include "hud.h"

int ui_init(void) { return gfx_init(); }


/* ---------- widgets ---------- */

/* Motion tracker scope. Forward is up; turns with your view. */
static void tracker(Ctx *c, float cx, float cy, float R, const float at[3], const float fw[2], const UiBlip *bl, int nb, const char *title) {
    float s = c->s;
    float pad = 14 * s, top = 26 * s, bot = 30 * s;
    panel(c, cx - R - pad, cy - R - top, 2 * (R + pad), 2 * R + top + bot, GREEN);
    text(c, title, cx - R - pad + 10 * s, cy - R - top + 7 * s, 12 * s, GREEN_LO, 1, 3 * s, LEFT, 0);
    char rng[16]; snprintf(rng, sizeof rng, "%dM", (int)TRACK_RANGE);
    text(c, rng, cx + R + pad - 10 * s, cy - R - top + 7 * s, 12 * s, GREEN_LO, 1, 2 * s, RIGHT, 0);

    disc(c, C(150, 2, 20, 12), cx, cy, R);
    GdipSetPenDashStyle(c->pen, 2);
    for (int i = 1; i < 3; i++) ring(c, with_a(GREEN, 50), 1, cx, cy, R * i / 3.f);
    GdipSetPenDashStyle(c->pen, 0);
    ring(c, with_a(GREEN, 150), 1.6f * s, cx, cy, R);
    line(c, with_a(GREEN, 36), 1, cx - R, cy, cx + R, cy); line(c, with_a(GREEN, 36), 1, cx, cy - R, cx, cy + R);
    /* view cone */
    float ca = 0.5f, sa = 0.866f;
    line(c, with_a(GREEN, 60), 1, cx, cy, cx - R * ca, cy - R * sa); line(c, with_a(GREEN, 60), 1, cx, cy, cx + R * ca, cy - R * sa);
    /* sweep with a fading trail (GDI+ angles: 0 = east, clockwise) */
    float a = (float)(c->now % 2000u) / 2000.f * 360.f - 90.f;
    for (int i = 0; i < 8; i++) {
        GdipSetSolidFillColor(c->br, with_a(GREEN, 34 - i * 4));
        GdipFillPie(c->g, c->br, cx - R, cy - R, 2 * R, 2 * R, a - (i + 1) * 5.f, 5.f);
    }
    float ar = a * 3.14159265f / 180.f;
    line(c, with_a(GREEN, 200), 1.5f * s, cx, cy, cx + cosf(ar) * R, cy + sinf(ar) * R);
    /* you */
    PtF me[3] = {{cx, cy - 7 * s}, {cx - 5 * s, cy + 5 * s}, {cx + 5 * s, cy + 5 * s}};
    poly(c, GREEN, me, 3);

    float nearest = -1;
    for (int i = 0; i < nb; i++) {
        float x, y, d = hud_track(at, fw, bl[i].p, &x, &y);
        int edge = 0;
        if (d > TRACK_RANGE) {
            if (!bl[i].label) continue;
            float n = sqrtf(x * x + y * y); x /= n; y /= n; edge = 1;
        }
        float px = cx + x * (R - 6 * s), py = cy - y * (R - 6 * s);
        ARGB col = bl[i].kind == 3 ? RED : bl[i].kind == 2 ? RED : bl[i].kind == 1 ? GREEN : INK;
        if ((bl[i].kind == 0 || bl[i].kind == 3) && !edge && (nearest < 0 || d < nearest)) nearest = d;
        float pulse = (float)((c->now + i * 137u) % 900u) / 900.f;
        if (!edge) ring(c, with_a(col, (int)(140 * (1 - pulse))), 1.2f * s, px, py, (4 + 10 * pulse) * s);
        disc(c, with_a(col, 70), px, py, (edge ? 5 : 7) * s);
        disc(c, col, px, py, (edge ? 2.5f : 3.5f) * s);
        if (bl[i].label) {
            char l[48], u[32]; upper(u, sizeof u, bl[i].label);
            snprintf(l, sizeof l, "%s %.0fM", u, d);
            int right = px > cx;
            text(c, l, px + (right ? -9 : 9) * s, py - 7 * s, 11 * s, col, 1, 1 * s, right ? RIGHT : LEFT, 1);
        }
    }
    /* big readout of the nearest moving contact, like the handheld tracker */
    char rd[24];
    if (nearest >= 0) snprintf(rd, sizeof rd, "%04.1fM", nearest); else snprintf(rd, sizeof rd, "--.-M");
    text(c, rd, cx, cy + R + 6 * s, 18 * s, nearest >= 0 && nearest < 8 ? RED : GREEN, 1, 2 * s, CENTER, 1);
}

/* Partner marker in the 3D view: brackets around them, or a chevron at the screen edge. */
static void marker(Ctx *c, const UiFrame *f) {
    float s = c->s;
    char name[24], dist[24]; upper(name, sizeof name, f->peer_name); snprintf(dist, sizeof dist, "%.1fM", f->mk_dist);
    if (f->mk_on) {
        float r = 900.f * s / (f->mk_dist + 3.f); if (r < 10 * s) r = 10 * s; if (r > 48 * s) r = 48 * s;
        float x = (float)f->mk_sx, y = (float)f->mk_sy;
        brackets(c, with_a(GREEN, 70), x - r - 1, y - r - 1, 2 * r + 2, 2 * r + 2, r * 0.45f, 4 * s);
        brackets(c, GREEN, x - r, y - r, 2 * r, 2 * r, r * 0.4f, 1.6f * s);
        disc(c, GREEN, x, y, 2 * s);
        text(c, name, x, y + r + 6 * s, 14 * s, GREEN, 1, 2 * s, CENTER, 1);
        text(c, dist, x, y + r + 23 * s, 12 * s, GREEN_LO, 1, 1 * s, CENTER, 0);
        return;
    }
    float ex = f->mk_dir[0], ey = f->mk_dir[1], n = sqrtf(ex * ex + ey * ey);
    if (n < 1e-4f) { ex = 1; ey = 0; n = 1; }
    ex /= n; ey /= n;
    float cx = c->W * 0.5f, cy = c->H * 0.5f, m = 56 * s;
    float k = fminf((cx - m) / fmaxf(fabsf(ex), 1e-3f), (cy - m) / fmaxf(fabsf(ey), 1e-3f));
    float ax = cx + ex * k, ay = cy - ey * k, dx = ex, dy = -ey;   /* screen direction */
    float L = 20 * s, Wd = 12 * s;
    PtF glow[3] = {{ax + dx * (L + 4 * s), ay + dy * (L + 4 * s)}, {ax - dy * (Wd + 4 * s), ay + dx * (Wd + 4 * s)}, {ax + dy * (Wd + 4 * s), ay - dx * (Wd + 4 * s)}};
    PtF tri[3] = {{ax + dx * L, ay + dy * L}, {ax - dy * Wd, ay + dx * Wd}, {ax + dy * Wd, ay - dx * Wd}};
    poly(c, with_a(GREEN, 60), glow, 3);
    poly(c, GREEN, tri, 3);
    float lx = ax - dx * 30 * s, ly = ay - dy * 30 * s - 8 * s;
    int al = dx > 0.3f ? RIGHT : dx < -0.3f ? LEFT : CENTER;
    text(c, name, lx, ly, 14 * s, GREEN, 1, 2 * s, al, 1);
    text(c, dist, lx, ly + 17 * s, 12 * s, GREEN_LO, 1, 1 * s, al, 0);
}

/* Status chip, top left: link state in one or two lines. */
static void chip(Ctx *c, const char *head, ARGB hc, const char *l1, ARGB c1, const char *l2, ARGB c2, int blink_dot) {
    float s = c->s, x = 24 * s, y = 24 * s;
    float w = text_w(c, head, 12 * s, 1, 3 * s) + 40 * s;
    if (l1) { float t = text_w(c, l1, 15 * s, 1, 2 * s) + 32 * s; if (t > w) w = t; }
    if (l2) { float t = text_w(c, l2, 13 * s, 0, 1 * s) + 32 * s; if (t > w) w = t; }
    float h = (l2 ? 72 : l1 ? 52 : 30) * s;
    panel(c, x, y, w, h, hc);
    int on = !blink_dot || (c->now / 500u) % 2;
    if (on) disc(c, hc, x + 16 * s, y + 14 * s, 3.5f * s);
    text(c, head, x + 26 * s, y + 7 * s, 12 * s, hc, 1, 3 * s, LEFT, 0);
    if (l1) text(c, l1, x + 16 * s, y + 26 * s, 15 * s, c1, 1, 2 * s, LEFT, 1);
    if (l2) text(c, l2, x + 16 * s, y + 48 * s, 13 * s, c2, 0, 1 * s, LEFT, 0);
}

/* Banner across the top centre: small tracked label above a big line. */
static void banner(Ctx *c, const char *label, const char *big, ARGB col, const char *sub) {
    float s = c->s, cx = c->W * 0.5f;
    float w = text_w(c, big, 26 * s, 1, 6 * s) + 120 * s, w2 = sub ? text_w(c, sub, 13 * s, 0, 2 * s) + 80 * s : 0;
    if (w2 > w) w = w2;
    float h = (sub ? 92 : 70) * s, y = 22 * s;
    panel(c, cx - w / 2, y, w, h, col);
    fill(c, col, cx - w / 2, y, w, 2 * s);
    text(c, label, cx, y + 10 * s, 12 * s, with_a(col, 210), 1, 5 * s, CENTER, 0);
    text(c, big, cx, y + 28 * s, 26 * s, col, 1, 6 * s, CENTER, 1);
    if (sub) text(c, sub, cx, y + 66 * s, 13 * s, with_a(INK, 200), 0, 2 * s, CENTER, 0);
}

static void hint_f9(Ctx *c, float give_up) {
    float s = c->s, cx = c->W * 0.5f, y = c->H - 70 * s;
    text(c, "HOLD F9 TO RETURN TO THE LAST CHECKPOINT", cx, y, 13 * s, with_a(INK, 170), 0, 2 * s, CENTER, 0);
    if (give_up > 0.01f) {
        float w = 320 * s;
        fill(c, C(150, 0, 0, 0), cx - w / 2, y + 22 * s, w, 6 * s);
        fill(c, AMBER, cx - w / 2, y + 22 * s, w * (give_up > 1 ? 1 : give_up), 6 * s);
    }
}

/* ---------- screens ---------- */
void ui_render(const UiFrame *f, uint32_t *argb, int W, int H) {
    memset(argb, 0, (size_t)W * H * 4);
    if (!g_ok || W <= 0 || H <= 0) return;
    GpBitmap *bmp = NULL; Ctx c = {0};
    if (GdipCreateBitmapFromScan0(W, H, W * 4, PARGB32, (BYTE *)argb, &bmp) != 0) return;
    GdipGetImageGraphicsContext(bmp, &c.g);
    GdipSetSmoothingMode(c.g, 4); GdipSetTextRenderingHint(c.g, 4);
    GdipCreateSolidFill(0, &c.br); GdipCreatePen1(0, 1, UNIT_PX, &c.pen);
    c.W = W; c.H = H; c.now = f->now; c.s = (float)H / 1080.f; if (c.s < 0.85f) c.s = 0.85f;   /* text stays readable at 720p */
    float s = c.s;
    char a[96], b[96], nm[24]; upper(nm, sizeof nm, f->peer_name ? f->peer_name : "partner");

    if (f->spect && f->follow_on) {
        /* dead, seeing through the partner's eyes: a camera feed with a frame and a live tag */
        brackets(&c, with_a(RED, 150), 18 * s, 18 * s, W - 36 * s, H - 36 * s, 60 * s, 2 * s);
        if ((f->now / 600u) % 2) disc(&c, RED, 44 * s, 46 * s, 6 * s);
        text(&c, "LIVE FEED", 58 * s, 38 * s, 14 * s, RED, 1, 4 * s, LEFT, 1);
        snprintf(a, sizeof a, "SPECTATING %s", nm);
        banner(&c, "YOU ARE DEAD", a, RED, "WHEN THEY DIE TOO, YOU BOTH GO BACK TO THE LAST CHECKPOINT");
        hint_f9(&c, f->give_up);
        vignette(&c, RED, 55, 150 * s);
    } else if (f->spect) {
        if (!f->linked) banner(&c, "YOU ARE DEAD", "SIGNAL LOST", RED, "PARTNER DISCONNECTED - RETURNING TO THE CHECKPOINT");
        else if (f->peer_dead) banner(&c, "YOU ARE DEAD", "BOTH DOWN", RED, "RETURNING TO THE LAST CHECKPOINT");
        else { snprintf(a, sizeof a, "WATCHING %s", nm); banner(&c, "YOU ARE DEAD", a, RED, "WHEN THEY DIE TOO, YOU BOTH GO BACK TO THE LAST CHECKPOINT"); }
        if (!f->thumb_ok && f->peer_live) {
            UiBlip pb[1]; int np = 0;
            if (f->alien_ok) { memcpy(pb[0].p, f->alien_p, 12); pb[0].kind = 3; pb[0].label = "alien"; np = 1; }
            float R = (H < W ? H : W) * 0.28f;
            snprintf(a, sizeof a, "BIO-MONITOR  %s", nm);
            tracker(&c, W * 0.5f, H * 0.5f + 40 * s, R, f->peer_p, f->peer_f, pb, np, a);
            if (f->alien_ok) {
                float dx = f->alien_p[0] - f->peer_p[0], dz = f->alien_p[2] - f->peer_p[2], d = sqrtf(dx * dx + dz * dz);
                if (d < 8) snprintf(b, sizeof b, "IT IS RIGHT ON THEM"); else snprintf(b, sizeof b, "ALIEN %.0fM FROM THEM", d);
                if (d >= 8 || (f->now / 300u) % 2) text(&c, b, W * 0.5f, H * 0.5f + 40 * s + R + 70 * s, 18 * s, d < 8 ? RED : AMBER, 1, 4 * s, CENTER, 1);
            }
        }
        if (f->linked && !f->peer_dead) hint_f9(&c, f->give_up);
    } else if (!f->linked) {
        snprintf(a, sizeof a, "WAITING FOR %s", nm);
        if (f->steam && f->steam_id) snprintf(b, sizeof b, "YOUR STEAM ID  %llu", (unsigned long long)f->steam_id);
        else if (f->steam) snprintf(b, sizeof b, "CONNECTING TO STEAM...");
        chip(&c, "CO-OP LINK", AMBER, a, INK, f->steam ? b : NULL, with_a(INK, 190), 1);
    } else if (!f->me_in_level) {
        snprintf(a, sizeof a, "%s CONNECTED", nm);
        chip(&c, "CO-OP LINK", GREEN, a, INK, "LOAD A SAVE TO PLAY TOGETHER", with_a(INK, 190), 0);
    } else {
        if (f->mk_show && !f->peer_dead) marker(&c, f);
        float R = H / 9.f; if (R < 60) R = 60;
        tracker(&c, 24 * s + 14 * s + R, H - 130.f * H / 1080.f - 30 * s - R, R,   /* above the game's health bar */ f->me_p, f->me_f, f->blips, f->nblips, "MOTION");
        if (f->peer_dead) {
            snprintf(a, sizeof a, "%s IS DOWN", nm);
            banner(&c, "WARNING", a, RED, "THEY ARE WATCHING YOU. STAY ALIVE.");
        } else if (f->far_apart) {
            snprintf(a, sizeof a, "%s  OUT OF RANGE", nm);
            chip(&c, "SIGNAL WEAK", AMBER, a, AMBER, NULL, 0, 1);
        }
    }
    if (f->flash_t && f->now - f->flash_t < 2000u) vignette(&c, RED, (int)(130 * (1.f - (f->now - f->flash_t) / 2000.f)), 170 * s);
    if (f->both_t && f->now - f->both_t < 5000u && !f->spect) banner(&c, "BOTH DOWN", "RETURNING TO CHECKPOINT", RED, NULL);

    GdipDeleteBrush(c.br); GdipDeletePen(c.pen); GdipDeleteGraphics(c.g); GdipDisposeImage(bmp);
}
