/* Offline test for the Steam P2P transport against fake_steam.dll (loaded as steam_api_real.dll). */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../steamnet.h"
#include "../proto.h"

static int pass, fail;
#define CHECK(c, ...) do { if (c) pass++; else { fail++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void logf_(const char *fmt, ...) { va_list a; va_start(a, fmt); printf("  log: "); vprintf(fmt, a); printf("\n"); va_end(a); }

#define A 76561198000000002ull
#define B 76561198000000003ull

int main(void) {
    HMODULE f = LoadLibraryA("steam_api_real.dll");
    CHECK(f != NULL, "load fake err %lu", GetLastError()); if (!f) return 1;
#define V(t, n) t n = (t)GetProcAddress(f, #n)
    V(int *, f_ready); V(uint64_t *, f_sent_to); V(uint64_t *, f_accepted); V(uint32_t *, f_sent_n);
    V(uint32_t *, f_sent_type); V(uint32_t *, f_sent_chan); V(uint32_t *, f_sends); V(uint32_t *, f_relay);
    V(uint32_t *, f_read_chan); V(int *, f_ncb); V(int *, f_cb_id); V(uint8_t *, f_sent);
    typedef void (__cdecl *Inject)(uint64_t, const void *, uint32_t);
    typedef int (__cdecl *Fire)(int, const void *);
    typedef int (__cdecl *Size)(int, int *);
    V(Inject, fake_inject); V(Fire, fake_fire); V(Fire, fake_fire_cr); V(Size, fake_size);

    CHECK(!steamnet_try_init(0, logf_), "not ready before SteamAPI_Init");
    *f_ready = 1;
    CHECK(steamnet_try_init(0, logf_), "ready after init");
    CHECK(steamnet_self() == 76561198000000001ull, "GetSteamID through hidden result pointer: %llu", (unsigned long long)steamnet_self());
    CHECK(!strcmp(steamnet_persona(), "Tester"), "persona name");
    CHECK(*f_relay == 1, "relay allowed");
    CHECK(*f_ncb == 2 && f_cb_id[0] == 1202 && f_cb_id[1] == 1203, "callbacks 1202/1203 registered");
    CHECK(steamnet_peer() == 0, "host starts without a partner");

    float p[3] = {1, 2, 3}, q[4] = {0, 0, 0, 1};
    PresencePkt k; proto_fill(&k, 1, p, q, 1, "host");
    CHECK(!steamnet_send(&k, sizeof k) && *f_sends == 0, "host doesn't send before anyone joins");

    uint64_t ida = A, idb = B;
    CHECK(fake_fire(1202, &ida) == 4, "Run(void*) is slot 1 and pops 4");
    steamnet_poll();
    CHECK(steamnet_peer() == A && *f_accepted == A, "host accepts the first request (accepted %llu)", (unsigned long long)*f_accepted);
    CHECK(steamnet_send(&k, sizeof k), "send");
    CHECK(*f_sent_to == A && *f_sent_n == 60 && *f_sent_type == 0 && *f_sent_chan == 7 && !memcmp(f_sent, &k, 60),
          "SendP2PPacket args to=%llu n=%u type=%u chan=%u", (unsigned long long)*f_sent_to, *f_sent_n, *f_sent_type, *f_sent_chan);

    PresencePkt in, out; proto_fill(&in, 9, p, q, 1, "joiner");
    fake_inject(B, &in, sizeof in);        /* stranger: dropped */
    fake_inject(A, &in, sizeof in);
    int n = steamnet_recv(&out, sizeof out);
    CHECK(n == 60 && !memcmp(&in, &out, 60) && proto_check(&out, n), "receive partner's packet, drop stranger's (n=%d)", n);
    CHECK(*f_read_chan == 7, "reads our channel");
    CHECK(steamnet_recv(&out, sizeof out) == 0, "queue empty");

    *f_accepted = 0;
    CHECK(fake_fire(1202, &idb) == 4, "second request delivered");
    steamnet_poll();
    CHECK(steamnet_peer() == A && *f_accepted == 0, "host keeps its partner, ignores a second player");

    uint8_t failp[16] = {0}; memcpy(failp, &ida, 8); failp[8] = 4;
    CHECK(fake_fire(1203, failp) == 4, "connect-fail callback pops 4");
    steamnet_poll();
    CHECK(fake_fire_cr(1202, &ida) == 16, "call-result Run is slot 0 and pops 16");
    int pop = -1;
    CHECK(fake_size(1202, &pop) == 8 && pop == 0, "GetCallbackSizeBytes(1202) = 8");
    CHECK(fake_size(1203, &pop) == 16 && pop == 0, "GetCallbackSizeBytes(1203) = 16");

    steamnet_set_peer(B);
    CHECK(steamnet_peer() == B && *f_accepted == B, "set_peer (join) accepts the host");
    CHECK(steamnet_send(&k, sizeof k) && *f_sent_to == B, "join sends to the host");

    printf("%d passed, %d failed\n", pass, fail);
    return fail != 0;
}
