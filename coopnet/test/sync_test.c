/* Offline checks for automatic world sync (the generated syncmap.h) and the joiner's alien playback. */
#include <stdio.h>
#include <math.h>
#include "../syncmap.h"
#include "../alien_interp.h"
#include "../cutscene.h"
#include "../proto.h"
#include "../alien_pup.h"

static int fails, passes;
#define CHECK(c, m) do { if (c) passes++; else { fails++; printf("FAIL: %s\n", m); } } while (0)
static int near3(const float *a, float x, float y, float z) { return fabsf(a[0] - x) < 1e-3f && fabsf(a[1] - y) < 1e-3f && fabsf(a[2] - z) < 1e-3f; }

int main(void) {
    /* table: sorted, unique, every entry found by the lookup, both kinds present */
    int sorted = 1, found = 1, k1 = 0, k2 = 0;
    for (int i = 0; i < SYNCMAP_N; i++) {
        if (i && !(SYNCMAP_KEY[i - 1][0] < SYNCMAP_KEY[i][0] || (SYNCMAP_KEY[i - 1][0] == SYNCMAP_KEY[i][0] && SYNCMAP_KEY[i - 1][1] < SYNCMAP_KEY[i][1]))) sorted = 0;
        if (syncmap_kind(SYNCMAP_KEY[i][0], SYNCMAP_KEY[i][1]) != SYNCMAP_KIND[i]) found = 0;
        k1 += SYNCMAP_KIND[i] == 1; k2 += SYNCMAP_KIND[i] == 2;
    }
    CHECK(SYNCMAP_N > 1000, "table has the level scripts' entities");
    CHECK(sorted, "sorted and unique");
    CHECK(found, "every entry found");
    CHECK(k1 > 100 && k2 > 1000, "both kinds");
    /* known entities */
    CHECK(syncmap_kind(0x143a5b75u, 0x26dbce33u) == 1, "Door_Mechanism IsCuttingCompleted (persistent) is STATE");
    CHECK(syncmap_kind(0x143a5b75u, 0x09de34aeu) == 0, "Door_Mechanism IsLocked (not persistent) isn't automatic");
    CHECK(syncmap_kind(0x37849153u, 0x9ba70bc8u) == 0, "KeyPadHacking LogicOnce_3 (reset on each use) left out");
    CHECK(syncmap_kind(0xef3722dcu, 0x92d9160au) == 0, "SwitchLevel itself isn't automatic (it has its own rule)");
    /* misses */
    int miss = 1;
    for (uint32_t x = 1; x < 2000; x++) if (syncmap_kind(x * 2654435761u, x * 40503u) && syncmap_kind(x * 2654435761u, x * 40503u) != 0) { miss = 0; break; }
    CHECK(miss, "random IDs are not shared");
    CHECK(syncmap_kind(0, 0) == 0 && syncmap_kind(0xFFFFFFFFu, 0xFFFFFFFFu) == 0, "edges");

    /* alien playback */
    AlienTrack a; memset(&a, 0, sizeof a); float o[3];
    CHECK(!alien_track_at(&a, 100, o), "no samples, no position");
    float p0[3] = {0, 0, 0}, p1[3] = {1, 0, 2}, p2[3] = {2, 0, 4};
    alien_track_push(&a, 1000, p0); alien_track_push(&a, 1033, p1); alien_track_push(&a, 1066, p2);
    CHECK(alien_track_at(&a, 1000, o) && near3(o, 0, 0, 0), "on a sample");
    CHECK(alien_track_at(&a, 1016, o) && fabsf(o[0] - 16.f / 33.f) < 1e-3f && fabsf(o[2] - 32.f / 33.f) < 1e-3f, "between samples");
    CHECK(alien_track_at(&a, 1050, o) && o[0] > 1.f && o[0] < 2.f, "second span");
    CHECK(alien_track_at(&a, 5000, o) && near3(o, 2, 0, 4), "newer than all: newest, no guessing ahead");
    CHECK(alien_track_at(&a, 10, o) && near3(o, 0, 0, 0), "older than all: oldest");
    float back[3] = {9, 9, 9};
    alien_track_push(&a, 1040, back);
    CHECK(alien_track_at(&a, 5000, o) && near3(o, 2, 0, 4), "late sample ignored");
    /* a vent jump: no sliding across the map */
    float far[3] = {40, 0, 4};
    alien_track_push(&a, 1100, far);
    CHECK(alien_track_at(&a, 1090, o) && near3(o, 2, 0, 4), "jump: stays put until the new sample's time");
    CHECK(alien_track_at(&a, 1100, o) && near3(o, 40, 0, 4), "jump: then snaps");
    /* buffer wraps */
    for (int i = 0; i < 20; i++) { float q[3] = {(float)i, 1, 0}; alien_track_push(&a, 2000u + (uint32_t)i * 33u, q); }
    CHECK(a.n == ALIEN_SAMPLES, "buffer wraps");
    CHECK(alien_track_at(&a, 2000u + 19u * 33u - 16u, o) && o[0] > 18.f && o[0] < 19.f, "after wrap");
    /* clock wrap */
    AlienTrack w; memset(&w, 0, sizeof w);
    alien_track_push(&w, 0xFFFFFFF0u, p0); alien_track_push(&w, 0x00000010u, p2);
    CHECK(alien_track_at(&w, 0, o) && fabsf(o[0] - 1.f) < 1e-3f, "across the 49-day tick wrap");

    /* cutscene tables */
    CHECK(CUT_COMP_N == 14 && STORY_CAM_N == 11, "14 cutscene composites, 11 story cameras");
    CHECK(cut_is_comp(0x5c929e95u) && !cut_is_comp(0x143a5b75u), "cutscene_22 is a cutscene, Door_Mechanism isn't");
    CHECK(cut_is_story_cam(STORY_CAM[0][0], STORY_CAM[0][1]) && !cut_is_story_cam(STORY_CAM[0][0], 1), "story camera lookup");

    /* landing spot: a point walked through about a metre back */
    CutTrail tr; memset(&tr, 0, sizeof tr); float s3[3];
    CHECK(!cut_safe_spot(&tr, 0, s3), "no trail, no spot");
    for (int i = 0; i <= 30; i++) { float q[3] = {i * 0.1f, 2.f, 0.f}; cut_trail_push(&tr, 1000u + (uint32_t)i * 33u, q); }
    uint32_t tnow = 1000u + 30u * 33u;
    CHECK(cut_safe_spot(&tr, tnow, s3) && s3[0] <= 2.0f && s3[0] >= 1.5f && s3[1] == 2.f, "spot 1 m behind on the path");
    CHECK(tr.n < 31, "points only every 0.3 m");
    for (int i = 0; i < 100; i++) { float q[3] = {3.0f, 2.f, 0.f}; cut_trail_push(&tr, tnow + 33u * (uint32_t)i, q); }
    CHECK(cut_safe_spot(&tr, tnow + 3300u, s3) && s3[0] <= 2.0f && s3[0] >= 1.5f, "standing still for 3 s keeps the spot behind (no landing inside them)");
    CutTrail st; memset(&st, 0, sizeof st);
    for (int i = 0; i < 10; i++) { float q[3] = {5.f, 0.f, 5.f}; cut_trail_push(&st, 100u + (uint32_t)i * 33u, q); }
    CHECK(cut_safe_spot(&st, 500, s3) && near3(s3, 5, 0, 5), "standing still: their own spot");
    CutTrail old; memset(&old, 0, sizeof old);
    { float q0[3] = {0, 0, 0}, q1[3] = {3, 0, 0}; cut_trail_push(&old, 100, q0); cut_trail_push(&old, 90000, q1); }
    CHECK(cut_safe_spot(&old, 90000, s3) && near3(s3, 3, 0, 0), "too old a point isn't used");
    for (int i = 0; i < 200; i++) { float q[3] = {(float)i, 0, 0}; cut_trail_push(&tr, 5000u + (uint32_t)i * 33u, q); }
    CHECK(tr.n == CUT_TRAIL && cut_safe_spot(&tr, 5000u + 199u * 33u, s3) && near3(s3, 198, 0, 0), "trail wraps");

    /* who moves */
    CHECK(cut_should_follow(1, 1, 20.f, -1, 0) && cut_should_follow(1, 1, 20.f, -1, 1), "far away: move");
    CHECK(!cut_should_follow(1, 1, 2.f, -1, 0), "already close: stay");
    CHECK(!cut_should_follow(0, 1, 20.f, -1, 0), "dead (spectating): stay");
    CHECK(!cut_should_follow(1, 0, 20.f, -1, 0), "not in a level: stay");
    CHECK(cut_should_follow(1, 1, 20.f, 500, 0) && !cut_should_follow(1, 1, 20.f, 500, 1), "both started one: the joiner goes to the host");
    CHECK(cut_should_follow(1, 1, 20.f, 5000, 1), "mine was long ago: move");
    CHECK(!cut_should_follow(1, 1, NAN, -1, 0), "bad distance: stay");

    /* packet */
    CutPkt ck; float cp[3] = {1.5f, -2.f, 300.f}; proto_fill_cut(&ck, 77, cp, CUT_SCENE);
    CHECK(proto_check_cut(&ck, sizeof ck) && ck.id == 77 && ck.pos[2] == 300.f, "cut packet round trip");
    CutPkt cb = ck; cb.pos[0] = 9999.f; cb.check = proto_sum(&cb, offsetof(CutPkt, check));
    CHECK(!proto_check_cut(&cb, sizeof cb), "position off the map rejected");
    cb = ck; cb.id = 0; cb.check = proto_sum(&cb, offsetof(CutPkt, check));
    CHECK(!proto_check_cut(&cb, sizeof cb), "id 0 rejected");
    cb = ck; cb.pos[1] = 1.f;
    CHECK(!proto_check_cut(&cb, sizeof cb), "checksum");
    AlienPkt ap; float apos[3] = {1, 2, 3}; proto_fill_alien(&ap, 5, apos, 1);
    CHECK(!proto_check_cut((const CutPkt *)&ap, sizeof ap) && proto_check_alien(&ap, sizeof ap), "same size as the alien packet, told apart by kind");

    /* regroup (F7) and the teleport fade */
    CHECK(regroup_ok(1, 1, 10.f) && !regroup_ok(1, 1, 1.f) && !regroup_ok(0, 1, 10.f) && !regroup_ok(1, 0, 10.f) && !regroup_ok(1, 1, NAN), "who may regroup");
    CHECK(tp_fade(-5) == 0.f && tp_fade(0) == 0.f && fabsf(tp_fade(125) - 0.5f) < 1e-3f && tp_fade(TP_FADE_OUT_MS) == 1.f, "fade out");
    CHECK(tp_fade(TP_FADE_OUT_MS + TP_FADE_IN_MS / 2) > 0.45f && tp_fade(TP_FADE_OUT_MS + TP_FADE_IN_MS / 2) < 0.55f && tp_fade(5000) == 0.f, "fade back in");
    CutPkt ra; float rp[3] = {1, 2, 3}; proto_fill_cut(&ra, 9, rp, CUT_REGROUP_ASK);
    CHECK(proto_check_cut(&ra, sizeof ra) && ra.flags == CUT_REGROUP_ASK, "regroup ask packet");
    ra.flags = 3; ra.check = proto_sum(&ra, offsetof(CutPkt, check));
    CHECK(!proto_check_cut(&ra, sizeof ra), "unknown teleport kind rejected");

    /* build check */
    BuildPkt bk; proto_fill_build(&bk, 0x1111, 0x2222);
    CHECK(proto_check_build(&bk, sizeof bk), "build packet round trip");
    CHECK(proto_build_diff(0x1111, 0x2222, &bk) == 0, "same builds");
    CHECK(proto_build_diff(0x9999, 0x2222, &bk) == 1 && proto_build_diff(0x1111, 0x9999, &bk) == 2 && proto_build_diff(9, 9, &bk) == 3, "what differs");
    CHECK(proto_build_diff(0, 0x2222, &bk) == 0, "unknown build: no verdict");
    BuildPkt bb = bk; bb.mod = 5;
    CHECK(!proto_check_build(&bb, sizeof bb), "build checksum");

    /* the joiner's alien: walk to the host's, stop and turn when there, put there when far or in the vents */
    {
        PupState ps = {0}; float me[3] = {0, 0, 0}; PupStep st;
        float far[3] = {5, 0, 0}, mid[3] = {1, 0, 0}, near_[3] = {0.1f, 0, 0}, up[3] = {0.5f, 2.5f, 0}, edge[3] = {0.3f, 0, 0};
        st = pup_decide(&ps, me, 0, far, 0, 1, 0);   CHECK(st.act == PUP_TELEPORT, "far off: put there");
        st = pup_decide(&ps, me, 0, up, 0, 1, 0);    CHECK(st.act == PUP_TELEPORT, "a floor up or down: put there");
        st = pup_decide(&ps, me, 0, mid, 0, 1, 0);   CHECK(st.act == PUP_STEER && fabsf(st.heading - 1.5707964f) < 1e-3f && st.speed == 1, "1 m off along +x: walk along +x at its speed");
        st = pup_decide(&ps, me, 0, edge, 0, 1, 0);  CHECK(st.act == PUP_STEER, "still walking until nearer than PUP_ARRIVE_M");
        st = pup_decide(&ps, me, 0, near_, 0, 1, 0); CHECK(st.act == PUP_STOP && !st.set_yaw, "arrived, same facing: stop");
        st = pup_decide(&ps, me, 0, edge, 0, 1, 0);  CHECK(st.act == PUP_STOP, "stopped: doesn't start again for 0.3 m (no jitter)");
        st = pup_decide(&ps, me, 0, near_, 2.0f, 1, 0); CHECK(st.set_yaw && st.yaw > 0.4f && st.yaw < 0.6f, "standing: turns a quarter of the way to its facing");
        st = pup_decide(&ps, me, 3.0f, near_, -3.0f, 1, 0); CHECK(st.set_yaw && st.yaw > 3.0f, "turns the short way round");
        float behind[3] = {0, 0, 2.5f};
        st = pup_decide(&ps, me, 0, behind, 0, 3, 0); CHECK(st.act == PUP_STEER && st.speed == 3 && fabsf(st.heading) < 1e-3f, "fell behind at top speed: stays at 3");
        st = pup_decide(&ps, me, 0, behind, 0, 1, 0); CHECK(st.speed == 2, "fell behind: one speed up");
        st = pup_decide(&ps, me, 0, mid, 0, 1, 1);    CHECK(st.act == PUP_TELEPORT, "in the vents: follows the point");
        st = pup_decide(&ps, me, 0, near_, 0, 1, 1);  CHECK(st.act == PUP_STOP, "in the vents and there: nothing");
    }
    {
        AlienExPkt x; memset(&x, 0, sizeof x);
        x.flags = ALX_FOUND | ALX_LEARN; x.speed = 2; x.pos[0] = 3; x.yaw = 1.f; x.setup = 4; x.learn_have = 0x18E; x.kills = 1;
        proto_fill_alienx(&x);
        CHECK(proto_check_alienx(&x, sizeof x), "alien packet round trip");
        CHECK(!proto_check_alienx(&x, sizeof x - 1), "alien packet: wrong length");
        AlienExPkt y = x; y.kills = 2; CHECK(!proto_check_alienx(&y, sizeof y), "alien packet checksum");
        y = x; y.speed = 7; y.check = proto_sum(&y, offsetof(AlienExPkt, check)); CHECK(!proto_check_alienx(&y, sizeof y), "alien packet: bad speed");
        y = x; y.yaw = NAN; y.check = proto_sum(&y, offsetof(AlienExPkt, check)); CHECK(!proto_check_alienx(&y, sizeof y), "alien packet: bad facing");
    }

    /* carry-along: a scripted jump is told to the partner, who comes along only if they were right there */
    {
        float a[3] = {0, 0, 0}, b[3] = {0, 4, 0}, walk[3] = {0.1f, 0, 0}, out[3];
        CHECK(carry_is_jump(a, b, 33, -1, 10000, 1), "4 m in one sample, well into the level: a scripted move");
        CHECK(!carry_is_jump(a, walk, 33, -1, 10000, 1), "walking isn't");
        CHECK(!carry_is_jump(a, b, 33, 1000, 10000, 1), "not right after one of our own teleports");
        CHECK(!carry_is_jump(a, b, 33, -1, 1000, 1), "not right after entering the level");
        CHECK(!carry_is_jump(a, b, 33, -1, 10000, 0), "not while dead");
        CHECK(!carry_is_jump(a, b, 500, -1, 10000, 1), "not across a gap in the samples");
        float far_[3] = {0, 0, 900};
        CHECK(!carry_is_jump(a, far_, 33, -1, 10000, 1), "a reload across the map isn't a lift ride");
        float me[3] = {1, 0, 1}, from[3] = {0, 0, 0}, to[3] = {0, 8, 20};
        CHECK(carry_follow(1, 1, me, from, to, out) && near3(out, 1, 8, 21), "next to the partner: moved by the same amount");
        float me_far[3] = {10, 0, 0};
        CHECK(!carry_follow(1, 1, me_far, from, to, out), "elsewhere: stays");
        CHECK(!carry_follow(0, 1, me, from, to, out), "dead: stays");
        /* settled moves only (found in game: the alien's kill shook the player back and forth and flooded carries) */
        CarryWatch w; memset(&w, 0, sizeof w); float f2[3], t2[3];
        float p0[3] = {0, 0, 0}, p1[3] = {0, 6, 0}, p1b[3] = {0.1f, 6, 0};
        CHECK(!carry_watch(&w, p0, p1, 1000, 33, -1, 20000, 1, f2, t2), "a lift jump: not yet (settling)");
        CHECK(!carry_watch(&w, p1, p1b, 1300, 33, -1, 20000, 1, f2, t2), "still settling");
        CHECK(carry_watch(&w, p1b, p1b, 1600, 33, -1, 20000, 1, f2, t2) && near3(t2, 0, 6, 0) && near3(f2, 0, 0, 0), "settled: told once");
        CHECK(!carry_watch(&w, p1b, p1b, 1700, 33, -1, 20000, 1, f2, t2), "only once");
        CarryWatch k2; memset(&k2, 0, sizeof k2);
        float q0[3] = {0, 0, 0}, q1[3] = {0, 0, 4};
        int sent = 0; uint32_t t = 30000;
        for (int i = 0; i < 40; i++, t += 50) sent += carry_watch(&k2, (i & 1) ? q1 : q0, (i & 1) ? q0 : q1, t, 50, -1, 30000, 1, f2, t2);
        for (int i = 0; i < 20; i++, t += 50) sent += carry_watch(&k2, q0, q0, t, 50, -1, 30000, 1, f2, t2);
        CHECK(sent == 0, "jumping back and forth (a kill animation): never sent");
        CarryWatch k3; memset(&k3, 0, sizeof k3);
        carry_watch(&k3, p0, p1, 1000, 33, -1, 20000, 1, f2, t2);
        CHECK(!carry_watch(&k3, p1, p1, 1600, 33, -1, 20000, 0, f2, t2), "died while settling: not sent");
        CarryWatch k4; memset(&k4, 0, sizeof k4); int n4 = 0;
        n4 += carry_watch(&k4, p0, p1, 1000, 33, -1, 20000, 1, f2, t2); n4 += carry_watch(&k4, p1, p1, 1600, 33, -1, 20000, 1, f2, t2);
        n4 += carry_watch(&k4, p1, p0, 4000, 33, -1, 20000, 1, f2, t2); n4 += carry_watch(&k4, p0, p0, 4600, 33, -1, 20000, 1, f2, t2);
        CHECK(n4 == 1, "a second ride within 8 s: not sent");
        CarryPkt ck; proto_fill_carry(&ck, 7, from, to);
        CHECK(proto_check_carry(&ck, sizeof ck), "carry packet round trip");
        ck.to[1] = 9; CHECK(!proto_check_carry(&ck, sizeof ck), "carry packet checksum");
    }

    printf("sync_test: %d/%d checks passed\n", passes, passes + fails);
    return fails != 0;
}
