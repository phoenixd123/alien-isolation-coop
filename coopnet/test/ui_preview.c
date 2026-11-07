/* Renders the overlay's screens to BMP files (over a dark backdrop standing in for the game) so the UI can be
 * checked without running the game. Usage: ui_preview <out dir> [width height] */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ui.h"

static void save(const char *dir, const char *name, const uint32_t *ov, int W, int H) {
    uint32_t *img = malloc((size_t)W * H * 4);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            /* backdrop: a dim blue-grey corridor-ish gradient with a lighter centre */
            float dx = (x - W / 2.f) / W, dy = (y - H / 2.f) / H, v = 0.30f - (dx * dx + dy * dy) * 0.5f;
            if (v < 0.05f) v = 0.05f;
            int br = (int)(v * 120), bg = (int)(v * 140), bb = (int)(v * 150);
            uint32_t o = ov[(size_t)y * W + x], a = o >> 24;
            int r = (int)((o >> 16) & 255) + br * (255 - (int)a) / 255;
            int g = (int)((o >> 8) & 255) + bg * (255 - (int)a) / 255;
            int b = (int)(o & 255) + bb * (255 - (int)a) / 255;
            img[(size_t)y * W + x] = 0xFF000000u | (uint32_t)(r > 255 ? 255 : r) << 16 | (uint32_t)(g > 255 ? 255 : g) << 8 | (uint32_t)(b > 255 ? 255 : b);
        }
    char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\ui_%s.bmp", dir, name);
    FILE *f = fopen(p, "wb");
    BITMAPFILEHEADER fh = {0}; BITMAPINFOHEADER ih = {0};
    ih.biSize = sizeof ih; ih.biWidth = W; ih.biHeight = -H; ih.biPlanes = 1; ih.biBitCount = 32;
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + (DWORD)W * H * 4;
    fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f); fwrite(img, (size_t)W * H * 4, 1, f); fclose(f);
    free(img);
    printf("%s\n", p);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    int W = argc > 3 ? atoi(argv[2]) : 1920, H = argc > 3 ? atoi(argv[3]) : 1080;
    if (!ui_init()) { printf("GDI+ failed\n"); return 1; }
    uint32_t *buf = malloc((size_t)W * H * 4);
    UiFrame f;

    memset(&f, 0, sizeof f); f.W = W; f.H = H; f.now = 1234; f.peer_name = "Sam"; f.steam = 1; f.steam_id = 76561198000000001ull;
    ui_render(&f, buf, W, H); save(dir, "1_waiting", buf, W, H);

    memset(&f, 0, sizeof f); f.W = W; f.H = H; f.now = 1500; f.peer_name = "Sam"; f.linked = 1;
    ui_render(&f, buf, W, H); save(dir, "2_menu", buf, W, H);

    /* in a level: partner ahead, two motion contacts, the alien close */
    memset(&f, 0, sizeof f); f.W = W; f.H = H; f.now = 2300; f.peer_name = "Sam"; f.linked = 1; f.peer_live = 1; f.me_in_level = 1;
    f.me_f[0] = 0; f.me_f[1] = 1;
    f.mk_show = 1; f.mk_on = 1; f.mk_sx = W * 58 / 100; f.mk_sy = H * 46 / 100; f.mk_dist = 7.4f;
    f.blips[0].p[0] = 1.5f; f.blips[0].p[2] = 7.f; f.blips[0].kind = 1; f.blips[0].label = "Sam";
    f.blips[1].p[0] = -9; f.blips[1].p[2] = 12; f.blips[1].kind = 0;
    f.blips[2].p[0] = 6; f.blips[2].p[2] = -5; f.blips[2].kind = 0;
    f.nblips = 3;
    ui_render(&f, buf, W, H); save(dir, "3_level", buf, W, H);

    /* partner off-screen to the left, far apart */
    f.mk_on = 0; f.mk_dir[0] = -0.9f; f.mk_dir[1] = 0.1f; f.mk_dist = 46.2f; f.far_apart = 1; f.now = 2600;
    f.blips[0].p[0] = -40; f.blips[0].p[2] = 20;
    ui_render(&f, buf, W, H); save(dir, "4_offscreen", buf, W, H);

    /* partner just died */
    f.far_apart = 0; f.peer_dead = 1; f.flash_t = 2500; f.now = 2900; f.blips[0].kind = 2;
    ui_render(&f, buf, W, H); save(dir, "5_partner_down", buf, W, H);

    /* we are dead, following their camera, holding F9 */
    memset(&f, 0, sizeof f); f.W = W; f.H = H; f.now = 3000; f.peer_name = "Sam"; f.linked = 1; f.peer_live = 1;
    f.spect = 1; f.follow_on = 1; f.give_up = 0.45f;
    ui_render(&f, buf, W, H); save(dir, "6_spectating", buf, W, H);

    /* we are dead, online without their camera: bio-monitor with the alien near them */
    f.follow_on = 0; f.give_up = 0; f.peer_f[1] = 1; f.alien_ok = 1; f.alien_p[0] = 3; f.alien_p[2] = 4; f.now = 3300;
    ui_render(&f, buf, W, H); save(dir, "7_biomonitor", buf, W, H);

    free(buf);
    return 0;
}
