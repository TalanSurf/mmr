#include "recorder.h"
#include "menu.h"
#include "hooks.h"

// menu::g_teleport_to_start is declared in menu.h

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <vector>
#include <windows.h>

namespace recorder {

    // Source runs queued rendering: the ImGui menu (Present, render thread)
    // and the recorder (CreateMove, main thread) run CONCURRENTLY. Every
    // entry point that mutates the tape vectors takes this lock — clicking
    // record/play/load in the menu while a tick was mid-push_back was
    // corrupting the heap (ntdll AV, detonating seconds later). Recursive
    // because commit/rollback fire execute_client_cmd, which re-enters
    // record_event synchronously through the ClientCmd hook.
    static std::recursive_mutex    g_mtx;

    static Mode                    g_mode        = Mode::Idle;
    static std::vector<Frame>      g_tape;
    static std::vector<TapeEvent>  g_events;      // metadata: Saveloc=commit, Teleport=rollback
    static std::vector<hooks::Vec3> g_positions;  // parallel to g_tape, drives auto delay-comp
    static std::vector<hooks::Vec3> g_vels;       // real m_vecVelocity (ups) per tick — v4 tapes
    static std::vector<int>         g_flags;      // m_fFlags per tick — v4 tapes
    static size_t                  g_head        = 0;

    // Source m_fFlags bits that change how the NEXT tick simulates. Two
    // frames agreeing in pos+vel but differing in any of these are
    // different physics states — never stitch across them.
    constexpr int FL_ONGROUND     = 1 << 0;
    constexpr int FL_DUCKING      = 1 << 1;
    constexpr int FL_INWATER      = 1 << 10;
    constexpr int PHYS_FLAGS_MASK = FL_ONGROUND | FL_DUCKING | FL_INWATER;
    constexpr float TICKS_PER_SEC = 66.666f; // Source SDK 2013 MP default tickrate — Momentum's default too

    // Playback skip windows computed from g_events + current slider. Skipping
    // happens LIVE at playback time — tune the delay slider mid-replay and
    // the run re-stitches on the fly. stitch_err = per-cut match error on
    // the live-predictor scale (pos units + 5x vel mismatch in ups);
    // -1 = unverified fallback window (no auto match found).
    struct SkipRange { size_t start, end; int rollback_event_idx; float stitch_err; };
    static std::vector<SkipRange>  g_skip_ranges;

    // Stitch-quality report — recomputed at every skip rebuild, shown in
    // the menu so the user knows WHICH cut to distrust before playing.
    static int   g_stitch_cuts  = 0;
    static int   g_stitch_risky = 0;
    static float g_stitch_worst = 0.f;
    int   stitch_cut_count()   { return g_stitch_cuts; }
    int   stitch_risky_count() { return g_stitch_risky; }
    float stitch_worst_err()   { return g_stitch_worst; }
    bool  tape_has_phys_state() {
        return !g_tape.empty() && g_vels.size() == g_tape.size()
                               && g_flags.size() == g_tape.size();
    }

    // ─── strafe-optimizer telemetry ───────────────────────────────────
    // Ring of per-tick horizontal speeds (~4s at 66 tick) feeding the
    // on-screen gauge. meta bit0 = speed rose this tick, bit1 = optimizer
    // was steering this tick.
    static constexpr int OPT_HIST = 256;
    static float   g_opt_hist[OPT_HIST];
    static uint8_t g_opt_meta[OPT_HIST];
    static int     g_opt_n      = 0;
    static bool    g_opt_active = false;
    static int     g_opt_side   = 0;

    static void strafe_telemetry_push(float speed, bool active) {
        int idx = g_opt_n & (OPT_HIST - 1);
        float prev = g_opt_n ? g_opt_hist[(g_opt_n - 1) & (OPT_HIST - 1)] : speed;
        g_opt_hist[idx] = speed;
        g_opt_meta[idx] = (speed > prev + 0.01f ? 1 : 0) | (active ? 2 : 0);
        ++g_opt_n;
    }

    bool  strafe_opt_active() { return g_opt_active; }
    int   strafe_opt_side()   { return g_opt_side; }
    float strafe_speed() {
        return g_opt_n ? g_opt_hist[(g_opt_n - 1) & (OPT_HIST - 1)] : 0.f;
    }
    float strafe_gain_1s() {
        if (g_opt_n < 2) return 0.f;
        int back = g_opt_n < 66 ? g_opt_n - 1 : 66;
        float then = g_opt_hist[(g_opt_n - 1 - back) & (OPT_HIST - 1)];
        return strafe_speed() - then;
    }
    int strafe_sync_pct() {
        if (g_opt_n < 2) return -1;
        int back = g_opt_n < 66 ? g_opt_n - 1 : 66;
        int active = 0, gained = 0;
        for (int i = 0; i < back; ++i) {
            uint8_t m = g_opt_meta[(g_opt_n - 1 - i) & (OPT_HIST - 1)];
            if (m & 2) { ++active; if (m & 1) ++gained; }
        }
        return active ? (gained * 100) / active : -1;
    }
    int strafe_history(float* out, int max_n) {
        int n = g_opt_n < OPT_HIST ? g_opt_n : OPT_HIST;
        if (n > max_n) n = max_n;
        for (int i = 0; i < n; ++i)
            out[i] = g_opt_hist[(g_opt_n - n + i) & (OPT_HIST - 1)];
        return n;
    }
    static int                     g_skip_offset_used = -1;
    static bool                    g_skip_dirty       = true;

    // Recording-time pointer into g_events — indexes the currently "active"
    // commit that Q / 1 / 3 walk from. E resets it to the newest commit.
    static int                     g_active_commit_idx = -1;

    // Reset each play_from_start so we log a single breadcrumb on the first
    // playback tick of every new play attempt.
    static bool                    g_first_play_tick_logged = false;

    // Plugin-slot bookkeeping — see recorder.h for semantics.
    static uint32_t g_saveloc_counter = 0;
    static uint32_t g_record_slot     = 0;

    void note_saveloc_fired() {
        ++g_saveloc_counter;
        hooks::log("[SaveLoc] counter -> %u", g_saveloc_counter);
    }

    static void push_meta(EventType kind, const char* cmd) {
        TapeEvent ev{};
        ev.frame_index = g_tape.size();
        ev.kind        = kind;
        std::strncpy(ev.cmd, cmd, sizeof(ev.cmd) - 1);
        g_events.push_back(ev);
        g_skip_dirty = true;
    }

    // Player velocity at the last E-press — used by the menu overlay to
    // flag high-momentum commits that will diverge on stitch.
    static float g_last_commit_vel_ups = 0.f;

    // Real-time stitch predictor state. On each E-press we snapshot the
    // player's position + velocity as the target state a future retry must
    // re-acquire for the algorithm to find a valid stitch. Each recording
    // tick we compute the current distance and track the min-so-far.
    static int         g_commits_made         = 0;
    static hooks::Vec3 g_last_commit_pos_vec  = { 0, 0, 0 };
    static hooks::Vec3 g_last_commit_vel_vec  = { 0, 0, 0 };
    static float       g_current_stitch_dist  = 0.f;
    static float       g_min_stitch_dist      = 1e30f;

    static bool g_last_commit_in_air = false;
    static int  g_last_commit_flags       = 0;
    static bool g_last_commit_flags_known = false;

    int   commits_made()                  { return g_commits_made; }
    float current_stitch_dist()           { return g_current_stitch_dist; }
    float min_stitch_dist_since_commit()  { return g_min_stitch_dist; }
    bool  last_commit_in_air()            { return g_last_commit_in_air; }

    void commit() {
        std::lock_guard<std::recursive_mutex> lk(g_mtx);
        if (g_mode != Mode::Recording) return;
        push_meta(EventType::Saveloc, "mom_savestate_create");
        g_active_commit_idx = static_cast<int>(g_events.size()) - 1;

        // Snapshot the target state for the live stitch predictor.
        hooks::Vec3 vel{}, pos{};
        if (hooks::get_local_player_vel(vel)) {
            g_last_commit_vel_ups = std::sqrt(vel.x*vel.x + vel.y*vel.y + vel.z*vel.z);
            g_last_commit_vel_vec = vel;
            // |vz| > ~50 = player is either mid-jump or mid-fall. that means
            // FL_ONGROUND=false, so any "matching" retry frame that happens
            // to be on the ground will have different physics from this one
            // — playback drift is inevitable. flag it.
            g_last_commit_in_air = std::fabs(vel.z) > 50.f;
        }
        if (hooks::get_local_player_pos(pos)) {
            g_last_commit_pos_vec = pos;
        }
        int cf = 0;
        g_last_commit_flags_known = hooks::get_local_player_flags(cf);
        g_last_commit_flags       = cf;
        if (g_last_commit_flags_known) {
            // Authoritative ground state — replaces the |vz| heuristic above
            // (a fast horizontal ramp glide has vz≈0 but is NOT grounded).
            g_last_commit_in_air = !(cf & FL_ONGROUND);
        }
        ++g_commits_made;
        g_min_stitch_dist     = 1e30f;   // reset so we track re-acquisition for THIS segment
        g_current_stitch_dist = 0.f;

        hooks::log("[Recorder] commit meta @ frame=%zu (active=%d, events=%zu, vel=%.0f ups)",
                   g_tape.size(), g_active_commit_idx, g_events.size(),
                   g_last_commit_vel_ups);
    }

    float last_commit_velocity_ups() { return g_last_commit_vel_ups; }

    // Playback drift telemetry — updated each tick from playback_tick_body.
    static int   g_pb_current_segment = 0;
    static float g_pb_drift_units     = 0.f;
    static int   g_pb_broken_segment  = -1;

    int   playback_current_segment() { return g_pb_current_segment; }
    float playback_drift_units()     { return g_pb_drift_units; }
    int   playback_broken_segment()  { return g_pb_broken_segment; }

    void rollback() {
        std::lock_guard<std::recursive_mutex> lk(g_mtx);
        if (g_mode != Mode::Recording) return;
        // Momentum semantics (mirror KSF): mom_savestate_load teleports to
        // the currently SELECTED savestate — wherever E/prev/next last left
        // the pointer — NOT automatically the newest commit. E selects the
        // newest on every press, so plain E/Q runs behave the same; the
        // pointer only diverges after a 1/3.
        if (g_active_commit_idx < 0) {
            hooks::log("[Recorder] Q IGNORED (no commits)");
            return;
        }
        push_meta(EventType::Teleport, "mom_savestate_load");
        hooks::log("[Recorder] Q rollback -> active idx=%d (commit frame=%zu)",
                   g_active_commit_idx,
                   g_events[g_active_commit_idx].frame_index);
    }

    // Which Saveloc commit is selected (1-based), and how many exist. For the
    // on-screen "Segment N / M" readout so prev/next navigation is visible.
    int selected_segment() {
        std::lock_guard<std::recursive_mutex> lk(g_mtx);
        if (g_active_commit_idx < 0) return 0;
        int n = 0;
        for (int i = 0; i <= g_active_commit_idx && i < (int)g_events.size(); ++i)
            if (g_events[i].kind == EventType::Saveloc) ++n;
        return n;
    }
    int segment_count() {
        std::lock_guard<std::recursive_mutex> lk(g_mtx);
        int n = 0;
        for (auto& e : g_events) if (e.kind == EventType::Saveloc) ++n;
        return n;
    }

    void rollback_prev() {
        std::lock_guard<std::recursive_mutex> lk(g_mtx);
        // Works while Recording OR Idle (navigate the segments you saved).
        if (g_mode != Mode::Recording && g_mode != Mode::Idle) return;
        if (g_active_commit_idx <= 0) {
            hooks::log("[Recorder] /prevloc IGNORED (already at first segment)");
            return;
        }
        int prev = -1;
        for (int i = g_active_commit_idx - 1; i >= 0; --i) {
            if (g_events[i].kind == EventType::Saveloc) { prev = i; break; }
        }
        if (prev < 0) {
            hooks::log("[Recorder] /prevloc IGNORED (no earlier segment)");
            return;
        }
        g_active_commit_idx = prev;
        if (g_mode == Mode::Recording)      // tape event only matters while recording
            push_meta(EventType::Prevloc, "mom_savestate_prev");
        hooks::log("[Recorder] /prevloc -> segment %d/%d (idx=%d frame=%zu)",
                   selected_segment(), segment_count(), prev, g_events[prev].frame_index);
    }

    void rollback_next() {
        std::lock_guard<std::recursive_mutex> lk(g_mtx);
        if (g_mode != Mode::Recording && g_mode != Mode::Idle) return;
        if (g_active_commit_idx < 0) {
            hooks::log("[Recorder] /nextloc IGNORED (no segments yet)");
            return;
        }
        int next = -1;
        for (int i = g_active_commit_idx + 1; i < static_cast<int>(g_events.size()); ++i) {
            if (g_events[i].kind == EventType::Saveloc) { next = i; break; }
        }
        if (next < 0) {
            hooks::log("[Recorder] /nextloc IGNORED (already at newest segment)");
            return;
        }
        g_active_commit_idx = next;
        if (g_mode == Mode::Recording)
            push_meta(EventType::Nextloc, "mom_savestate_next");
        hooks::log("[Recorder] /nextloc -> segment %d/%d (idx=%d frame=%zu)",
                   selected_segment(), segment_count(), next, g_events[next].frame_index);
    }

    // Build the playback skip windows from g_events + current slider. Walks
    // the same active-commit pointer the recorder used at Q/1/3 time so we
    // know exactly which commit each rollback event targeted:
    //   Teleport (Q)     -> latest commit before the event
    //   Prevloc  (1)     -> walk pointer back one commit
    //   Nextloc  (3)     -> walk pointer forward one commit
    // Each rollback produces a skip window
    //   [target_commit_frame + offset + 1, rollback_frame + offset + 1)
    // then overlapping windows are merged (so /prevloc's broader skip
    // absorbs any nested Q's from the segment the user gave up on).
    struct AutoMatch {
        size_t commit_frame;
        size_t snap_frame;
        float  dist;          // dist of the accepted (or best rejected) match
        float  best_seen;     // absolute smallest dist we saw during search
        float  best_q_dist;   // smallest Q-press vs saveloc distance we saw
        bool   ok;
        float  err;           // match error on the live-predictor scale
        bool   no_teleport;   // no landing exists after the event — server
                              // dropped the cmd; the event must not cut
        size_t landing;       // detected teleport landing frame
        size_t resume;        // first frame MOVING again after the landing —
                              // TRIX-style servers freeze the player at the
                              // loc for a pause; == landing on KSF (no pause)
        bool   landing_ok;    // landing is valid (discontinuity was found)
    };
    // Find the FIRST post-rollback frame whose position is within tolerance
    // of SOME commit-side frame — but only if the player was clearly far
    // from that commit-side position at the moment Q was pressed. That
    // "was far, now near" transition is the actual /teleport landing on
    // the client. Without the "was far" guard the algorithm falsely locks
    // onto coincidental same-position matches (player revisits the same
    // spot on a ramp) and produces useless skip windows.
    // Velocity at frame k in units-per-tick. Prefers the tape's real
    // m_vecVelocity (v4 tapes) — exact, and immune to the discontinuity
    // spike finite differencing produces right at a teleport (pos jumps
    // hundreds of units in one tick, reading as absurd "velocity").
    // Falls back to position[k] - position[k-1] on older tapes.
    static hooks::Vec3 vel_at(size_t k) {
        if (k < g_vels.size() && g_vels.size() == g_positions.size()) {
            const auto& v = g_vels[k];
            return { v.x / TICKS_PER_SEC, v.y / TICKS_PER_SEC, v.z / TICKS_PER_SEC };
        }
        if (k == 0 || k >= g_positions.size()) return { 0, 0, 0 };
        const auto& a = g_positions[k - 1];
        const auto& b = g_positions[k];
        return { b.x - a.x, b.y - a.y, b.z - a.z };
    }

    static AutoMatch auto_match_delay(size_t target_frame, size_t event_frame) {
        AutoMatch r{ target_frame, event_frame, 1e30f, 1e30f, 0.f, false, 0.f, false, 0, 0, false };
        if (g_positions.size() < 2 || event_frame >= g_positions.size()) return r;

        constexpr size_t COMMIT_LOOK_BEFORE = 3;
        constexpr size_t COMMIT_LOOK_AFTER  = 90;
        constexpr size_t POST_MAX_FRAMES    = 500;
        constexpr float  POS_TOLERANCE      = 5.f;  // MUCH tighter — only accept near-exact positional matches
        constexpr float  VEL_TOLERANCE      = 3.f;  // and near-exact velocity matches (~200 ups tolerance)
        constexpr float  FAR_MULT           = 100.f; // Q-press must be > 50 units from saveloc
        constexpr float  VEL_WEIGHT         = 20.f;  // velocity match dominates the score
        constexpr float  AIR_CUT_PENALTY    = 60.f;  // prefer grounded cuts — friction on the ground
                                                     // CONTRACTS a small state error each tick, while
                                                     // airborne ramp physics amplifies it. a slightly
                                                     // worse grounded pair beats a better airborne one.
        constexpr int    MIN_ACCEL_FRAMES   = 2;    // was 10 — that guard predates velocity matching and
                                                     // skipped right past the exact teleport landing on
                                                     // servers that restore velocity on savestate load (the
                                                     // landing frame matches the saved state bit-exactly:
                                                     // the PERFECT cut). The parked "just teleported,
                                                     // vel=0" moment it guarded against can't pass the
                                                     // velocity check against a moving commit anyway.

        // Hidden-state data is only usable when the array is truly parallel
        // to the tape (v4 recording, no stale tail from an edit).
        const bool have_flags = !g_flags.empty() && g_flags.size() == g_positions.size();

        size_t c_lo = target_frame > COMMIT_LOOK_BEFORE ? target_frame - COMMIT_LOOK_BEFORE : 0;
        size_t c_hi = std::min({ g_positions.size(),
                                 target_frame + COMMIT_LOOK_AFTER + 1,
                                 event_frame + 1 }); // CRITICAL: never past the Q-press
        if (c_hi <= c_lo) return r;

        // Small guard after the Q-press for teleport-tick jitter only. The
        // frames between Q and the landing sit at the fail spot, >50 units
        // from any commit-side candidate, so the position check rejects
        // them on its own — the ideal cut is the landing itself.
        size_t p_lo = event_frame + 1 + MIN_ACCEL_FRAMES;
        size_t p_hi = std::min(g_positions.size(), event_frame + POST_MAX_FRAMES + 1);
        if (p_hi <= p_lo) return r;

        // Floor the search at this rollback's ACTUAL landing — the first
        // post-event frame whose step deviates from its own recorded
        // velocity by a teleport-sized jump. Needed for Q double-taps:
        // after the FIRST teleport lands, the player retraces the commit
        // path exactly (same saved state, same ballistic flight), so the
        // frames between the second Q-press and its landing are bit-clones
        // of commit-side frames. Pos+vel+flags matching locks onto one of
        // those shadows, the tape then rewinds one tick later at the real
        // landing, and the splice plays a future the recording never had
        // (observed: end+1 sitting 169-228 units off at the cut audit).
        {
            bool landing_found = false;
            size_t scan_hi = std::min(g_positions.size(), event_frame + 121);
            for (size_t k = event_frame + 1; k < scan_hi; ++k) {
                hooks::Vec3 v = vel_at(k - 1); // units per tick
                const auto& a = g_positions[k - 1];
                const auto& b = g_positions[k];
                float ex = b.x - (a.x + v.x);
                float ey = b.y - (a.y + v.y);
                float ez = b.z - (a.z + v.z);
                if (ex*ex + ey*ey + ez*ez > 40.f * 40.f) {
                    landing_found = true;
                    r.landing    = k;
                    r.landing_ok = true;
                    // TRIX-style servers freeze the player at the loc for a
                    // pause after the teleport; those parked frames can
                    // match a standing commit and splice mid-freeze — then
                    // playback plays the leftover frozen frames with no
                    // server freezing it live, and any keys held during
                    // the pause walk the run off the line. Measure the
                    // pause from the tape (no config needed — the length
                    // is whatever the server did) and floor the cut at the
                    // first frame that MOVES again. KSF restores motion on
                    // the very next frame, so this is a no-op there.
                    size_t resume = k;
                    while (resume + 1 < g_positions.size() &&
                           resume < k + 400) {
                        const auto& f0 = g_positions[resume];
                        const auto& f1 = g_positions[resume + 1];
                        float mx = f1.x - f0.x, my = f1.y - f0.y, mz = f1.z - f0.z;
                        if (mx*mx + my*my + mz*mz > 0.25f) break; // moving again
                        ++resume;
                    }
                    r.resume = resume;
                    if (resume > k)
                        hooks::log("      landing %zu + %zu-tick freeze -> resume floor %zu",
                                   k, resume - k, resume);
                    if (r.resume > p_lo) p_lo = r.resume;
                    break;
                }
            }
            // No teleport-sized jump anywhere after the press: the server
            // dropped the command (flood limit on rapid Q's) and the tape
            // simply continues the same attempt. The event is fiction —
            // there is nothing to cut, and fabricating a window here slices
            // out real gameplay and splices to a near-miss state (observed:
            // an err-309 cut seeding 61 ups of error that broke seg 6).
            if (!landing_found) {
                r.no_teleport = true;
                return r;
            }
            if (p_hi <= p_lo) return r;
        }

        float pos_tol_sq = POS_TOLERANCE * POS_TOLERANCE;
        float vel_tol_sq = VEL_TOLERANCE * VEL_TOLERANCE;
        float far_sq     = pos_tol_sq * FAR_MULT;
        const auto& q_pos = g_positions[event_frame]; // where the client was when Q fired

        // For each post-rollback frame, sweep EVERY commit-side candidate
        // (not just the closest). Accept the first (post, commit) pair that
        // simultaneously satisfies (a) they match within tolerance and
        // (b) the Q-press was far from that specific commit-side position.
        // The previous "closest match per post-frame" strategy failed when
        // a coincidental same-spot match at a nearby commit-side frame
        // shadowed a real snap-back to a different commit-side frame.
        // Find the (i, j) pair whose position AND velocity match — that's
        // the true "same physical state" boundary, not just "same position."
        // Position match alone gave us the frame right after teleport
        // landing, where velocity was zero. But playback at that cut moment
        // has whatever velocity segment 1 built up (usually high, since E
        // was pressed mid-surf). Applying retry inputs (designed for zero
        // velocity) to a fast-moving player = wrong physics = failed run.
        // Solution: also match velocity, so the cut lands where segment 2's
        // retry has ALREADY accelerated back to segment 1's ending velocity.
        float best_score = 1e30f;
        size_t best_i = event_frame, best_j = target_frame;
        float best_pos_d2 = 0.f, best_vel_d2 = 0.f;
        for (size_t i = p_lo; i < p_hi; ++i) {
            if (i == 0) continue;
            const auto& p_i = g_positions[i];
            hooks::Vec3 v_i = vel_at(i);
            for (size_t j = c_lo; j < c_hi; ++j) {
                if (j == 0) continue;
                const auto& p_j = g_positions[j];
                float dx = p_i.x - p_j.x, dy = p_i.y - p_j.y, dz = p_i.z - p_j.z;
                float pos_d2 = dx*dx + dy*dy + dz*dz;
                if (pos_d2 < r.best_seen) r.best_seen = pos_d2;
                if (pos_d2 > pos_tol_sq) continue;

                float qx = q_pos.x - p_j.x, qy = q_pos.y - p_j.y, qz = q_pos.z - p_j.z;
                float q_d2 = qx*qx + qy*qy + qz*qz;
                if (q_d2 > r.best_q_dist) r.best_q_dist = q_d2;
                if (q_d2 < far_sq) continue;

                // Hidden-state guard: same pos+vel with different ground/
                // duck/water bits is a DIFFERENT physics state (e.g. the
                // landing frame vs the fly-through frame at the same spot
                // on a ramp) — stitching across it guarantees divergence.
                if (have_flags && menu::g_match_require_flags &&
                    ((g_flags[i] ^ g_flags[j]) & PHYS_FLAGS_MASK)) continue;

                hooks::Vec3 v_j = vel_at(j);
                float vdx = v_i.x - v_j.x, vdy = v_i.y - v_j.y, vdz = v_i.z - v_j.z;
                float vel_d2 = vdx*vdx + vdy*vdy + vdz*vdz;
                if (vel_d2 > vel_tol_sq) continue;

                // Never splice onto a frame the tape rewinds right after —
                // its recorded future contains a teleport that playback
                // won't perform. Backup for the landing floor above (e.g.
                // when a flood-dropped teleport leaves no discontinuity to
                // find and shadow frames survive into the search range).
                if (i + 1 < g_positions.size()) {
                    hooks::Vec3 vi1 = vel_at(i);
                    const auto& pa = g_positions[i];
                    const auto& pb = g_positions[i + 1];
                    float tx = pb.x - (pa.x + vi1.x);
                    float ty = pb.y - (pa.y + vi1.y);
                    float tz = pb.z - (pa.z + vi1.z);
                    if (tx*tx + ty*ty + tz*tz > 40.f * 40.f) continue;
                }

                float score = pos_d2 + vel_d2 * VEL_WEIGHT;
                if (have_flags && !(g_flags[i] & FL_ONGROUND)) score += AIR_CUT_PENALTY;
                if (score < best_score) {
                    best_score  = score;
                    best_i      = i;
                    best_j      = j;
                    best_pos_d2 = pos_d2;
                    best_vel_d2 = vel_d2;
                }
            }
        }
        if (best_score < 1e30f) {
            r.commit_frame = best_j;
            r.snap_frame   = best_i;
            r.dist         = std::sqrt(best_pos_d2);
            // Human-scale match error: pos units + 5x velocity mismatch in
            // ups — the SAME scale as the live predictor's cur/best readout,
            // so "err 80" here means what "best 80" meant while recording.
            r.err          = std::sqrt(best_pos_d2)
                           + std::sqrt(best_vel_d2) * TICKS_PER_SEC * 5.f;
            r.ok           = true;
            hooks::log("      match  i=%zu j=%zu pos_d=%.1f vel_d=%.1f score=%.1f err=%.0f%s",
                       best_i, best_j,
                       std::sqrt(best_pos_d2), std::sqrt(best_vel_d2), best_score, r.err,
                       (have_flags && !(g_flags[best_i] & FL_ONGROUND)) ? " (air cut)" : "");
            return r;
        }

        r.best_seen   = r.best_seen   >= 1e30f ? 0.f : std::sqrt(r.best_seen);
        r.best_q_dist = r.best_q_dist <= 0.f ? 0.f : std::sqrt(r.best_q_dist);
        return r;
    }

    static void rebuild_skip_ranges() {
        g_skip_ranges.clear();
        int fallback_off = std::max(0, menu::g_saveloc_delay_frames);
        bool can_auto = menu::g_saveloc_auto_delay
                        && g_positions.size() == g_tape.size()
                        && !g_positions.empty();

        // Sanity: dump the first/mid/last recorded positions. If they're
        // all (0,0,0) or identical, the netvar read is broken and every
        // AUTO_MISS with best_dist=0 will make sense.
        if (!g_positions.empty()) {
            size_t mid = g_positions.size() / 2;
            const auto& p0 = g_positions.front();
            const auto& pm = g_positions[mid];
            const auto& pn = g_positions.back();
            hooks::log("[Recorder] position sample  first=(%.0f,%.0f,%.0f)  "
                       "mid=(%.0f,%.0f,%.0f)  last=(%.0f,%.0f,%.0f)",
                       p0.x, p0.y, p0.z, pm.x, pm.y, pm.z, pn.x, pn.y, pn.z);
        }

        int active = -1;
        for (size_t i = 0; i < g_events.size(); ++i) {
            const auto& ev = g_events[i];
            if (ev.kind == EventType::Saveloc) {
                active = static_cast<int>(i);
                continue;
            }
            // Rollback — resolve active pointer per its flavor, matching
            // Momentum/KSF: mom_savestate_load targets the SELECTED
            // savestate without moving the pointer; only E (append+select
            // newest) and prev/next move it. The old walker reset Q to the
            // newest commit, which diverged from the game whenever 1/3
            // had been used.
            if (ev.kind == EventType::Teleport) {
                // selection unchanged
            } else if (ev.kind == EventType::Prevloc) {
                if (active > 0) {
                    for (int j = active - 1; j >= 0; --j) {
                        if (g_events[j].kind == EventType::Saveloc) { active = j; break; }
                    }
                }
            } else if (ev.kind == EventType::Nextloc) {
                if (active >= 0) {
                    for (size_t j = static_cast<size_t>(active) + 1; j < i; ++j) {
                        if (g_events[j].kind == EventType::Saveloc) { active = static_cast<int>(j); break; }
                    }
                }
            }
            if (active < 0) continue;
            size_t target_frame = g_events[active].frame_index;

            bool auto_ok = false;
            if (can_auto) {
                AutoMatch m = auto_match_delay(target_frame, ev.frame_index);
                if (m.no_teleport) {
                    // Dropped teleport — ignore the event completely. The
                    // fallback window must NOT run either: it would cut real
                    // gameplay that the server never rolled back.
                    hooks::log("  ev[%zu] NO_TELEPORT  target=%zu ev=%zu — no landing in tape "
                               "(server dropped the cmd); event ignored, no cut",
                               i, target_frame, ev.frame_index);
                    continue;
                }
                if (m.ok) {
                    // Resume ON the matched frame. positions[k] is the state
                    // BEFORE frame k's inputs run, so after playing frames
                    // [.., j-1] the live state is ≈ positions[j] ≈
                    // positions[i] — exactly the state frame i's inputs were
                    // recorded against. The old [j+1, i+1) window instead
                    // played frame j and resumed at i+1, baking one hybrid
                    // tick (segment-1 input applied to a state the retry
                    // never saw) into every single cut.
                    size_t s = m.commit_frame;
                    size_t e = m.snap_frame;
                    if (s < e) {
                        g_skip_ranges.push_back({ s, e, static_cast<int>(i), m.err });
                        auto_ok = true;
                        hooks::log("  ev[%zu] AUTO  target=%zu ev=%zu match=%zu snap=%zu dist=%.1f err=%.0f -> [%zu,%zu)",
                                   i, target_frame, ev.frame_index,
                                   m.commit_frame, m.snap_frame, m.dist, m.err, s, e);
                    }
                }
                if (!auto_ok) {
                    hooks::log("  ev[%zu] AUTO_MISS  target=%zu ev=%zu best_seen=%.1f (tol=5)  best_q_dist=%.1f (need>50)",
                               i, target_frame, ev.frame_index,
                               m.best_seen, m.best_q_dist);
                }
                // No state match, but the teleport's landing IS known.
                // savestate_create lags the E-press by the same delay
                // savestate_load lags the Q, so cut [target + delay, landing)
                // — the landing frame's state is the saved state, and
                // target+delay is our best estimate of the frame it was
                // saved at. Far better than the blind slider window, which
                // fabricated the fiction cuts that broke chaotic tapes
                // (map-teleporter mid-run, Q-mash inside the far-guard).
                if (!auto_ok && m.landing_ok) {
                    // Start offset uses the TELEPORT delay only (saveloc
                    // lags E by the same server trip); the end skips past
                    // any TRIX-style freeze so playback resumes at the
                    // first frame the recording was actually moving.
                    size_t d = m.landing - ev.frame_index;
                    size_t s = target_frame + d;
                    size_t e = m.resume;
                    if (s < e) {
                        g_skip_ranges.push_back({ s, e, static_cast<int>(i), -1.f });
                        auto_ok = true;
                        hooks::log("  ev[%zu] LANDING_CUT  target=%zu ev=%zu delay=%zu freeze=%zu -> [%zu,%zu)",
                                   i, target_frame, ev.frame_index, d,
                                   m.resume - m.landing, s, e);
                    }
                }
            }
            if (!auto_ok) {
                size_t s = target_frame + static_cast<size_t>(fallback_off) + 1;
                size_t e = ev.frame_index + static_cast<size_t>(fallback_off) + 1;
                if (s < e) {
                    g_skip_ranges.push_back({ s, e, static_cast<int>(i), -1.f });
                    hooks::log("  ev[%zu] FALLBACK  target=%zu ev_frame=%zu -> [%zu,%zu)",
                               i, target_frame, ev.frame_index, s, e);
                } else {
                    hooks::log("  ev[%zu] SKIPPED (s>=e)  target=%zu ev_frame=%zu",
                               i, target_frame, ev.frame_index);
                }
            }
        }

        // Overlapping windows: keep the pair with the largest end WHOLESALE.
        // Each auto window is a matched (start=j, end=i) pair — "state
        // before frame i equals state before frame j". The old min-start/
        // max-end merge spliced frame i of one pair onto frame j of a
        // DIFFERENT pair, teleporting playback by however far the two j's
        // are apart: a Q-mash group with windows [278,435) [278,452)
        // [309,494) merged to [278,494), resumed at 494 (whose partner is
        // 309, not 278) and broke the run by 542 units. Only the LAST
        // rollback of an overlapping group defines the retry that
        // survived, so its pair wins outright. Frames between an earlier
        // pair's start and the winner's start are pre-fail commit-side
        // frames — correct to play.
        if (!g_skip_ranges.empty()) {
            std::sort(g_skip_ranges.begin(), g_skip_ranges.end(),
                      [](const SkipRange& a, const SkipRange& b) { return a.start < b.start; });
            std::vector<SkipRange> merged;
            merged.push_back(g_skip_ranges.front());
            for (size_t i = 1; i < g_skip_ranges.size(); ++i) {
                if (g_skip_ranges[i].start < merged.back().end) {
                    if (g_skip_ranges[i].end > merged.back().end)
                        merged.back() = g_skip_ranges[i];
                } else {
                    merged.push_back(g_skip_ranges[i]);
                }
            }
            g_skip_ranges = std::move(merged);
        }

        // Stitch report: a cut is RISKY when its match error exceeds the
        // menu threshold, or when it's an unverified fallback window
        // (err < 0). Playing a failed attempt inline instead of cutting is
        // NOT an option timer-safe — the post-Q frames were recorded after
        // the savestate load landed, so replaying them without firing
        // savestate_load desyncs far worse than any cut. Risky cuts still cut;
        // the report tells the user which segment to re-record.
        g_stitch_cuts  = static_cast<int>(g_skip_ranges.size());
        g_stitch_risky = 0;
        g_stitch_worst = 0.f;
        for (const auto& rr : g_skip_ranges) {
            if (rr.stitch_err < 0.f || rr.stitch_err > menu::g_stitch_max_err) ++g_stitch_risky;
            if (rr.stitch_err > g_stitch_worst) g_stitch_worst = rr.stitch_err;
        }
        if (g_stitch_cuts > 0) {
            hooks::log("[Recorder] stitch report: %d cut(s), %d risky (limit=%.0f), worst err=%.0f",
                       g_stitch_cuts, g_stitch_risky, menu::g_stitch_max_err, g_stitch_worst);
        }

        g_skip_offset_used = fallback_off;
        g_skip_dirty       = false;
        // Dump the resulting windows for diagnosis of "runs still messing up".
        hooks::log("[Recorder] rebuild_skips  auto=%d fallback_off=%d positions=%zu/%zu -> %zu range(s)",
                   can_auto ? 1 : 0, fallback_off,
                   g_positions.size(), g_tape.size(), g_skip_ranges.size());
        size_t dump_n = std::min<size_t>(g_skip_ranges.size(), 8);
        for (size_t i = 0; i < dump_n; ++i) {
            hooks::log("  skip[%zu] = [%zu, %zu)  width=%zu",
                       i, g_skip_ranges[i].start, g_skip_ranges[i].end,
                       g_skip_ranges[i].end - g_skip_ranges[i].start);
        }
    }

    // Track the auto-toggle (and the new matcher settings) so mid-playback
    // changes force a rebuild.
    static bool  g_skip_auto_used  = false;
    static bool  g_skip_flags_used = true;
    static float g_skip_conf_used  = -1.f;

    static void maybe_rebuild_skips() {
        int cur_off = std::max(0, menu::g_saveloc_delay_frames);
        bool cur_auto = menu::g_saveloc_auto_delay;
        if (g_skip_dirty || cur_off != g_skip_offset_used || cur_auto != g_skip_auto_used ||
            menu::g_match_require_flags != g_skip_flags_used ||
            menu::g_stitch_max_err      != g_skip_conf_used) {
            rebuild_skip_ranges();
            g_skip_auto_used  = cur_auto;
            g_skip_flags_used = menu::g_match_require_flags;
            g_skip_conf_used  = menu::g_stitch_max_err;
        }
    }
    uint32_t record_slot() { return g_record_slot; }
    void reset_saveloc_counter() {
        g_saveloc_counter = 0;
        g_record_slot     = 0;
        hooks::log("[SaveLoc] counter reset");
    }

    // Debounce: SM chat + our hotkey path can both fire from a single user
    // action (e.g. we log "say /saveloc" AND the chat input echoes back
    // through the ClientCmd hook). Also guards against server-side flood
    // detectors when a user mashes E rapidly.
    static ULONGLONG g_last_save_ms = 0;
    static ULONGLONG g_last_tele_ms = 0;
    static constexpr ULONGLONG EVENT_MIN_GAP_MS = 400;

    void record_event(EventType kind, const char* cmd) {
        std::lock_guard<std::recursive_mutex> lk(g_mtx);
        if (g_mode != Mode::Recording) return;

        ULONGLONG now = GetTickCount64();
        ULONGLONG& last = (kind == EventType::Saveloc) ? g_last_save_ms : g_last_tele_ms;
        if (now - last < EVENT_MIN_GAP_MS) {
            hooks::log("[SaveLoc] skipped dupe event kind=%d (last %llums ago)",
                       (int)kind, now - last);
            return;
        }
        last = now;

        TapeEvent ev{};
        ev.frame_index = g_tape.size();
        ev.kind        = kind;
        if (cmd) {
            std::strncpy(ev.cmd, cmd, sizeof(ev.cmd) - 1);
            ev.cmd[sizeof(ev.cmd) - 1] = 0;
        }
        g_events.push_back(ev);
        hooks::log("[SaveLoc] recorded event kind=%d cmd='%s' @ frame=%zu (total=%zu)",
                   (int)kind, ev.cmd, ev.frame_index, g_events.size());
    }
    static bool                g_loop        = false;
    static float               g_speed       = 1.f;
    static float               g_speed_accum = 0.f;

    // Saved position/angles from record start — used for walk-to-start on play.
    static hooks::Vec3         g_start_pos    = { 0, 0, 0 };
    static QAngle              g_start_angles = { 0, 0, 0 };
    static bool                g_start_saved  = false;
    static constexpr float     SEEK_ARRIVE_RADIUS   = 15.f;
    static constexpr float     SEEK_STUCK_DELTA     = 0.3f; // dist not shrinking by this per tick
    static constexpr int       SEEK_STUCK_TICKS     = 30;   // consecutive no-progress ticks → give up
    static constexpr float     SETTLE_STABLE_DELTA  = 0.2f;
    static constexpr int       SETTLE_STABLE_TICKS  = 33;
    static constexpr int       SETTLE_MIN_TOTAL_TICKS = 66;
    static hooks::Vec3         g_last_pos{};
    static float               g_last_dist       = 0.f;
    static int                 g_stuck_ticks     = 0;
    static int                 g_stable_ticks    = 0;
    static int                 g_settling_ticks  = 0;

    Mode   mode()         { return g_mode; }
    size_t tape_size()    { return g_tape.size(); }
    size_t play_head()    { return g_head; }
    bool   looping()      { return g_loop; }
    float  play_speed()   { return g_speed; }

    void   set_play_head(size_t v) {
        std::lock_guard<std::recursive_mutex> lk(g_mtx);
        g_head = v > g_tape.size() ? g_tape.size() : v;
    }
    void   set_looping(bool v)     { g_loop = v; }
    void   set_play_speed(float v) {
        if (v < 0.1f) v = 0.1f;
        if (v > 5.f)  v = 5.f;
        g_speed = v;
    }

    void start_record() {
        std::lock_guard<std::recursive_mutex> lk(g_mtx);
        g_tape.clear();
        g_events.clear();
        g_positions.clear();
        g_vels.clear();
        g_flags.clear();
        g_commits_made         = 0;
        g_current_stitch_dist  = 0.f;
        g_min_stitch_dist      = 1e30f;
        g_last_commit_vel_ups  = 0.f;
        g_last_commit_flags_known = false;
        g_skip_ranges.clear();
        g_skip_dirty = true;
        g_active_commit_idx = -1;
        g_head = 0;
        g_speed_accum = 0.f;

        g_start_saved = hooks::get_local_player_pos(g_start_pos);

        g_mode = Mode::Recording;

        // Fire the initial savestate so mid-run Q's have something to
        // teleport back to, and seed a matching checkpoint at frame 0 in
        // the tape. If you Q before ever pressing E, we roll all the way
        // back to frame 0 (fresh restart from the start position).
        if (menu::g_saveloc_mode) {
            note_saveloc_fired();
            hooks::execute_server_cmd("mom_savestate_create");
            TapeEvent ev{};
            ev.frame_index = 0;
            ev.kind = EventType::Saveloc;
            std::strncpy(ev.cmd, "mom_savestate_create", sizeof(ev.cmd) - 1);
            g_events.push_back(ev);
            g_active_commit_idx = static_cast<int>(g_events.size()) - 1;
            g_skip_dirty = true;
        }

        hooks::log("[Recorder] START_RECORD  saveloc_mode=%d  events=%zu",
                   menu::g_saveloc_mode ? 1 : 0, g_events.size());
    }

    void play_from_start() {
        std::lock_guard<std::recursive_mutex> lk(g_mtx);
        if (g_tape.empty()) {
            hooks::log("[Recorder] PLAY_FROM_START ignored — tape empty");
            return;
        }
        g_first_play_tick_logged = false;
        // Reset playback drift telemetry so the overlay reads clean.
        g_pb_current_segment = 0;
        g_pb_drift_units     = 0.f;
        g_pb_broken_segment  = -1;
        hooks::log("[Recorder] PLAY_FROM_START  tape_frames=%zu events=%zu skips=%zu  "
                   "flags: rev=%d side=%d saveloc=%d strafe=%d",
                   g_tape.size(), g_events.size(), g_skip_ranges.size(),
                   menu::g_reverse_playback ? 1 : 0,
                   menu::g_sideways_playback? 1 : 0,
                   menu::g_saveloc_mode     ? 1 : 0,
                   menu::g_strafe_optimizer ? 1 : 0);
        g_head = 0;
        g_speed_accum = 0.f;

        // Start-state audit: how far the live player is from the recorded
        // start BEFORE any frame plays. Any offset here is seed error no
        // later cut can remove — it compounds through the whole run.
        if (!g_positions.empty()) {
            hooks::Vec3 live{};
            if (hooks::get_local_player_pos(live)) {
                float dx = live.x - g_positions[0].x;
                float dy = live.y - g_positions[0].y;
                float dz = live.z - g_positions[0].z;
                float d0 = std::sqrt(dx*dx + dy*dy + dz*dz);
                hooks::log("[Recorder] PLAY start offset = %.1f units%s", d0,
                           d0 > 2.f ? "  (WARN >2: teleport to the run's first"
                                      " saveloc before F5 for an exact start)"
                                    : "");
            }
        }

        const Frame& f0 = g_tape[0];
        g_start_angles = f0.viewangles;

        // Deliberately no auto /teleport on PLAY — user positions themselves
        // manually before pressing play. Playback then drives inputs and
        // fires recorded /saveloc + /teleport events at their tape offsets.

        // Teleport mode: fire setpos + setang via the engine's console. Only
        // works on servers with sv_cheats 1, silently no-ops otherwise.
        if (menu::g_teleport_to_start && g_start_saved) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "setpos %.3f %.3f %.3f; setang %.4f %.4f %.4f",
                g_start_pos.x, g_start_pos.y, g_start_pos.z,
                f0.viewangles.pitch, f0.viewangles.yaw, f0.viewangles.roll);
            hooks::execute_client_cmd(buf);
            g_mode = Mode::Playing;
            return;
        }

        // Walk-to-start fallback for non-sv_cheats servers.
        if (g_start_saved) {
            hooks::Vec3 cur;
            if (hooks::get_local_player_pos(cur)) {
                float dx = g_start_pos.x - cur.x;
                float dy = g_start_pos.y - cur.y;
                float dist = std::sqrt(dx * dx + dy * dy);
                if (dist > SEEK_ARRIVE_RADIUS) {
                    g_mode = Mode::Seeking;
                    return;
                }
                // Close enough that no walk is needed — but still settle.
                // Playback used to start instantly from here, inheriting
                // whatever walking velocity the player had at F5 as seed
                // error the whole run then carries. Settling holds neutral
                // input until still, fires the recorded setang, then plays.
                g_mode = Mode::Settling;
                g_last_pos = cur;
                g_stable_ticks = 0;
                g_settling_ticks = 0;
                return;
            }
        }
        g_mode = Mode::Playing;
    }

    void resume_playback() {
        std::lock_guard<std::recursive_mutex> lk(g_mtx);
        if (g_tape.empty()) {
            hooks::log("[Recorder] RESUME ignored — tape empty");
            return;
        }
        g_first_play_tick_logged = false;
        g_pb_current_segment = 0;
        g_pb_drift_units     = 0.f;
        g_pb_broken_segment  = -1;
        hooks::log("[Recorder] RESUME  head=%zu tape_frames=%zu events=%zu skips=%zu  "
                   "flags: rev=%d side=%d saveloc=%d strafe=%d",
                   g_head, g_tape.size(), g_events.size(), g_skip_ranges.size(),
                   menu::g_reverse_playback ? 1 : 0,
                   menu::g_sideways_playback? 1 : 0,
                   menu::g_saveloc_mode     ? 1 : 0,
                   menu::g_strafe_optimizer ? 1 : 0);
        g_speed_accum = 0.f;
        g_mode = Mode::Playing;
    }

    void pause() {
        std::lock_guard<std::recursive_mutex> lk(g_mtx);
        g_mode = Mode::Idle;
    }
    void stop()  {
        std::lock_guard<std::recursive_mutex> lk(g_mtx);
        hooks::log("[Recorder] STOP  tape_frames=%zu  events=%zu",
                   g_tape.size(), g_events.size());
        g_mode = Mode::Idle; g_head = 0; g_speed_accum = 0.f;
    }

    // Keep every parallel physics array in lockstep with the tape after an
    // edit — a stale tail fails the size-equality checks and silently turns
    // auto delay-comp (and the flag matcher) off for the whole tape.
    static void trim_parallel_arrays(size_t n) {
        if (g_positions.size() > n) g_positions.resize(n);
        if (g_vels.size()      > n) g_vels.resize(n);
        if (g_flags.size()     > n) g_flags.resize(n);
    }

    void clear_tape() {
        std::lock_guard<std::recursive_mutex> lk(g_mtx);
        g_tape.clear();
        g_events.clear();
        g_positions.clear();
        g_vels.clear();
        g_flags.clear();
        g_skip_ranges.clear();
        g_skip_dirty = true;
        g_head = 0;
        g_speed_accum = 0.f;
        g_mode = Mode::Idle;
    }

    void trim_to_head() {
        std::lock_guard<std::recursive_mutex> lk(g_mtx);
        if (g_head < g_tape.size()) {
            g_tape.resize(g_head);
            trim_parallel_arrays(g_head);
            g_skip_dirty = true;
        }
    }

    void overdub_from_head() {
        std::lock_guard<std::recursive_mutex> lk(g_mtx);
        if (g_tape.empty()) return;
        g_tape.resize(g_head);
        trim_parallel_arrays(g_head);
        g_skip_dirty = true;
        g_speed_accum = 0.f;
        g_mode = Mode::Recording;
    }

    // ─── per-tick hook ────────────────────────────────────────────────
    static Frame from_cmd(const CUserCmd* c) {
        return { c->viewangles,
                 c->forwardmove, c->sidemove, c->upmove,
                 c->buttons, c->impulse };
    }
    static void to_cmd(const Frame& f, CUserCmd* c) {
        c->viewangles  = f.viewangles;
        c->forwardmove = f.forwardmove;
        c->sidemove    = f.sidemove;
        c->upmove      = f.upmove;
        c->buttons     = f.buttons;
        c->impulse     = f.impulse;
    }

    static void playback_tick_body(CUserCmd* cmd, int steps);

    void on_createmove(CUserCmd* cmd) {
        if (!cmd) return;
        std::lock_guard<std::recursive_mutex> lk(g_mtx);

        // Dedupe so if both hooks (CHLClient::CreateMove and
        // IClientMode::CreateMove) fire on the same tick, we only process the
        // first one — the inner hook runs earlier in the pipeline so it wins.
        static int last_cn = -1;
        if (cmd->command_number == last_cn) return;
        last_cn = cmd->command_number;

        // Strafe optimizer — applied BEFORE the recorder captures inputs, so
        // any tape recorded with the optimizer on has the optimized moves
        // baked into it. Skipped during Playing so a recorded tape's inputs
        // drive the cmd untouched.
        //
        // Air-accel math: per-tick gain is maximized when wishdir is EXACTLY
        // perpendicular to velocity — addspeed stays at the full 30-unit air
        // cap AND the added velocity is orthogonal, so none of it fights the
        // current heading. (Holds whenever airaccelerate*maxspeed/tickrate
        // >= 30, i.e. every Source-engine config from vanilla 10 to KSF 150.) The old
        // version only flipped sidemove's sign and ZEROED it inside a ±2°
        // dead zone — the exact window where the gain lives. We now rebuild
        // both forwardmove and sidemove so the wish vector sits at
        // vel_yaw ± 90°, side chosen toward the view so the mouse still
        // steers the carve. Overriding W is harmless here: at >300 ups an
        // airborne forward wish can't pass the addspeed>0 check anyway.
        g_opt_active = false;
        g_opt_side   = 0;
        if (menu::g_strafe_optimizer &&
            g_mode != Mode::Playing &&
            g_mode != Mode::Seeking &&
            g_mode != Mode::Settling) {
            hooks::Vec3 vel{};
            bool got_vel = hooks::get_local_player_vel(vel);
            int  pflags  = 0;
            bool grounded = hooks::get_local_player_flags(pflags) &&
                            (pflags & FL_ONGROUND);
            bool braking  = cmd->forwardmove < -100.f; // user holds S = wants to slow

            // Free-fall gate — the perpendicular-wish rule is a FREE-AIR
            // result and fights surf ramps, so only engage in genuine
            // gravity free-fall (the air gaps between ramps, bhop hops) and
            // never while a ramp is supporting you. On a ramp the surface
            // clips your velocity, so vertical speed drops far slower than
            // pure gravity; in free air it drops by the full g*dt each tick.
            // We self-calibrate the free-fall delta — nothing makes you fall
            // FASTER than gravity, so the most-negative dvz we ever see IS
            // pure free-fall — which makes this independent of tickrate and
            // sv_gravity. A ramp's gentler drop then fails the 75% test.
            static float s_ff_dvz  = -12.f;
            static float s_prev_vz = 0.f;
            static bool  s_was_air = false;
            static int   s_ff_run  = 0;
            bool freefall = false;
            if (got_vel) {
                if (s_was_air && !grounded) {
                    float dvz = vel.z - s_prev_vz;
                    s_ff_dvz *= 0.999f;                     // slowly forget stale extremes
                    if (dvz < s_ff_dvz) s_ff_dvz = dvz;     // learn true free-fall
                    if (s_ff_dvz > -7.f)  s_ff_dvz = -7.f;  // floor: ramps never qualify
                    if (s_ff_dvz < -20.f) s_ff_dvz = -20.f;
                    // Within ±10% of learned free-fall AND sustained 3
                    // ticks. The old one-sided 75% test let steep ramps
                    // (~60-70°, which bleed vz at 75-90% of gravity) fire
                    // the optimizer mid-ride. Near-vertical faces (>72°)
                    // are indistinguishable from free-fall by dvz alone —
                    // toggle the optimizer off on maps built from those.
                    bool ff_tick = dvz <= s_ff_dvz * 0.90f &&
                                   dvz >= s_ff_dvz * 1.10f;
                    s_ff_run = ff_tick ? s_ff_run + 1 : 0;
                    freefall = s_ff_run >= 3;
                } else {
                    s_ff_run = 0;
                }
                s_prev_vz = vel.z;
                s_was_air = !grounded;
            }

            if (got_vel && !grounded && !braking && freefall) {
                float speed2 = vel.x * vel.x + vel.y * vel.y;
                if (speed2 > 300.f * 300.f) {
                    constexpr float RAD2DEG = 57.29577951308232f;
                    constexpr float DEG2RAD = 0.017453292519943295f;
                    float vel_yaw = std::atan2(vel.y, vel.x) * RAD2DEG;
                    float delta = cmd->viewangles.yaw - vel_yaw;
                    while (delta >  180.f) delta -= 360.f;
                    while (delta < -180.f) delta += 360.f;
                    // Carve toward the view. Hysteresis so jitter around 0°
                    // doesn't flip the carve side every tick.
                    static int s_side = 1;
                    if      (delta >  0.5f) s_side = +1;
                    else if (delta < -0.5f) s_side = -1;
                    // Wish at vel_yaw + side*90, expressed view-relative:
                    // wishvel = fwd*f + right*s  ->  f = cos(phi), s = -sin(phi)
                    float phi = (vel_yaw + s_side * 90.f - cmd->viewangles.yaw) * DEG2RAD;
                    cmd->forwardmove =  450.f * std::cos(phi);
                    cmd->sidemove    = -450.f * std::sin(phi);
                    // Matching IN_ bits — some servers ignore analog moves
                    // whose button bit isn't set.
                    constexpr int IN_FORWARD   = 1 << 3;
                    constexpr int IN_BACK      = 1 << 4;
                    constexpr int IN_MOVELEFT  = 1 << 9;
                    constexpr int IN_MOVERIGHT = 1 << 10;
                    cmd->buttons &= ~(IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT);
                    if (cmd->forwardmove >  25.f) cmd->buttons |= IN_FORWARD;
                    if (cmd->forwardmove < -25.f) cmd->buttons |= IN_BACK;
                    if (cmd->sidemove    >  25.f) cmd->buttons |= IN_MOVERIGHT;
                    if (cmd->sidemove    < -25.f) cmd->buttons |= IN_MOVELEFT;
                    g_opt_active = true;
                    g_opt_side   = s_side;
                }
            }
            if (got_vel)
                strafe_telemetry_push(std::sqrt(vel.x*vel.x + vel.y*vel.y),
                                      g_opt_active);
        }

        if (g_mode == Mode::Recording) {
            Frame f = from_cmd(cmd);
            g_tape.push_back(f);
            // Keep the physics-state arrays parallel to g_tape — one entry
            // per recorded tick. Position drives the matcher's distance
            // check; real velocity + m_fFlags give it the hidden state that
            // position deltas can't see. On a read failure keep the last
            // known good value so we don't corrupt the arrays with zero
            // sentinels the way an earlier bug did.
            hooks::Vec3 pos{};
            if (!hooks::get_local_player_pos(pos)) {
                pos = g_positions.empty() ? hooks::Vec3{} : g_positions.back();
            }
            g_positions.push_back(pos);

            hooks::Vec3 cur_vel{};
            bool got_vel = hooks::get_local_player_vel(cur_vel);
            if (!got_vel) {
                cur_vel = g_vels.empty() ? hooks::Vec3{} : g_vels.back();
            }
            g_vels.push_back(cur_vel);

            int cur_flags = 0;
            bool got_flags = hooks::get_local_player_flags(cur_flags);
            if (!got_flags) {
                cur_flags = g_flags.empty() ? 0 : g_flags.back();
            }
            g_flags.push_back(cur_flags);

            // Live stitch predictor — combined pos+vel distance from the
            // last E-press target. Ignored until first commit is made.
            if (g_commits_made > 0) {
                float pdx = pos.x - g_last_commit_pos_vec.x;
                float pdy = pos.y - g_last_commit_pos_vec.y;
                float pdz = pos.z - g_last_commit_pos_vec.z;
                float pos_d = std::sqrt(pdx*pdx + pdy*pdy + pdz*pdz);
                float vel_d = g_last_commit_vel_ups; // fallback if read failed
                if (got_vel) {
                    float vdx = cur_vel.x - g_last_commit_vel_vec.x;
                    float vdy = cur_vel.y - g_last_commit_vel_vec.y;
                    float vdz = cur_vel.z - g_last_commit_vel_vec.z;
                    vel_d = std::sqrt(vdx*vdx + vdy*vdy + vdz*vdz);
                }
                // Weight velocity 5× — that's what dominates physics
                // divergence at the stitch. Same weight the auto-detector
                // uses so the "safe" region matches what actually stitches.
                g_current_stitch_dist = pos_d + vel_d * 5.f;
                // A frame whose ground/duck/water bits differ from the
                // commit's is not a stitch candidate no matter how close
                // pos+vel get — don't let it drag min_dist down into a
                // false MATCHED verdict on the overlay.
                bool flags_ok = !(got_flags && g_last_commit_flags_known) ||
                                (((cur_flags ^ g_last_commit_flags) & PHYS_FLAGS_MASK) == 0);
                if (flags_ok && g_current_stitch_dist < g_min_stitch_dist)
                    g_min_stitch_dist = g_current_stitch_dist;
            }
            g_head = g_tape.size();
            return;
        }

        if (g_mode == Mode::Seeking) {
            hooks::Vec3 cur;
            if (!hooks::get_local_player_pos(cur)) {
                g_mode = Mode::Playing;
                return;
            }
            float dx = g_start_pos.x - cur.x;
            float dy = g_start_pos.y - cur.y;
            float dist = std::sqrt(dx * dx + dy * dy);
            float yaw = std::atan2(dy, dx) * 57.29577951308232f;

            static int seek_log = 8;
            if (seek_log > 0) {
                --seek_log;
                // Log via a fwrite is easier than pulling logging into recorder.
                FILE* f = nullptr; fopen_s(&f, "C:\\Users\\Talan\\momentum-menu\\momentum_menu.log", "a");
                if (f) {
                    fprintf(f, "SEEK cur=(%.1f,%.1f,%.1f) start=(%.1f,%.1f,%.1f) dist=%.1f yaw=%.1f\n",
                            cur.x, cur.y, cur.z, g_start_pos.x, g_start_pos.y, g_start_pos.z,
                            dist, yaw);
                    fclose(f);
                }
            }

            // Stuck detection — distance isn't dropping, force settle.
            float progress = g_last_dist - dist; // positive = getting closer
            g_last_dist = dist;
            if (progress < SEEK_STUCK_DELTA) ++g_stuck_ticks;
            else                             g_stuck_ticks = 0;

            if (dist <= SEEK_ARRIVE_RADIUS || g_stuck_ticks >= SEEK_STUCK_TICKS) {
                g_mode = Mode::Settling;
                g_last_pos = cur;
                g_stable_ticks = 0;
                g_settling_ticks = 0;
                g_stuck_ticks = 0;
                return;
            }

            float speed;
            if (menu::g_variable_walk) {
                speed = dist * 2.0f - 10.0f;
                if (speed <   5.f) speed =   5.f;
                if (speed > 450.f) speed = 450.f;
            } else {
                speed = 450.f;
            }

            cmd->viewangles.pitch = 0.f;
            cmd->viewangles.yaw   = yaw;
            cmd->viewangles.roll  = 0.f;
            cmd->forwardmove = speed;
            cmd->sidemove    = 0.f;
            cmd->upmove      = 0.f;
            cmd->buttons     = 0;

            hooks::QAngle3 va{ 0.f, yaw, 0.f };
            hooks::set_view_angles(va);
            return;
        }

        // Settling: hold zero input and wait for BOTH (a) a minimum total
        // duration in Settle, AND (b) the player's position to stop changing.
        // A single failed position read doesn't switch modes — we just keep
        // holding neutral inputs.
        if (g_mode == Mode::Settling) {
            ++g_settling_ticks;

            hooks::Vec3 cur;
            if (hooks::get_local_player_pos(cur)) {
                float ddx = cur.x - g_last_pos.x;
                float ddy = cur.y - g_last_pos.y;
                float ddz = cur.z - g_last_pos.z;
                float step = std::sqrt(ddx*ddx + ddy*ddy + ddz*ddz);
                g_last_pos = cur;
                if (step < SETTLE_STABLE_DELTA) ++g_stable_ticks;
                else                            g_stable_ticks = 0;
            }

            // hold neutral inputs so friction kills any momentum
            cmd->forwardmove = 0.f;
            cmd->sidemove    = 0.f;
            cmd->upmove      = 0.f;
            cmd->buttons     = 0;

            if (g_settling_ticks >= SETTLE_MIN_TOTAL_TICKS &&
                g_stable_ticks   >= SETTLE_STABLE_TICKS) {
                if (!g_tape.empty()) {
                    const Frame& f0 = g_tape[0];
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "setang %.4f %.4f %.4f",
                                  f0.viewangles.pitch, f0.viewangles.yaw, f0.viewangles.roll);
                    hooks::execute_client_cmd(buf);
                }
                g_mode = Mode::Playing;
            }
            return;
        }

        if (g_mode == Mode::Playing) {
            if (g_tape.empty()) { g_mode = Mode::Idle; return; }

            // One-shot per-play breadcrumb the first tick after PLAY starts
            // driving frames — pins down whether a crash happens before we
            // even touch cmd, or after. Reset each play_from_start().
            if (!g_first_play_tick_logged) {
                hooks::log("[Recorder] first playback tick  head=%zu speed=%.2f",
                           g_head, g_speed);
                g_first_play_tick_logged = true;
            }

            // Refresh skip windows if the slider changed since last rebuild.
            // Cheap — g_events is short. Runs at most once per playback tick.
            maybe_rebuild_skips();

            g_speed_accum += g_speed;
            int steps = static_cast<int>(g_speed_accum);
            g_speed_accum -= static_cast<float>(steps);
            if (steps < 1) return;

            // Playback body is called through a helper so we can wrap the
            // whole thing in structured exception handling — if any of the
            // cmd writes, set_view_angles, or vector accesses fault, we log
            // where we were and drop to Idle instead of dying silently.
            [&]() {
                __try {
                    playback_tick_body(cmd, steps);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    hooks::log("[Recorder] PLAYBACK EXCEPTION head=%zu tape=%zu "
                               "skips=%zu — dropping to Idle",
                               g_head, g_tape.size(), g_skip_ranges.size());
                    g_mode = Mode::Idle;
                }
            }();
            return;
        }

        // The rest of the on_createmove function (Seeking, Settling) sits
        // above the Playing block — nothing to do here for Idle.
    }

    // Playback body extracted so we can SEH-wrap it. C++ objects with
    // destructors can't sit in the same function as __try/__except, so this
    // stays in its own function.
    static void playback_tick_body(CUserCmd* cmd, int steps) {
        for (int i = 0; i < steps; ++i) {
            // Skip-window jump. Re-scan after every jump — pair-preserving
            // merge keeps touching windows separate, so a jump can land
            // exactly on the start of the next window and must hop again,
            // never writing a frame from inside a skip.
            bool jumped;
            do {
                jumped = false;
                for (const auto& r : g_skip_ranges) {
                    if (g_head >= r.start && g_head < r.end) {
                        g_head = r.end;
                        // Cut audit: distance from the LIVE player to the
                        // tape around the landing frame. end≈0 means the
                        // replay arrived on-state and the splice is sound;
                        // a large value means the drift happened BEFORE
                        // this cut (replay error), not at it. end-1/end+1
                        // expose any one-tick phase misalignment.
                        {
                            hooks::Vec3 live{};
                            if (hooks::get_local_player_pos(live)) {
                                auto d_at = [&](size_t k) -> float {
                                    if (k >= g_positions.size()) return -1.f;
                                    float ax = live.x - g_positions[k].x;
                                    float ay = live.y - g_positions[k].y;
                                    float az = live.z - g_positions[k].z;
                                    return std::sqrt(ax*ax + ay*ay + az*az);
                                };
                                float vd = -1.f;
                                hooks::Vec3 lv{};
                                if (hooks::get_local_player_vel(lv) && g_head < g_vels.size()) {
                                    float bx = lv.x - g_vels[g_head].x;
                                    float by = lv.y - g_vels[g_head].y;
                                    float bz = lv.z - g_vels[g_head].z;
                                    vd = std::sqrt(bx*bx + by*by + bz*bz);
                                }
                                hooks::log("[Playback] cut %zu->%zu  live pos_d(end-1=%.1f, end=%.1f, end+1=%.1f)  vel_d=%.0f ups",
                                           r.start, g_head,
                                           g_head > 0 ? d_at(g_head - 1) : -1.f,
                                           d_at(g_head), d_at(g_head + 1), vd);
                            }
                        }
                        jumped = true;
                        break;
                    }
                }
            } while (jumped);

            if (g_head >= g_tape.size()) {
                if (g_loop) g_head = 0;
                else        { g_mode = Mode::Idle; return; }
            }

            if (i == steps - 1) {
                Frame f = g_tape[g_head];

                constexpr int IN_FORWARD   = 1 << 3;
                constexpr int IN_BACK      = 1 << 4;
                constexpr int IN_MOVELEFT  = 1 << 9;
                constexpr int IN_MOVERIGHT = 1 << 10;
                auto swap_bits = [&](int a, int b) {
                    bool ha = (f.buttons & a) != 0;
                    bool hb = (f.buttons & b) != 0;
                    f.buttons &= ~(a | b);
                    if (ha) f.buttons |= b;
                    if (hb) f.buttons |= a;
                };

                if (menu::g_reverse_playback) {
                    f.viewangles.yaw += 180.f;
                    if (f.viewangles.yaw >  180.f) f.viewangles.yaw -= 360.f;
                    if (f.viewangles.yaw < -180.f) f.viewangles.yaw += 360.f;
                    f.forwardmove = -f.forwardmove;
                    f.sidemove    = -f.sidemove;
                    swap_bits(IN_FORWARD,  IN_BACK);
                    swap_bits(IN_MOVELEFT, IN_MOVERIGHT);
                    f.viewangles.pitch += static_cast<float>(menu::g_reverse_pitch_offset);
                    if (f.viewangles.pitch >  89.f) f.viewangles.pitch =  89.f;
                    if (f.viewangles.pitch < -89.f) f.viewangles.pitch = -89.f;
                }
                if (menu::g_sideways_playback) {
                    // Same trick as backwards: rotate the view 90° CW and
                    // counter-rotate the move vector so the world-space
                    // wishdir — and therefore the trajectory — is IDENTICAL
                    // to the recording. With view at yaw-90, world wishdir
                    // is preserved by (forward', side') = (side, -forward).
                    f.viewangles.yaw -= 90.f;
                    if (f.viewangles.yaw >  180.f) f.viewangles.yaw -= 360.f;
                    if (f.viewangles.yaw < -180.f) f.viewangles.yaw += 360.f;
                    float of = f.forwardmove, os = f.sidemove;
                    f.forwardmove =  os;
                    f.sidemove    = -of;
                    // Key-cross rotates with the analog: W->A, A->S, S->D,
                    // D->W (some servers ignore analog moves whose matching
                    // IN_ bit isn't set).
                    bool w = (f.buttons & IN_FORWARD)   != 0;
                    bool a = (f.buttons & IN_MOVELEFT)  != 0;
                    bool s = (f.buttons & IN_BACK)      != 0;
                    bool d = (f.buttons & IN_MOVERIGHT) != 0;
                    f.buttons &= ~(IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT);
                    if (w) f.buttons |= IN_MOVELEFT;
                    if (a) f.buttons |= IN_BACK;
                    if (s) f.buttons |= IN_MOVERIGHT;
                    if (d) f.buttons |= IN_FORWARD;
                    f.viewangles.pitch += static_cast<float>(menu::g_sideways_pitch_offset);
                    if (f.viewangles.pitch >  89.f) f.viewangles.pitch =  89.f;
                    if (f.viewangles.pitch < -89.f) f.viewangles.pitch = -89.f;
                }
                // Drift telemetry — measured every tick (independent of the
                // correction toggle). Feeds the on-screen overlay so you
                // can see WHICH segment is diverging in real time.
                if (g_head < g_positions.size()) {
                    hooks::Vec3 actual_pos{};
                    if (hooks::get_local_player_pos(actual_pos)) {
                        const auto& recorded = g_positions[g_head];
                        float ddx = recorded.x - actual_pos.x;
                        float ddy = recorded.y - actual_pos.y;
                        float ddz = recorded.z - actual_pos.z;
                        g_pb_drift_units = std::sqrt(ddx*ddx + ddy*ddy + ddz*ddz);

                        // Which segment are we in? Walk g_events and count
                        // Saveloc entries whose frame_index <= head.
                        int seg = 0;
                        for (const auto& ev : g_events) {
                            if (ev.kind == EventType::Saveloc && ev.frame_index <= g_head)
                                ++seg;
                            else if (ev.frame_index > g_head) break;
                        }
                        g_pb_current_segment = seg;

                        // If drift crosses fail threshold and we haven't
                        // already flagged this segment, mark it broken.
                        if (g_pb_drift_units > 200.f && g_pb_broken_segment != seg) {
                            g_pb_broken_segment = seg;
                            hooks::log("[Playback] segment %d BROKE at head=%zu  drift=%.0f units",
                                       seg, g_head, g_pb_drift_units);
                        }

                        // 1 Hz drift breadcrumb — the growth curve separates
                        // seed error (starts high), gradual replay divergence
                        // (smooth ramp), and cut error (step at a boundary).
                        static int s_crumb = 0;
                        if (++s_crumb >= 66) {
                            s_crumb = 0;
                            hooks::log("[Playback] head=%zu seg=%d drift=%.1f",
                                       g_head, seg, g_pb_drift_units);
                        }
                    }
                }

                to_cmd(f, cmd);
                hooks::QAngle3 va{ f.viewangles.pitch, f.viewangles.yaw, f.viewangles.roll };
                hooks::set_view_angles(va);
            }

            ++g_head;
        }
    }

    // ─── file i/o ─────────────────────────────────────────────────────
    static constexpr uint32_t MAGIC   = 0x5041544D; // 'MTAP'
    // v3 adds a per-frame position array after the events (needed for the
    // auto delay-comp analysis). v4 adds per-frame velocity (real
    // m_vecVelocity, ups) and m_fFlags arrays — the stitch matcher needs
    // true physics state, not just position deltas. Older tapes still
    // load; the matcher falls back to finite-difference velocity and
    // skips the hidden-state check for them.
    static constexpr uint32_t VERSION = 4;

    bool save(const char* path) {
        std::lock_guard<std::recursive_mutex> lk(g_mtx);
        std::ofstream f(path, std::ios::binary);
        if (!f) return false;

        uint32_t n = static_cast<uint32_t>(g_tape.size());
        uint32_t m = static_cast<uint32_t>(g_events.size());
        uint32_t p = static_cast<uint32_t>(g_positions.size());
        f.write(reinterpret_cast<const char*>(&MAGIC),   sizeof(MAGIC));
        f.write(reinterpret_cast<const char*>(&VERSION), sizeof(VERSION));
        f.write(reinterpret_cast<const char*>(&n),       sizeof(n));
        if (n) f.write(reinterpret_cast<const char*>(g_tape.data()), n * sizeof(Frame));
        f.write(reinterpret_cast<const char*>(&m), sizeof(m));
        if (m) f.write(reinterpret_cast<const char*>(g_events.data()), m * sizeof(TapeEvent));
        f.write(reinterpret_cast<const char*>(&p), sizeof(p));
        if (p) f.write(reinterpret_cast<const char*>(g_positions.data()), p * sizeof(hooks::Vec3));
        uint32_t vc = static_cast<uint32_t>(g_vels.size());
        f.write(reinterpret_cast<const char*>(&vc), sizeof(vc));
        if (vc) f.write(reinterpret_cast<const char*>(g_vels.data()), vc * sizeof(hooks::Vec3));
        uint32_t fc = static_cast<uint32_t>(g_flags.size());
        f.write(reinterpret_cast<const char*>(&fc), sizeof(fc));
        if (fc) f.write(reinterpret_cast<const char*>(g_flags.data()), fc * sizeof(int));
        return f.good();
    }

    bool load(const char* path) {
        std::lock_guard<std::recursive_mutex> lk(g_mtx);
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;

        uint32_t magic = 0, ver = 0, n = 0;
        f.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        f.read(reinterpret_cast<char*>(&ver),   sizeof(ver));
        if (magic != MAGIC) return false;
        if (ver < 1 || ver > VERSION) return false;
        f.read(reinterpret_cast<char*>(&n), sizeof(n));

        std::vector<Frame> loaded(n);
        if (n) f.read(reinterpret_cast<char*>(loaded.data()), n * sizeof(Frame));
        if (!f.good() && !f.eof()) return false;

        std::vector<TapeEvent> ev_loaded;
        if (ver >= 2) {
            uint32_t m = 0;
            f.read(reinterpret_cast<char*>(&m), sizeof(m));
            ev_loaded.resize(m);
            if (m) f.read(reinterpret_cast<char*>(ev_loaded.data()), m * sizeof(TapeEvent));
        }

        std::vector<hooks::Vec3> pos_loaded;
        if (ver >= 3) {
            uint32_t p = 0;
            f.read(reinterpret_cast<char*>(&p), sizeof(p));
            pos_loaded.resize(p);
            if (p) f.read(reinterpret_cast<char*>(pos_loaded.data()), p * sizeof(hooks::Vec3));
        }

        std::vector<hooks::Vec3> vel_loaded;
        std::vector<int>         flag_loaded;
        if (ver >= 4) {
            uint32_t vc = 0;
            f.read(reinterpret_cast<char*>(&vc), sizeof(vc));
            vel_loaded.resize(vc);
            if (vc) f.read(reinterpret_cast<char*>(vel_loaded.data()), vc * sizeof(hooks::Vec3));
            uint32_t fc = 0;
            f.read(reinterpret_cast<char*>(&fc), sizeof(fc));
            flag_loaded.resize(fc);
            if (fc) f.read(reinterpret_cast<char*>(flag_loaded.data()), fc * sizeof(int));
        }

        g_tape      = std::move(loaded);
        g_events    = std::move(ev_loaded);
        g_positions = std::move(pos_loaded);
        g_vels      = std::move(vel_loaded);
        g_flags     = std::move(flag_loaded);
        g_skip_dirty = true;
        g_head = 0;
        g_speed_accum = 0.f;
        g_mode = Mode::Idle;
        hooks::log("[Recorder] LOAD  ver=%u  frames=%zu  events=%zu  positions=%zu  vels=%zu  flags=%zu",
                   ver, g_tape.size(), g_events.size(), g_positions.size(),
                   g_vels.size(), g_flags.size());
        return true;
    }
}
