#pragma once

// Source SDK types we actually touch — no engine class hierarchy here.
struct QAngle { float pitch, yaw, roll; };

// Momentum (Strata) layout — decoded byte-by-byte from live LayoutAudit
// dumps 2026-07-23 (known keys held while recording): momentum inserted a
// 12-byte vector after viewangles (aimdirection, CS:GO-style, always zero
// for us), which shifts everything from forwardmove onward +12 vs classic
// Source 2013. Offsets verified: cn@8, tick@12, angles@16, fwd@40 (+450
// on W / -450 on S), side@44 (+450 on D), buttons@52 (0x8 W, 0x10 S,
// 0x400 D). Full engine-side stride is 120 bytes; fields past impulse are
// untouched by this tool and left unmodeled.
class CUserCmd {
public:
    virtual ~CUserCmd() {}
    int           command_number;      // +8
    int           tick_count;          // +12
    QAngle        viewangles;          // +16
    float         aimdirection[3];     // +28 — leave alone, engine-owned
    float         forwardmove;         // +40
    float         sidemove;            // +44
    float         upmove;              // +48
    int           buttons;             // +52
    unsigned char impulse;             // +56
};

enum IN_Buttons : int {
    IN_ATTACK    = 1 << 0,
    IN_JUMP      = 1 << 1,
    IN_DUCK      = 1 << 2,
    IN_FORWARD   = 1 << 3,
    IN_BACK      = 1 << 4,
    IN_USE       = 1 << 5,
    IN_MOVELEFT  = 1 << 9,
    IN_MOVERIGHT = 1 << 10,
    IN_ATTACK2   = 1 << 11,
};

// Windows x64 has a single calling convention, so passing `this` as the
// first arg to a raw function pointer matches the ABI for member calls.
template <typename R, typename... Args>
inline R call_vfunc(void* instance, int idx, Args... args) {
    using Fn = R(*)(void*, Args...);
    Fn fn = reinterpret_cast<Fn>((*reinterpret_cast<void***>(instance))[idx]);
    return fn(instance, args...);
}
