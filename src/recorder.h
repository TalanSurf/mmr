#pragma once
#include <cstddef>
#include <cstdint>
#include "sdk.h"

namespace recorder {

    enum class Mode { Idle, Recording, Playing, Seeking, Settling };

    struct Frame {
        QAngle        viewangles;
        float         forwardmove;
        float         sidemove;
        float         upmove;
        int           buttons;
        unsigned char impulse;
    };

    // SaveLoc-mode event stream — commit/rollback metadata used to compute
    // playback skip windows. Saveloc = E-press (commit). Teleport/Prevloc/
    // Nextloc = rollback flavors, modeled after Momentum's savestate system
    // (matching KSF semantics 1:1): a SELECTED-loc pointer, E appends +
    // selects newest (mom_savestate_create), Q teleports to the SELECTED
    // loc without moving the pointer (+mom_savestate_load), 1 moves the
    // pointer back + teleports (mom_savestate_prev), 3 moves it forward +
    // teleports (mom_savestate_next). The `cmd` field logs the command
    // fired for debugging; replay is driven by kind, not cmd.
    enum class EventType : uint8_t { Saveloc = 1, Teleport = 2, Prevloc = 3, Nextloc = 4 };
    struct TapeEvent {
        size_t    frame_index;
        EventType kind;
        char      cmd[64];
    };

    // Called by hooks::execute_client_cmd interceptor when the user issues a
    // chat command we care about while Recording.
    void record_event(EventType kind, const char* cmd);

    // SaveLoc-style tape editing:
    //   commit()   -> mark the current tape length as a checkpoint (E press
    //                 → /saveloc). Later frames are considered "in progress"
    //                 until either commit() or rollback() runs.
    //   rollback() -> truncate the tape back to the most-recent checkpoint
    //                 (Q press → /teleport). Throws away every frame you
    //                 recorded since the last E, so a stitched run only
    //                 contains the successful segments.
    void commit();
    void rollback();       // Q — targets the SELECTED commit (+mom_savestate_load)
    void rollback_prev();  // selects one segment earlier (works Recording or Idle)
    void rollback_next();  // selects one segment later (works Recording or Idle)

    // Selected segment position (1-based) and total, for the on-screen readout.
    int selected_segment();
    int segment_count();

    // Session counter of every /saveloc we fire (via RECORD, E hotkey, or
    // user chat typed while injected). record_slot() returns the counter's
    // value at the moment the last RECORD was pressed — that's the plugin
    // slot number PLAY needs to /teleport back to.
    void     note_saveloc_fired();
    uint32_t record_slot();
    void     reset_saveloc_counter();

    // Player velocity magnitude at the last E-press.
    float    last_commit_velocity_ups();

    // Live recording-time stitch predictor. After each E-press we save the
    // player's position AND velocity as the "target state" the retry must
    // hit again for a valid stitch. Then each tick we compute the current
    // distance to that target. The MIN distance seen since the last E is
    // the best the retry has done — if that stays high, no valid stitch
    // exists in this segment and the run will fail on playback.
    //
    //   commits_made()       — how many E-presses so far (0 = "no E yet")
    //   current_stitch_dist  — live combined pos+vel distance to last commit state
    //   min_stitch_dist      — smallest combined distance seen since last E-press
    //   last_commit_in_air   — z-velocity at last E-press was non-trivial;
    //                          in-air commits carry hidden physics state risk
    //                          (fl_onground differs) that pure pos+vel can't
    //                          detect. flag it for the user.
    int   commits_made();
    float current_stitch_dist();
    float min_stitch_dist_since_commit();
    bool  last_commit_in_air();

    // Playback drift telemetry — measured live and updated each tick while
    // the tape is playing. `current_segment` = how many E-presses we've
    // passed on the tape (i.e., the segment we're currently REPLAYING).
    // `drift_units` = distance between actual player position and the
    // tape's recorded position for the current head. `broken_segment` is
    // the last segment index where drift crossed the "will fail" threshold
    // (~200 units), or -1 if the run has held together so far.
    int   playback_current_segment();
    float playback_drift_units();
    int   playback_broken_segment();

    // Stitch-quality report — recomputed whenever the skip windows are
    // rebuilt. A cut is RISKY when its match error (pos units + 5x velocity
    // mismatch in ups, same scale as the live predictor's cur/best) exceeds
    // the menu threshold, or when it fell back to the fixed delay slider
    // (unverified). tape_has_phys_state() = the tape carries per-tick
    // velocity + m_fFlags (v4 recordings); without them the matcher can't
    // see hidden state and cuts are inherently less trustworthy.
    int   stitch_cut_count();
    int   stitch_risky_count();
    float stitch_worst_err();
    bool  tape_has_phys_state();

    // Strafe-optimizer telemetry for the on-screen gauge. History is a ring
    // of per-tick horizontal speeds (~4s at 66 tick); strafe_history copies
    // them oldest-first and returns the count.
    bool  strafe_opt_active();   // steering this tick (airborne, fast enough)
    int   strafe_opt_side();     // +1 carving left, -1 carving right, 0 idle
    float strafe_speed();        // current horizontal speed, ups
    float strafe_gain_1s();      // ups gained over the trailing second
    int   strafe_sync_pct();     // % of the last second's active ticks that gained speed
    int   strafe_history(float* out, int max_n);

    // state
    Mode   mode();
    size_t tape_size();
    size_t play_head();
    void   set_play_head(size_t);
    bool   looping();
    void   set_looping(bool);
    float  play_speed();
    void   set_play_speed(float);

    // controls
    void start_record();        // wipes tape, records from frame 0
    void play_from_start();     // seeks to 0 and starts playback
    void resume_playback();     // continues playback from current head
    void pause();               // stops but keeps head — pair with resume_playback
    void stop();                // stops, resets head to 0
    void clear_tape();          // wipes everything back to idle

    // editing
    void trim_to_head();        // discard everything after the current head
    void overdub_from_head();   // truncate tail and start recording from head

    // persistence
    bool save(const char* path);
    bool load(const char* path);

    // called from the CreateMove hook on every tick
    void on_createmove(CUserCmd* cmd);
}
