#pragma once

namespace menu {
    extern bool g_open;
    extern bool g_teleport_to_start;  // use setpos on PLAY (needs sv_cheats 1)
    extern bool g_variable_walk;      // slow down on seek approach — off = always full speed
    extern bool g_auto_saveloc;       // fire mom_savestate_create on RECORD so the run's start is savable
    extern bool g_reverse_playback;   // play back facing 180° with inverted moves — world motion identical
    extern bool g_sideways_playback;  // play back facing 90° CW with rotated moves — world motion identical
    extern bool g_saveloc_mode;       // record + replay /saveloc + /teleport events for stitched runs

    // If > 0, RECORD skips the counter and locks record_slot to this exact
    // value — the # the surf plugin will assign your next /saveloc.
    extern int  g_saveloc_slot_override;

    // Delay-compensation for Q rollback. When Q fires, the server-side
    // /teleport takes N ticks to actually land (ping + processing). Used
    // only as the fallback skip window when auto-match finds no snap.
    extern int  g_saveloc_delay_frames;

    // When on, each commit/rollback pair gets its OWN measured skip window
    // by matching recorded position + velocity + physics flags around the
    // teleport snap-back. Falls back to the slider when no match is found.
    extern bool g_saveloc_auto_delay;

    // Auto-match requires FL_ONGROUND / FL_DUCKING / FL_INWATER equality
    // between the two frames of a stitch pair — pos+vel can't tell a
    // grounded frame from an airborne one at the same spot, but their
    // next-tick physics differ completely. Needs a v4 tape.
    extern bool g_match_require_flags;

    // Cuts whose match error (pos units + 5x vel mismatch in ups) exceeds
    // this are flagged RISKY in the stitch report + log so the user knows
    // which segment to re-record.
    extern float g_stitch_max_err;

    // Strafe optimizer — during Recording / Idle, overrides sidemove each
    // tick to steer wishdir toward the side that maximizes Source-engine
    // air-accel gain. Disabled during Playback so tapes replay unchanged.
    extern bool g_strafe_optimizer;
    extern bool g_rmb_overdub;        // right-click during playback = overdub from current frame
    extern bool g_compact_hud;        // shrink the status chip
    extern bool g_hud_top_left;       // move the status chip to the top-left
    extern bool g_commit_freeze;      // freeze input around each commit
    extern int  g_commit_freeze_ticks;// freeze-window length in ticks
    extern bool g_rewind_mode;        // Q discards failed attempt (continuous tape, no stitching)

    // Extra pitch offset (degrees) applied on top of the recorded pitch
    // when backwards playback is on. Negative = look down at the ground.
    extern int  g_reverse_pitch_offset;

    // Same, for sideways playback.
    extern int  g_sideways_pitch_offset;

    // ── rebindable hotkeys ────────────────────────────────────────────
    // Values are Windows virtual-key codes. VK_XBUTTON1/2 = Mouse4/Mouse5,
    // VK_MBUTTON = middle mouse. Defaults match the original hardcoded keys.
    enum BindAction : int {
        BindMenu = 0,   // toggle this menu          (INSERT)
        BindPlay,       // play from start           (F5)
        BindPause,      // pause playback            (F4)
        BindRecord,     // start recording           (Mouse5)
        BindStop,       // stop                      (Mouse4)
        BindCommit,     // commit segment            (E, recording only)
        BindRollback,   // rollback failed attempt   (Q, recording only)
        BindPrevloc,    // rollback to prev commit   (1, recording only)
        BindNextloc,    // rollback to next commit   (3, recording only)
        BindCount
    };
    int         bind_vk(int action);
    const char* bind_label(int action);    // action name for UI / help
    const char* bind_key_name(int action); // display name of the bound key

    // WndProc feeds every key/mouse press here FIRST. Returns true when the
    // press was consumed by an active "press a key…" capture in the binds
    // UI (so the key neither reaches the game nor triggers an action).
    bool bind_feed_key(int vk);

    // True while an ImGui text field wants the keyboard — action hotkeys
    // must not fire while the user is typing a filename in the menu.
    bool ui_wants_keyboard();

    // ── config persistence ────────────────────────────────────────────
    // Settings + keybinds live in %APPDATA%\momentum-menu\config.cfg. Loaded
    // automatically at inject (init_paths), autosaved a couple seconds
    // after any change, and saved explicitly before DLL unload.
    void config_load();
    void config_save();

    // sets up %APPDATA%/momentum-menu/tapes + loads the config
    void init_paths();
    void start_welcome();
    void draw();
}
