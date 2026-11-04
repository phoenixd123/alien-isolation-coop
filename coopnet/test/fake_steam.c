/* Fake "steam_api_real.dll" for the steamnet test. Every method is declared with raw dwords in the
 * MSVC x86 layout (CSteamID = two dwords, hidden result pointer for GetSteamID), so a caller that
 * pushes the wrong layout shows up as wrong argument values here. */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#define THISCALL __attribute__((thiscall))
#define EXPORT __declspec(dllexport)
typedef struct { void **vtbl; } Obj;

EXPORT int f_ready;
EXPORT uint64_t f_self = 76561198000000001ull;
EXPORT uint64_t f_sent_to, f_accepted;
EXPORT uint32_t f_sent_n, f_sent_type, f_sent_chan, f_sends, f_relay = 99, f_read_chan;
EXPORT uint8_t f_sent[256];
EXPORT void *f_cb[8]; EXPORT int f_cb_id[8]; EXPORT int f_ncb;

/* inbound queue */
static struct { uint64_t from; uint32_t n; uint8_t d[256]; } q[32]; static int qh, qt;
EXPORT void __cdecl fake_inject(uint64_t from, const void *d, uint32_t n) {
    q[qt % 32].from = from; q[qt % 32].n = n; memcpy(q[qt % 32].d, d, n); qt++;
}

static int THISCALL n_send(Obj *o, uint32_t lo, uint32_t hi, const void *d, uint32_t n, uint32_t type, uint32_t chan) {
    (void)o; f_sent_to = ((uint64_t)hi << 32) | lo; f_sent_n = n; f_sent_type = type; f_sent_chan = chan; f_sends++;
    if (n <= sizeof f_sent) memcpy(f_sent, d, n);
    return 1;
}
static int THISCALL n_avail(Obj *o, uint32_t *sz, uint32_t chan) {
    (void)o; f_read_chan = chan; if (qh == qt) return 0; *sz = q[qh % 32].n; return 1;
}
static int THISCALL n_read(Obj *o, void *dst, uint32_t cap, uint32_t *got, uint32_t *from, uint32_t chan) {
    (void)o; (void)chan; if (qh == qt) return 0;
    uint32_t n = q[qh % 32].n < cap ? q[qh % 32].n : cap;
    memcpy(dst, q[qh % 32].d, n); *got = n; memcpy(from, &q[qh % 32].from, 8); qh++; return 1;
}
static int THISCALL n_accept(Obj *o, uint32_t lo, uint32_t hi) { (void)o; f_accepted = ((uint64_t)hi << 32) | lo; return 1; }
static int THISCALL n_close(Obj *o, uint32_t lo, uint32_t hi) { (void)o; (void)lo; (void)hi; return 1; }
static int THISCALL n_closech(Obj *o, uint32_t lo, uint32_t hi, uint32_t c) { (void)o; (void)lo; (void)hi; (void)c; return 1; }
static int THISCALL n_state(Obj *o, uint32_t lo, uint32_t hi, void *s) { (void)o; (void)lo; (void)hi; (void)s; return 0; }
static int THISCALL n_relay(Obj *o, uint32_t b) { (void)o; f_relay = b & 0xFF; return 1; }
static void *net_vt[8] = { n_send, n_avail, n_read, n_accept, n_close, n_closech, n_state, n_relay };
static Obj net = { net_vt };

static int THISCALL u_hsu(Obj *o) { (void)o; return 1; }
static int THISCALL u_logged(Obj *o) { (void)o; return 1; }
static uint64_t *THISCALL u_id(Obj *o, uint64_t *ret) { (void)o; *ret = f_self; return ret; }
static void *user_vt[3] = { u_hsu, u_logged, u_id };
static Obj user = { user_vt };
static const char *THISCALL fr_name(Obj *o) { (void)o; return "Tester"; }
static void *fr_vt[1] = { fr_name };
static Obj friends = { fr_vt };

EXPORT void *__cdecl SteamUser(void) { return f_ready ? &user : NULL; }
EXPORT void *__cdecl SteamNetworking(void) { return f_ready ? &net : NULL; }
EXPORT void *__cdecl SteamFriends(void) { return f_ready ? &friends : NULL; }
EXPORT void __cdecl SteamAPI_RegisterCallback(void *cb, int id) {
    if (f_ncb < 8) { f_cb[f_ncb] = cb; f_cb_id[f_ncb++] = id; }
    ((uint8_t *)cb)[4] |= 1; *(int *)((uint8_t *)cb + 8) = id;   /* what Steam does to CCallbackBase */
}

/* Call slot of cb as MSVC thiscall with n dwords; returns bytes the callee popped. */
static int vcall(void *obj, int slot, int n, const uint32_t *args, uint32_t *ret) {
    void *fn = (*(void ***)obj)[slot];
    uint32_t r, saved, after;
    __asm__ volatile(
        "movl %%esp, %[saved]\n\t"
        "movl %[n], %%ecx\n\t"
        "1: testl %%ecx, %%ecx\n\t"
        "jz 2f\n\t"
        "pushl -4(%[args],%%ecx,4)\n\t"
        "decl %%ecx\n\t"
        "jmp 1b\n\t"
        "2: movl %[obj], %%ecx\n\t"
        "call *%[fn]\n\t"
        "movl %%esp, %[after]\n\t"
        "movl %[saved], %%esp\n\t"
        : [saved] "=&m"(saved), [after] "=&m"(after), "=a"(r)
        : [n] "m"(n), [args] "r"(args), [obj] "m"(obj), [fn] "m"(fn)
        : "ecx", "edx", "memory", "cc");
    if (ret) *ret = r;
    return (int)(after - (saved - 4u * (uint32_t)n));
}
/* Deliver a callback the way the dispatcher does: Run(void *param) is vtable slot 1. */
EXPORT int __cdecl fake_fire(int id, const void *param) {
    for (int i = 0; i < f_ncb; i++) if (f_cb_id[i] == id) { uint32_t a[1] = { (uint32_t)param }; return vcall(f_cb[i], 1, 1, a, NULL); }
    return -1;
}
/* Slot 0 (call-result Run(void*, bool, SteamAPICall_t)) pops 16; slot 2 returns the param size. */
EXPORT int __cdecl fake_fire_cr(int id, const void *param) {
    for (int i = 0; i < f_ncb; i++) if (f_cb_id[i] == id) { uint32_t a[4] = { (uint32_t)param, 0, 5, 0 }; return vcall(f_cb[i], 0, 4, a, NULL); }
    return -1;
}
EXPORT int __cdecl fake_size(int id, int *popped) {
    for (int i = 0; i < f_ncb; i++) if (f_cb_id[i] == id) { uint32_t r; *popped = vcall(f_cb[i], 2, 0, NULL, &r); return (int)r; }
    return -1;
}
