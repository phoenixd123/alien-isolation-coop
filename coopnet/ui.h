/* ui - the in-game overlay, drawn with GDI+ into a premultiplied ARGB buffer (per-pixel alpha).
 * Pure drawing: coopnet.c fills a UiFrame from its state, ui_render() paints it. The preview tool
 * (test/ui_preview.c) renders the same frames to images without the game. */
#pragma once
#include <stdint.h>

#define UI_MAX_BLIPS 24

typedef struct { float p[3]; int kind; const char *label; } UiBlip;   /* kind: 0 motion, 1 partner, 2 partner down, 3 alien */

typedef struct {
    int W, H;
    uint32_t now;                 /* ms, any clock (animations) */

    /* link */
    int linked;                   /* partner's packets are arriving */
    int peer_live;                /* partner is in a level, position valid */
    int me_in_level;
    const char *peer_name;        /* "partner" if unknown */
    int steam; uint64_t steam_id; /* waiting screen: show our Steam ID */

    /* partner marker (3D view) */
    int mk_show, mk_on;           /* on = inside the view; otherwise an edge arrow */
    int mk_sx, mk_sy; float mk_dist; float mk_dir[2];   /* dir: camera-space x,y of the partner (arrow) */

    /* motion tracker */
    float me_p[3], me_f[2];
    UiBlip blips[UI_MAX_BLIPS]; int nblips;
    int far_apart;                /* > 40 m: "signal weak" */

    /* death and spectating */
    int spect;                    /* we are dead */
    int follow_on;                /* our game shows the partner's camera */
    int thumb_ok;                 /* couch: partner's window shown under us */
    int peer_dead, both_dead;
    float give_up;                /* F9 hold progress 0..1 */
    uint32_t flash_t, both_t;     /* partner just died / both down (ms, same clock as now; 0 = never) */
    float peer_p[3], peer_f[2];   /* bio-monitor */
    int alien_ok; float alien_p[3];
    uint32_t cut_t;               /* moved to the partner for a cutscene (same clock as now; 0 = never) */
} UiFrame;

int ui_init(void);                                             /* GDI+ startup; 0 on failure */
void ui_render(const UiFrame *f, uint32_t *argb, int W, int H); /* argb: top-down premultiplied, stride W*4 */
