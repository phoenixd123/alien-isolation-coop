/* Offline tests for the presence protocol: encode/decode, corruption, real UDP loopback, loss + reorder. */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "../proto.h"

static int pass, fail;
#define CHECK(c, m) do { if (c) pass++; else { fail++; printf("FAIL: %s (line %d)\n", m, __LINE__); } } while (0)

int main(void) {
    float pos[3] = {12.5f, -3.25f, 400.f}, q[4] = {0, 0.7071068f, 0, 0.7071068f};
    PresencePkt k; proto_fill(&k, 7, pos, q, 1, "averyveryverylongname");
    CHECK(proto_check(&k, sizeof k), "valid packet accepted");
    CHECK(k.name[15] == 0 && !memcmp(k.name, "averyveryverylon", 15), "name truncated to 15 + NUL");
    CHECK(!memcmp(k.pos, pos, 12) && !memcmp(k.quat, q, 16) && k.seq == 7 && (k.flags & 1), "fields round-trip");
    CHECK(!proto_check(&k, sizeof k - 1), "short length rejected");
    PresencePkt bad = k; ((uint8_t *)&bad)[20] ^= 0x40; CHECK(!proto_check(&bad, sizeof bad), "bit flip rejected");
    bad = k; bad.magic = 1; CHECK(!proto_check(&bad, sizeof bad), "bad magic rejected");
    bad = k; bad.version = 9; bad.check = proto_sum(&bad, offsetof(PresencePkt, check)); CHECK(!proto_check(&bad, sizeof bad), "bad version rejected");
    float nanp[3] = {0, 0, 0}; nanp[1] = strtof("nan", NULL); proto_fill(&bad, 1, nanp, q, 1, "x");
    CHECK(!proto_check(&bad, sizeof bad), "NaN position rejected");

    /* real UDP loopback: 500 packets, receiver keeps newest-seq only, sender shuffles 10% of pairs */
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    SOCKET a = socket(AF_INET, SOCK_DGRAM, 0), b = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sa = {0}; sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); sa.sin_port = htons(45590);
    CHECK(bind(b, (struct sockaddr *)&sa, sizeof sa) == 0, "bind receiver");
    u_long nb = 1; ioctlsocket(b, FIONBIO, &nb);
    srand(1234);
    uint32_t order[500]; for (int i = 0; i < 500; i++) order[i] = (uint32_t)i + 1;
    for (int i = 0; i + 1 < 500; i += 2) if (rand() % 10 == 0) { uint32_t t = order[i]; order[i] = order[i + 1]; order[i + 1] = t; }
    int dropped = 0;
    for (int i = 0; i < 500; i++) {
        if (rand() % 20 == 0 && order[i] != 500) { dropped++; continue; }  /* 5% loss, never the last */
        float p[3] = {(float)order[i], 0, 0};
        proto_fill(&k, order[i], p, q, 1, "p1");
        sendto(a, (const char *)&k, sizeof k, 0, (struct sockaddr *)&sa, sizeof sa);
    }
    /* also inject garbage */
    char junk[64] = "garbage"; sendto(a, junk, 40, 0, (struct sockaddr *)&sa, sizeof sa);
    Sleep(100);
    uint32_t newest = 0; int got = 0, rejected = 0, stale = 0;
    for (;;) {
        PresencePkt r; int n = recv(b, (char *)&r, sizeof r, 0);
        if (n <= 0) break;
        if (!proto_check(&r, n)) { rejected++; continue; }
        got++;
        if (newest && (int32_t)(r.seq - newest) <= 0) { stale++; continue; }
        newest = r.seq;
        if (r.pos[0] != (float)r.seq) { fail++; printf("FAIL: payload mismatch seq %u\n", r.seq); }
    }
    printf("udp: sent %d, received %d, garbage rejected %d, stale ignored %d, newest %u\n", 500 - dropped, got, rejected, stale, newest);
    CHECK(got == 500 - dropped, "all non-dropped packets arrive on loopback");
    CHECK(rejected == 1, "garbage rejected");
    CHECK(newest == 500, "receiver ends on the newest packet");
    closesocket(a); closesocket(b); WSACleanup();
    printf("%d passed, %d failed\n", pass, fail);
    return fail != 0;
}
