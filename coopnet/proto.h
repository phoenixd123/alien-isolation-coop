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
