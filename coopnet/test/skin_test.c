/* Offline checks for the skin system: request layout, level detection, skin choice, skin packet. */
#include <stdio.h>
#include "../skins.h"
#include "../proto.h"

static int fails, passes;
#define CHECK(c, m) do { if (c) passes++; else { fails++; printf("FAIL: %s\n", m); } } while (0)
static uint32_t get32(const uint8_t *b, int o) { uint32_t v; memcpy(&v, b + o, 4); return v; }

int main(void) {
    /* hashes: AI-MP's E_RIPLEY and HUMANOID values */
    CHECK(skin_hash("E_RIPLEY") == 0x21F92DEAu, "FNV-1a of E_RIPLEY");
    CHECK(skin_hash("HUMANOID") == 0xF864FC82u, "FNV-1a of HUMANOID");

    /* request: an E_RIPLEY skin must reproduce AI-MP's CIVILIAN template field for field */
    Skin rip = {"E_RIPLEY", "E_RIPLEY", "E_RIPLEY", 0xD9A16D5Bu, 0, ""};
    float pos[3] = {1.5f, -2.f, 30.25f};
    uint8_t r[SKIN_REQ_SIZE];
    skin_build_request(r, &rip, pos, 77, 0x80001234u, 5);
    CHECK(get32(r, 0x00) == 77 && get32(r, 0xF0) == 77, "spawn key at 0x00 and 0xF0");
    CHECK(get32(r, 0x10) == 0x02000100u, "0x10");
    CHECK(get32(r, 0x14) == 0x21F92DEAu && get32(r, 0x20) == 0x21F92DEAu && get32(r, 0x24) == 0x21F92DEAu, "identity hashes");
    CHECK(get32(r, 0x18) == 0xF864FC82u, "anim tree");
    CHECK(get32(r, 0x2C) == 0x5FDA4212u && get32(r, 0x30) == 3 && get32(r, 0x34) == 2, "0x2C..0x34");
    CHECK(get32(r, 0x3C) == 0xA21D4E0Bu && get32(r, 0x40) == 0x5E5739EAu && get32(r, 0x44) == 0xA6CB83CFu, "0x3C..0x44");
    CHECK(get32(r, 0x4C) == 0x33000000u, "0x4C");
    CHECK(get32(r, 0x28) == 0x80001234u && get32(r, 0x38) == 5, "entity key, selection");
    float p2[3]; memcpy(p2, r + 0x50, 12);
    CHECK(p2[0] == 1.5f && p2[1] == -2.f && p2[2] == 30.25f, "position");
    CHECK(!memcmp(r + 0xA4, "CIVILIAN", 9), "class");
    CHECK(r[0xF4] == 0 && get32(r, 0xFC) == 0xD9A16D5Bu, "DisplayModel parameter");
    /* a story skin with a different skeleton name */
    const Skin *con = skin_find("CONNOR");
    skin_build_request(r, con, pos, 1, 0x80000001u, 1);
    CHECK(get32(r, 0x14) == skin_hash("CONNOR") && get32(r, 0x20) == skin_hash("CONRAD") && get32(r, 0x24) == skin_hash("CONNOR"), "CONNOR uses skeleton CONRAD");
    CHECK(skin_entity_key(5, 1) & 0x80000000u, "entity key high bit");
    CHECK(skin_entity_key(5, 1) != skin_entity_key(5, 2), "entity keys differ per spawn");

    /* level from the file the game opens */
    char lv[48];
    CHECK(skin_level_from_path("C:\\Games\\Alien Isolation\\DATA\\ENV\\PRODUCTION\\HAB_AIRPORT\\WORLD\\COMMANDS.PAK", lv, sizeof lv) && !strcmp(lv, "HAB_AIRPORT"), "story level");
    CHECK(skin_level_from_path("DATA/ENV/PRODUCTION/DLC/CHALLENGEMAP16/WORLD/LEVEL.STR", lv, sizeof lv) && !strcmp(lv, "DLC_CHALLENGEMAP16"), "DLC level");
    CHECK(skin_level_from_path("D:\\x\\data\\env\\production\\SCI_HUB\\WORLD\\x.bin", lv, sizeof lv) && !strcmp(lv, "SCI_HUB"), "lower-case path");
    CHECK(!skin_level_from_path("DATA\\ENV\\PRODUCTION\\HAB_AIRPORT\\RENDERABLE\\LEVEL_TEXTURES.PAK", lv, sizeof lv), "not a WORLD file");
    CHECK(!skin_level_from_path("C:\\Users\\x\\PRODUCTIONS\\a\\WORLD\\b", lv, sizeof lv), "only a PRODUCTION folder counts");

    /* choice */
    CHECK(skin_choose("TAYLOR", "SCI_HUB") == skin_find("TAYLOR"), "pick available");
    CHECK(skin_choose("TAYLOR", "HAB_AIRPORT") == skin_find("AXEL"), "airport falls back to AXEL");
    CHECK(skin_choose("ANDROID", "HAB_AIRPORT") == skin_find("ANDROID"), "experimental when picked");
    CHECK(skin_choose("TAYLOR", "TECH_RND") == NULL, "android-only level: no automatic android");
    CHECK(skin_choose("TAYLOR", "ENG_ALIEN_NEST") == NULL, "no skin in the nest");
    CHECK(skin_choose(NULL, "SOLACE") == skin_find("TAYLOR"), "no pick");
    CHECK(skin_choose("NOBODY", "") == NULL, "no level");
    int with = 0; const char *lvls[] = {"BSP_LV426_PT01", "BSP_TORRENS", "HAB_AIRPORT", "HAB_SHOPPINGCENTRE", "SCI_ANDROIDLAB",
                                        "SCI_HOSPITALUPPER", "SCI_HUB", "TECH_COMMS", "TECH_HUB", "SOLACE"};
    for (int i = 0; i < 10; i++) with += skin_choose("TAYLOR", lvls[i]) != NULL;
    CHECK(with == 10, "levels with a story skin");

    /* skins copied in by skinprep (coop_skins.txt) */
    CHECK(skin_choose("TAYLOR", "TECH_RND") == NULL, "before the list: nothing in TECH_RND");
    { char l1[64], l2[64]; snprintf(l1, sizeof l1, "TECH_RND TAYLOR SAMUELS AXEL%c%c", 13, 10); snprintf(l2, sizeof l2, "DLC_CHALLENGEMAP16 MARLOW%c", 10);
      skin_extra_add_line(l1); skin_extra_add_line(l2); }
    CHECK(skin_choose("TAYLOR", "TECH_RND") == skin_find("TAYLOR"), "copied skin is used");
    CHECK(skin_choose("MARLOW", "TECH_RND") == skin_find("TAYLOR"), "skin not copied there falls back");
    CHECK(skin_choose("MARLOW", "DLC_CHALLENGEMAP16") == skin_find("MARLOW"), "Survivor map");
    CHECK(skin_choose("AXEL", "TECH_RND_HZDLAB") == NULL, "level names don't match by prefix");

    /* packet */
    SkinPkt k; proto_fill_skin(&k, "DR_KUHLMAN");
    CHECK(proto_check_skin(&k, sizeof k) && !strcmp(k.skin, "DR_KUHLMAN"), "skin packet round trip");
    SkinPkt bad = k; bad.skin[0] = 'a'; bad.check = proto_sum(&bad, offsetof(SkinPkt, check));
    CHECK(!proto_check_skin(&bad, sizeof bad), "lower case rejected");
    bad = k; bad.skin[3] = 'Z';
    CHECK(!proto_check_skin(&bad, sizeof bad), "checksum");
    CHECK(!proto_check_skin(&k, 28), "length");

    printf("skin_test: %d/%d checks passed\n", passes, passes + fails);
    return fails != 0;
}
