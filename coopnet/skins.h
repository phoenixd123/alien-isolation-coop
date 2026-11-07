/* Skins: which character your partner shows up as in your game.
 *
 * A skin is a character model the level already ships with (a DisplayModel:<NAME> composite) spawned as a
 * CIVILIAN through the engine's own spawn-request handler. The spawn request layout, the template values and
 * the engine addresses come from AI-MP (https://github.com/Dropper7856/AIMP), "AI-MP — originally created by
 * Dropper7856", used under its non-commercial attribution license (see third_party/AIMP-LICENSE.txt). AI-MP
 * spawns the Nostromo crew from the Crew Expendable DLC; this mod uses the story characters each level has.
 *
 * Pure code (no Windows calls): shared by skin.c and test/skin_test.c.
 */
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    const char *name;        /* display_model, also the name players pick */
    const char *anim_set, *skeleton;
    uint32_t display_guid;   /* ShortGuid of the DisplayModel:<name> composite (same in every level) */
    int experimental;        /* only used when picked explicitly */
    const char *levels;      /* levels that ship the model, space separated with a leading and trailing space */
} Skin;

/* Generated from the level scripts (levelmod ids/funcs over every level). */
static const Skin SKINS[] = {
    {"TAYLOR", "TAYLOR", "TAYLOR", 0x6d1dcf62u, 0, " BSP_TORRENS HAB_SHOPPINGCENTRE SCI_HUB SOLACE "},
    {"SAMUELS", "SAMUELS", "SAMUELS", 0xc03257c5u, 0, " BSP_TORRENS HAB_SHOPPINGCENTRE SCI_ANDROIDLAB SCI_HUB "},
    {"AXEL", "AXEL", "AXEL", 0x6b02f8d6u, 0, " HAB_AIRPORT SOLACE "},
    {"RICARDO", "RICARDO", "RICARDO", 0x43ce0a75u, 0, " HAB_SHOPPINGCENTRE SCI_HUB TECH_COMMS TECH_HUB "},
    {"MARLOW", "MARLOW", "MARLOW", 0x94fe9e3au, 0, " HAB_SHOPPINGCENTRE SOLACE "},
    {"WAIT", "WAIT", "WAIT", 0x6be388a8u, 0, " HAB_SHOPPINGCENTRE SCI_HUB "},
    {"VERLAINE", "VERLAINE", "VERLAINE", 0xecbb96cbu, 0, " BSP_TORRENS "},
    {"CONNOR", "CONNOR", "CONRAD", 0x6f9b8b67u, 0, " BSP_TORRENS "},
    {"DR_KUHLMAN", "DR_KUHLMAN", "DR_KUHLMAN", 0x9f0d98f5u, 0, " SCI_HOSPITALUPPER "},
    {"SPACESUIT_HEYST", "SPACESUIT_HEYST", "SPACESUIT_HEYST", 0x50346ffau, 0, " BSP_LV426_PT01 BSP_LV426_PT02 "},
    {"SPACESUIT_MEEKS", "SPACESUIT_MEEKS", "SPACESUIT_MEEKS", 0x2e66a1c7u, 0, " BSP_LV426_PT01 BSP_LV426_PT02 "},
    {"ANDROID", "ANDROID", "ANDROID", 0x3704e3bbu, 1,
     " ENG_REACTORCORE ENG_TOWPLATFORM HAB_AIRPORT HAB_SHOPPINGCENTRE SCI_ANDROIDLAB SCI_HOSPITALLOWER SCI_HOSPITALUPPER SCI_HUB TECH_COMMS TECH_HUB TECH_MUTHRCORE TECH_RND TECH_RND_HZDLAB "},
};
#define NSKINS ((int)(sizeof SKINS / sizeof SKINS[0]))

/* FNV-1a 32: the engine's hash for anim set, skeleton and model names. */
static inline uint32_t skin_hash(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}

static inline const Skin *skin_find(const char *name) {
    if (!name || !*name) return NULL;
    for (int i = 0; i < NSKINS; i++) if (!strcmp(SKINS[i].name, name)) return &SKINS[i];
    return NULL;
}

/* Skins copied into levels by tools/skinprep, from coop_skins.txt ("LEVEL SKIN SKIN ..." per line), kept as
   " LEVEL:SKIN " tokens. */
static char skin_extra[8192];
static inline int skin_tok_end(char c) { return c == 0 || c == ' ' || c == 9 || c == 10 || c == 13; }
static inline void skin_extra_add_line(const char *line) {
    char lv[48]; int n = 0;
    while (*line == ' ') line++;
    while (!skin_tok_end(*line) && n < 47) lv[n++] = *line++;
    lv[n] = 0;
    if (!n) return;
    for (;;) {
        while (*line == ' ' || *line == 9) line++;
        char sk[32]; int m = 0;
        while (!skin_tok_end(*line) && m < 31) sk[m++] = *line++;
        sk[m] = 0;
        if (!m) break;
        size_t L = strlen(skin_extra);
        if (L + (size_t)n + (size_t)m + 4 < sizeof skin_extra) {
            if (!L) skin_extra[L++] = ' ';
            memcpy(skin_extra + L, lv, (size_t)n); L += (size_t)n; skin_extra[L++] = ':';
            memcpy(skin_extra + L, sk, (size_t)m); L += (size_t)m; skin_extra[L++] = ' '; skin_extra[L] = 0;
        }
    }
}

static inline int skin_in_level(const Skin *s, const char *level) {
    if (!s || !level || !*level || strlen(level) > 40) return 0;
    char k[96]; k[0] = ' '; strcpy(k + 1, level); strcat(k, " ");
    if (strstr(s->levels, k)) return 1;
    snprintf(k, sizeof k, " %s:%s ", level, s->name);
    return strstr(skin_extra, k) != NULL;
}

/* The skin to show for a partner who picked `want` in `level`: their pick if the level has it, otherwise the
   first non-experimental skin the level has. NULL if the level has none. */
static inline const Skin *skin_choose(const char *want, const char *level) {
    const Skin *w = skin_find(want);
    if (w && skin_in_level(w, level)) return w;
    for (int i = 0; i < NSKINS; i++) if (!SKINS[i].experimental && skin_in_level(&SKINS[i], level)) return &SKINS[i];
    return NULL;
}

/* "...\DATA\ENV\PRODUCTION\HAB_AIRPORT\WORLD\COMMANDS.PAK" -> "HAB_AIRPORT"; DLC maps -> "DLC_CHALLENGEMAP16".
   Returns 1 if the path is a file inside a level's WORLD folder. */
static inline int skin_level_from_path(const char *path, char *out, int cap) {
    const char *p = path, *hit = NULL;
    for (; *p; p++) {
        const char *k = "PRODUCTION"; int i = 0;
        while (k[i] && p[i] && (p[i] & ~0x20) == k[i]) i++;
        if (!k[i] && (p[i] == '\\' || p[i] == '/') && (p == path || p[-1] == '\\' || p[-1] == '/')) hit = p + i + 1;
    }
    if (!hit) return 0;
    const char *w = hit; int n = 0;
    for (; *w; w++) {
        if ((w[0] == '\\' || w[0] == '/') && !strncmp(w + 1, "WORLD", 5) && (w[6] == '\\' || w[6] == '/')) break;
    }
    if (!*w) return 0;
    for (const char *q = hit; q < w && n < cap - 1; q++) out[n++] = (*q == '\\' || *q == '/') ? '_' : *q;
    out[n] = 0;
    return n > 0;
}

/* The 0x130-byte spawn request. Template fields from AI-MP's CIVILIAN profile; we change the identity hashes,
   the DisplayModel composite, the position and the per-spawn keys. */
#define SKIN_REQ_SIZE 0x130
static inline void skin_put32(uint8_t *b, int off, uint32_t v) { memcpy(b + off, &v, 4); }
static inline void skin_build_request(uint8_t req[SKIN_REQ_SIZE], const Skin *s, const float pos[3],
                                      uint32_t spawn_key, uint32_t entity_key, uint32_t selection) {
    memset(req, 0, SKIN_REQ_SIZE);
    skin_put32(req, 0x00, spawn_key);
    skin_put32(req, 0x10, 0x02000100u);
    skin_put32(req, 0x14, skin_hash(s->anim_set));
    skin_put32(req, 0x18, skin_hash("HUMANOID"));          /* anim tree set */
    skin_put32(req, 0x20, skin_hash(s->skeleton));
    skin_put32(req, 0x24, skin_hash(s->name));             /* display model */
    skin_put32(req, 0x28, entity_key);
    skin_put32(req, 0x2C, 0x5FDA4212u);
    skin_put32(req, 0x30, 3u);
    skin_put32(req, 0x34, 2u);
    skin_put32(req, 0x38, selection);
    skin_put32(req, 0x3C, 0xA21D4E0Bu);
    skin_put32(req, 0x40, 0x5E5739EAu);
    skin_put32(req, 0x44, 0xA6CB83CFu);
    skin_put32(req, 0x4C, 0x33000000u);
    memcpy(req + 0x50, pos, 12);
    memcpy(req + 0xA4, "CIVILIAN", 9);
    skin_put32(req, 0xF0, spawn_key);
    req[0xF4] = 0;                                          /* extended mode 0: parameter is the DisplayModel */
    skin_put32(req, 0xFC, s->display_guid);
}

/* Entity key for a spawn (AI-MP's mix: unique, high bit set). */
static inline uint32_t skin_entity_key(uint32_t spawn_key, uint32_t nonce) {
    uint32_t v = spawn_key ^ (nonce * 0x9E3779B9u) ^ 0xA17C4A52u;
    v ^= v >> 16; v *= 0x7FEB352Du; v ^= v >> 15; v *= 0x846CA68Bu; v ^= v >> 16;
    v |= 0x80000000u;
    return v ? v : 0xA17C4A52u;
}
