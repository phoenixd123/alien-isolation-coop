/* Wire format for coopnet presence packets. Shared by the DLL and the offline tests. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AIC_MAGIC 0x50434941u  /* "AICP" */
#define AIC_VERSION 1

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint8_t  version, kind, flags, pad;  /* kind 1 = presence; flags bit0 = quat valid */
    uint32_t seq;
    float    pos[3];
    float    quat[4];
    char     name[16];
    uint32_t check;                      /* simple checksum over the bytes before it */
} PresencePkt;
#pragma pack(pop)

_Static_assert(sizeof(PresencePkt) == 60, "PresencePkt must be 60 bytes");

static inline uint32_t proto_sum(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p; uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

static inline void proto_fill(PresencePkt *k, uint32_t seq, const float pos[3], const float quat[4], int quat_ok, const char *name) {
    memset(k, 0, sizeof *k);
    k->magic = AIC_MAGIC; k->version = AIC_VERSION; k->kind = 1; k->flags = quat_ok ? 1 : 0; k->seq = seq;
    memcpy(k->pos, pos, 12); memcpy(k->quat, quat, 16);
    size_t L = strlen(name); if (L > 15) L = 15; memcpy(k->name, name, L);
    k->check = proto_sum(k, offsetof(PresencePkt, check));
}

/* kind 2: a world event (run `method` on the entity `eid` of type `type` at the level-file path `path`).
   Sent three times; the receiver drops repeats by seq. */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint8_t  version, kind, npath, pad;
    uint32_t session;                    /* random per game launch: a restarted partner starts a new seq range */
    uint32_t seq;
    uint32_t method, eid, type;
    uint32_t path[6];
    uint32_t check;
} WorldPkt;
#pragma pack(pop)

_Static_assert(sizeof(WorldPkt) == 56, "WorldPkt must be 56 bytes");

static inline void proto_fill_world(WorldPkt *k, uint32_t session, uint32_t seq, uint32_t method, uint32_t eid, uint32_t type, const uint32_t *path, int npath) {
    memset(k, 0, sizeof *k);
    k->magic = AIC_MAGIC; k->version = AIC_VERSION; k->kind = 2; k->session = session; k->seq = seq;
    k->method = method; k->eid = eid; k->type = type;
    if (npath > 6) npath = 6;
    k->npath = (uint8_t)npath; memcpy(k->path, path, (size_t)npath * 4);
    k->check = proto_sum(k, offsetof(WorldPkt, check));
}

static inline int proto_check_world(const WorldPkt *k, int len) {
    if (len != (int)sizeof *k || k->magic != AIC_MAGIC || k->version != AIC_VERSION || k->kind != 2 || k->npath > 6) return 0;
    return k->check == proto_sum(k, offsetof(WorldPkt, check));
}

static inline int proto_check(const PresencePkt *k, int len) {
    if (len != (int)sizeof *k || k->magic != AIC_MAGIC || k->version != AIC_VERSION || k->kind != 1) return 0;
    if (k->check != proto_sum(k, offsetof(PresencePkt, check))) return 0;
    for (int i = 0; i < 3; i++) if (!(k->pos[i] == k->pos[i]) || k->pos[i] > 1e6f || k->pos[i] < -1e6f) return 0;
    return 1;
}

/* kind 3: life state (co-op death and spectating, see life.c). Sent every 500 ms and on every change, 3 times.
   flags bit0 = sender runs on the same PC (couch / split screen), so `pid` names its game process;
   bit1 = sender is the host. */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint8_t  version, kind, state, flags;
    uint32_t epoch;                      /* how many times the sender went back to a checkpoint */
    uint32_t pid;
    uint32_t check;
} LifePkt;
#pragma pack(pop)

_Static_assert(sizeof(LifePkt) == 20, "LifePkt must be 20 bytes");

static inline void proto_fill_life(LifePkt *k, int state, uint32_t epoch, uint32_t pid, int flags) {
    memset(k, 0, sizeof *k);
    k->magic = AIC_MAGIC; k->version = AIC_VERSION; k->kind = 3;
    k->state = (uint8_t)state; k->flags = (uint8_t)flags; k->epoch = epoch; k->pid = pid;
    k->check = proto_sum(k, offsetof(LifePkt, check));
}

static inline int proto_check_life(const LifePkt *k, int len) {
    if (len != (int)sizeof *k || k->magic != AIC_MAGIC || k->version != AIC_VERSION || k->kind != 3 || k->state > 2) return 0;
    return k->check == proto_sum(k, offsetof(LifePkt, check));
}

/* kind 4: where the sender's alien is (character-table record). 30 Hz. flags bit0 = found. */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint8_t  version, kind, flags, pad;
    uint32_t seq;
    float    pos[3];
    uint32_t check;
} AlienPkt;
#pragma pack(pop)

_Static_assert(sizeof(AlienPkt) == 28, "AlienPkt must be 28 bytes");

static inline void proto_fill_alien(AlienPkt *k, uint32_t seq, const float pos[3], int found) {
    memset(k, 0, sizeof *k);
    k->magic = AIC_MAGIC; k->version = AIC_VERSION; k->kind = 4; k->flags = found ? 1 : 0; k->seq = seq;
    if (found) memcpy(k->pos, pos, 12);
    k->check = proto_sum(k, offsetof(AlienPkt, check));
}

static inline int proto_check_alien(const AlienPkt *k, int len) {
    if (len != (int)sizeof *k || k->magic != AIC_MAGIC || k->version != AIC_VERSION || k->kind != 4) return 0;
    if (k->check != proto_sum(k, offsetof(AlienPkt, check))) return 0;
    for (int i = 0; i < 3; i++) if (!(k->pos[i] == k->pos[i]) || k->pos[i] > 1e6f || k->pos[i] < -1e6f) return 0;
    return 1;
}

/* kind 5: the skin the sender wants to be seen as (skins.h). Every 2 s. */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint8_t  version, kind, pad[2];
    char     skin[24];
    uint32_t check;
} SkinPkt;
#pragma pack(pop)
_Static_assert(sizeof(SkinPkt) == 36, "SkinPkt must be 36 bytes");
static inline void proto_fill_skin(SkinPkt *k, const char *skin) {
    memset(k, 0, sizeof *k);
    k->magic = AIC_MAGIC; k->version = AIC_VERSION; k->kind = 5;
    size_t L = strlen(skin); if (L > 23) L = 23; memcpy(k->skin, skin, L);
    k->check = proto_sum(k, offsetof(SkinPkt, check));
}
static inline int proto_check_skin(const SkinPkt *k, int len) {
    if (len != (int)sizeof *k || k->magic != AIC_MAGIC || k->version != AIC_VERSION || k->kind != 5) return 0;
    if (k->check != proto_sum(k, offsetof(SkinPkt, check))) return 0;
    for (int i = 0; i < 24; i++) { char c = k->skin[i]; if (!c) return i > 0; if (!(c == '_' || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return 0; }
    return 0;
}
