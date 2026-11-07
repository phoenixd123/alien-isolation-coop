/* gfx - the drawing layer shared by the in-game overlay (ui.c) and the lobby (lobby/lobbyui.c).
 * GDI+ flat API (gdiplus.dll ships with Windows), declared here so no SDK headers are needed.
 * Look: the game's late-70s terminals. Phosphor green on dark glass, monospaced caps, thin frames with corner
 * brackets, scanlines, a soft glow. Amber for warnings, red for death and the alien. */
#pragma once
#include <windows.h>
#include <stdint.h>
#include <string.h>

/* ---------- GDI+ flat API ---------- */
typedef float REAL;
typedef uint32_t ARGB;
typedef int GpStatus;
typedef void GpGraphics, GpBitmap, GpBrush, GpPen, GpFont, GpFontFamily, GpStringFormat;
typedef struct { REAL X, Y; } PtF;
typedef struct { REAL X, Y, Width, Height; } RcF;
typedef struct { UINT32 ver; void *dbg; BOOL no_thread; BOOL no_codecs; } GdipStartIn;
#define GP WINAPI
GpStatus GP GdiplusStartup(ULONG_PTR *, const GdipStartIn *, void *);
GpStatus GP GdipCreateBitmapFromScan0(INT, INT, INT, INT, BYTE *, GpBitmap **);
GpStatus GP GdipGetImageGraphicsContext(GpBitmap *, GpGraphics **);
GpStatus GP GdipDisposeImage(GpBitmap *);
GpStatus GP GdipDeleteGraphics(GpGraphics *);
GpStatus GP GdipSetSmoothingMode(GpGraphics *, INT);
GpStatus GP GdipSetTextRenderingHint(GpGraphics *, INT);
GpStatus GP GdipGraphicsClear(GpGraphics *, ARGB);
GpStatus GP GdipCreateSolidFill(ARGB, GpBrush **);
GpStatus GP GdipSetSolidFillColor(GpBrush *, ARGB);
GpStatus GP GdipDeleteBrush(GpBrush *);
GpStatus GP GdipCreatePen1(ARGB, REAL, INT, GpPen **);
GpStatus GP GdipSetPenColor(GpPen *, ARGB);
GpStatus GP GdipSetPenWidth(GpPen *, REAL);
GpStatus GP GdipSetPenDashStyle(GpPen *, INT);
GpStatus GP GdipDeletePen(GpPen *);
GpStatus GP GdipFillRectangle(GpGraphics *, GpBrush *, REAL, REAL, REAL, REAL);
GpStatus GP GdipDrawLine(GpGraphics *, GpPen *, REAL, REAL, REAL, REAL);
GpStatus GP GdipFillEllipse(GpGraphics *, GpBrush *, REAL, REAL, REAL, REAL);
GpStatus GP GdipDrawEllipse(GpGraphics *, GpPen *, REAL, REAL, REAL, REAL);
GpStatus GP GdipDrawArc(GpGraphics *, GpPen *, REAL, REAL, REAL, REAL, REAL, REAL);
GpStatus GP GdipFillPie(GpGraphics *, GpBrush *, REAL, REAL, REAL, REAL, REAL, REAL);
GpStatus GP GdipFillPolygon(GpGraphics *, GpBrush *, const PtF *, INT, INT);
GpStatus GP GdipCreateLineBrush(const PtF *, const PtF *, ARGB, ARGB, INT, GpBrush **);
GpStatus GP GdipCreateFontFamilyFromName(const WCHAR *, void *, GpFontFamily **);
GpStatus GP GdipCreateFont(const GpFontFamily *, REAL, INT, INT, GpFont **);
GpStatus GP GdipDeleteFont(GpFont *);
GpStatus GP GdipStringFormatGetGenericTypographic(GpStringFormat **);
GpStatus GP GdipDrawString(GpGraphics *, const WCHAR *, INT, const GpFont *, const RcF *, const GpStringFormat *, const GpBrush *);
GpStatus GP GdipMeasureString(GpGraphics *, const WCHAR *, INT, const GpFont *, const RcF *, const GpStringFormat *, RcF *, INT *, INT *);

#define PARGB32 0x000E200B
#define UNIT_PX 2

/* ---------- palette ---------- */
static inline ARGB C(int a, int r, int g, int b) { return (ARGB)a << 24 | (ARGB)r << 16 | (ARGB)g << 8 | (ARGB)b; }
static inline ARGB with_a(ARGB c, int a) { return (c & 0xFFFFFF) | (ARGB)(a < 0 ? 0 : a > 255 ? 255 : a) << 24; }
#define GREEN    C(255, 132, 244, 176)
#define GREEN_LO C(255, 58, 132, 96)
#define INK      C(255, 220, 238, 226)
#define AMBER    C(255, 245, 186, 80)
#define RED      C(255, 255, 76, 60)
#define GLASS    C(196, 5, 14, 10)

/* ---------- drawing context ---------- */
static int g_ok;
static GpFontFamily *g_fam;
static GpStringFormat *g_fmt;
typedef struct { GpGraphics *g; GpBrush *br; GpPen *pen; float s; int W, H; uint32_t now; } Ctx;

static inline int gfx_init(void) {
    if (g_ok) return 1;
    ULONG_PTR tok; GdipStartIn in = {1, NULL, FALSE, FALSE};
    if (GdiplusStartup(&tok, &in, NULL) != 0) return 0;
    const WCHAR *fams[] = {L"Consolas", L"Lucida Console", L"Courier New"};
    for (int i = 0; i < 3 && !g_fam; i++) GdipCreateFontFamilyFromName(fams[i], NULL, &g_fam);
    GdipStringFormatGetGenericTypographic(&g_fmt);
    g_ok = g_fam && g_fmt;
    return g_ok;
}

static inline void fill(Ctx *c, ARGB col, float x, float y, float w, float h) { GdipSetSolidFillColor(c->br, col); GdipFillRectangle(c->g, c->br, x, y, w, h); }
static inline void line(Ctx *c, ARGB col, float w, float x0, float y0, float x1, float y1) {
    GdipSetPenColor(c->pen, col); GdipSetPenWidth(c->pen, w); GdipDrawLine(c->g, c->pen, x0, y0, x1, y1);
}
static inline void disc(Ctx *c, ARGB col, float x, float y, float r) { GdipSetSolidFillColor(c->br, col); GdipFillEllipse(c->g, c->br, x - r, y - r, 2 * r, 2 * r); }
static inline void ring(Ctx *c, ARGB col, float w, float x, float y, float r) {
    GdipSetPenColor(c->pen, col); GdipSetPenWidth(c->pen, w); GdipDrawEllipse(c->g, c->pen, x - r, y - r, 2 * r, 2 * r);
}
static inline void poly(Ctx *c, ARGB col, const PtF *p, int n) { GdipSetSolidFillColor(c->br, col); GdipFillPolygon(c->g, c->br, p, n, 0); }

/* Corner brackets of a box, the frame motif. */
static inline void brackets(Ctx *c, ARGB col, float x, float y, float w, float h, float len, float t) {
    line(c, col, t, x, y, x + len, y); line(c, col, t, x, y, x, y + len);
    line(c, col, t, x + w, y, x + w - len, y); line(c, col, t, x + w, y, x + w, y + len);
    line(c, col, t, x, y + h, x + len, y + h); line(c, col, t, x, y + h, x, y + h - len);
    line(c, col, t, x + w, y + h, x + w - len, y + h); line(c, col, t, x + w, y + h, x + w, y + h - len);
}

/* A glass panel: dark translucent fill, scanlines, hairline frame, bright corner brackets. */
static inline void panel(Ctx *c, float x, float y, float w, float h, ARGB accent) {
    fill(c, GLASS, x, y, w, h);
    float step = 3.f * c->s < 2.f ? 2.f : 3.f * c->s;
    for (float yy = y + 1; yy < y + h; yy += step) fill(c, C(34, 0, 0, 0), x, yy, w, 1);
    GdipSetPenColor(c->pen, with_a(accent, 70)); GdipSetPenWidth(c->pen, 1);
    line(c, with_a(accent, 70), 1, x, y, x + w, y); line(c, with_a(accent, 70), 1, x, y + h, x + w, y + h);
    line(c, with_a(accent, 70), 1, x, y, x, y + h); line(c, with_a(accent, 70), 1, x + w, y, x + w, y + h);
    brackets(c, accent, x, y, w, h, 10 * c->s, 2 * c->s);
}

/* ---------- text: monospaced caps with tracking and a soft phosphor glow ---------- */
#define NFONT 8
static struct { int px, bold; GpFont *f; float adv; } g_fonts[NFONT];
static inline GpFont *font(Ctx *c, float px, int bold, float *adv) {
    int ip = (int)(px + 0.5f); if (ip < 8) ip = 8;
    for (int i = 0; i < NFONT; i++) if (g_fonts[i].f && g_fonts[i].px == ip && g_fonts[i].bold == bold) { *adv = g_fonts[i].adv; return g_fonts[i].f; }
    int slot = 0; for (int i = 0; i < NFONT; i++) if (!g_fonts[i].f) { slot = i; break; }
    if (g_fonts[slot].f) GdipDeleteFont(g_fonts[slot].f);
    GpFont *f = NULL; GdipCreateFont(g_fam, (REAL)ip, bold ? 1 : 0, UNIT_PX, &f);
    RcF lay = {0, 0, 1000, 1000}, bb = {0};
    GdipMeasureString(c->g, L"MMMMMMMMMM", 10, f, &lay, g_fmt, &bb, NULL, NULL);
    g_fonts[slot].px = ip; g_fonts[slot].bold = bold; g_fonts[slot].f = f; g_fonts[slot].adv = bb.Width / 10.f;
    *adv = g_fonts[slot].adv;
    return f;
}

enum { LEFT, CENTER, RIGHT };
/* Draws s (ASCII) at x,y (top), tracking `trk` px between letters. Returns the width. */
static inline float text(Ctx *c, const char *s, float x, float y, float px, ARGB col, int bold, float trk, int align, int glow) {
    float adv; GpFont *f = font(c, px, bold, &adv);
    int n = (int)strlen(s); if (n > 120) n = 120;
    float w = n * (adv + trk) - trk;
    if (align == CENTER) x -= w / 2; else if (align == RIGHT) x -= w;
    WCHAR ws[2] = {0};
    if (glow) {
        GdipSetSolidFillColor(c->br, with_a(col, 38));
        float o = 1.2f * c->s;
        for (int i = 0; i < n; i++) {
            if (s[i] == ' ') continue;
            ws[0] = (WCHAR)(unsigned char)s[i];
            for (int k = 0; k < 4; k++) {
                RcF r = {x + i * (adv + trk) + (k == 0 ? -o : k == 1 ? o : 0), y + (k == 2 ? -o : k == 3 ? o : 0), adv * 2, px * 2};
                GdipDrawString(c->g, ws, 1, f, &r, g_fmt, c->br);
            }
        }
    }
    GdipSetSolidFillColor(c->br, col);
    for (int i = 0; i < n; i++) {
        if (s[i] == ' ') continue;
        ws[0] = (WCHAR)(unsigned char)s[i];
        RcF r = {x + i * (adv + trk), y, adv * 2, px * 2};
        GdipDrawString(c->g, ws, 1, f, &r, g_fmt, c->br);
    }
    return w;
}
static inline float text_w(Ctx *c, const char *s, float px, int bold, float trk) {
    float adv; font(c, px, bold, &adv); int n = (int)strlen(s); return n * (adv + trk) - trk;
}
static inline void upper(char *d, size_t n, const char *s) {
    size_t i = 0; for (; s && s[i] && i + 1 < n; i++) d[i] = (s[i] >= 'a' && s[i] <= 'z') ? (char)(s[i] - 32) : s[i];
    d[i] = 0;
}

/* Edge vignette (red flash, death): linear gradients from each edge inward. */
static inline void vignette(Ctx *c, ARGB col, int a, float depth) {
    if (a <= 0) return;
    float W = (float)c->W, H = (float)c->H;
    struct { PtF p0, p1; float x, y, w, h; } e[4] = {
        {{0, 0}, {0, depth}, 0, 0, W, depth}, {{0, H}, {0, H - depth}, 0, H - depth, W, depth},
        {{0, 0}, {depth, 0}, 0, 0, depth, H}, {{W, 0}, {W - depth, 0}, W - depth, 0, depth, H}};
    for (int i = 0; i < 4; i++) {
        GpBrush *lb = NULL;
        if (GdipCreateLineBrush(&e[i].p0, &e[i].p1, with_a(col, a), with_a(col, 0), 3, &lb) == 0) {
            GdipFillRectangle(c->g, lb, e[i].x, e[i].y, e[i].w, e[i].h); GdipDeleteBrush(lb);
        }
    }
}
