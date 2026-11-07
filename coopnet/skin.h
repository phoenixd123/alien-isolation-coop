/* Skin bodies (skin.c). */
#pragma once
int  skin_install(void (*logf)(const char *, ...), int bodies);   /* hooks; 0 if this build doesn't match */
void skin_init_level_watch(void (*logf)(const char *, ...));
void skin_set_partner(const char *name);                    /* partner's pick, from their SkinPkt */
void skin_set_target(int ok, const float feet[3]);          /* where the partner stands (net thread) */
int  skin_active(void);                                     /* a skin body is spawned and following */
int  skin_level_has_skin(void);                             /* this level ships a usable skin model */
const char *skin_status(void);
void skin_share_alien(int on, int tag);                     /* joiner: keep our alien where the host's is */
void skin_set_alien(int ok, const float pos[3]);            /* the host's alien (net thread) */
