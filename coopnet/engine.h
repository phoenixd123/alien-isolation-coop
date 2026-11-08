/* engine: moving the local player at the end of the frame (engine.c, method from OpenCAGE). */
#pragma once
#include <stdint.h>

enum { ENG_PENDING = -1, ENG_DONE = 0, ENG_NO_PLAYER, ENG_DEAD, ENG_NO_BODY };

int engine_install(void (*logf)(const char *, ...));   /* 0 if this build doesn't match: no teleports */
uint32_t engine_teleport(const float to[3]);           /* any thread; returns a ticket (0: not installed) */
int engine_result(uint32_t ticket);                    /* ENG_PENDING until that teleport ran */
