#pragma once

namespace hooks {
    bool install();
    void uninstall();

    // diagnostics — displayed in the menu so you can see hooks working
    int  endscene_calls();
    int  createmove_calls();
    int  cm_createmove_calls();
    bool ready();

    // multi-probe diagnostics: index 0..probe_count()-1
    int  probe_count();
    int  probe_slot(int idx);
    int  probe_hits(int idx);
    int  write_delta_calls();

    // Unload the DLL from memory so the file can be rebuilt without
    // restarting the game. Uninstalls all hooks first, then FreeLibrary's
    // ourselves on a background thread.
    void request_unload();

    // Engine helpers used by the recorder playback.
    struct QAngle3 { float pitch, yaw, roll; }; // ABI-matching QAngle
    struct Vec3    { float x, y, z; };

    void set_view_angles(const QAngle3& angles);
    // Re-stamp the last tape view angle onto the engine's cl.viewangles.
    // Call once per rendered frame during playback so the camera holds the
    // tape angle instead of the engine's per-frame mouse resample.
    void reassert_view_angle();
    void execute_client_cmd(const char* cmd);
    // Sends a command string to the SERVER (IVEngineClient::ServerCmd). The
    // mom_savestate_* commands are server-registered — the teleport logic
    // runs server-side — so they must go through this, not ClientCmd.
    void execute_server_cmd(const char* cmd);

    // printf-style trace to momentum_menu.log — safe to call from any TU.
    void log(const char* fmt, ...);

    // Returns false if the local player entity isn't reachable yet
    // (main menu, not connected, etc.).
    bool get_local_player_pos(Vec3& out);
    bool get_local_player_vel(Vec3& out);

    // m_fFlags netvar (FL_ONGROUND / FL_DUCKING / FL_INWATER bits). Pos+vel
    // alone can't tell a grounded frame from an airborne one at the same
    // spot, but their next-tick physics differ completely — the stitch
    // matcher needs these bits to reject that class of false match.
    bool get_local_player_flags(int& out);

    // Reconstruct the spectated player's cmd fields (forwardmove, sidemove,
    // viewangles) from their velocity + view netvars. Returns false if not
    // spectating anyone or netvars aren't resolvable.
    struct SpecInputs {
        float  forwardmove;
        float  sidemove;
        float  upmove;
        QAngle3 viewangles;
    };
    bool read_spectated_inputs(SpecInputs& out);
}
