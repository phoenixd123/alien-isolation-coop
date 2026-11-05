#pragma once
enum { SFX_PING_FAR = 0, SFX_PING_MID, SFX_PING_NEAR, SFX_HEART, SFX_STING, SFX_COUNT };
void sfx_init(void);
void sfx_play(int which);
