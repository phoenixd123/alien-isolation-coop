/* memfind <pid> <hex-bytes...>  : scan all committed readable memory of a process for a byte pattern (read-only).
 * memfind <pid> -f x y z        : pattern = three little-endian floats.
 * Prints address, region base, protection and type of each hit (max 50). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage\n"); return 1; }
    DWORD pid = strtoul(argv[1], 0, 10);
    unsigned char pat[64]; int n = 0;
    if (!strcmp(argv[2], "-f")) { for (int i = 3; i < argc && n < 60; i++) { float f = (float)atof(argv[i]); memcpy(pat + n, &f, 4); n += 4; } }
    else for (int i = 2; i < argc && n < 64; i++) pat[n++] = (unsigned char)strtoul(argv[i], 0, 16);
    HANDLE h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) { fprintf(stderr, "open failed\n"); return 2; }
    MEMORY_BASIC_INFORMATION mi; unsigned char *a = 0; int hits = 0; size_t scanned = 0;
    static unsigned char buf[1 << 20];
    while (VirtualQueryEx(h, a, &mi, sizeof mi) && hits < 50) {
        unsigned char *base = mi.BaseAddress; SIZE_T sz = mi.RegionSize;
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            for (SIZE_T off = 0; off < sz; off += sizeof buf - 64) {
                SIZE_T want = sz - off < sizeof buf ? sz - off : sizeof buf, got = 0;
                if (!ReadProcessMemory(h, base + off, buf, want, &got) || got < (SIZE_T)n) continue;
                scanned += got;
                for (SIZE_T i = 0; i + n <= got; i++)
                    if (buf[i] == pat[0] && !memcmp(buf + i, pat, n)) {
                        printf("hit %p region %p prot %lx type %lx\n", base + off + i, base, mi.Protect, mi.Type);
                        if (++hits >= 50) break;
                    }
                if (want < sizeof buf) break;
            }
        }
        a = base + sz;
        if ((uintptr_t)a >= 0x7FFF0000) break;
    }
    printf("scanned %zu MB, %d hits\n", scanned >> 20, hits);
    CloseHandle(h);
    return 0;
}
