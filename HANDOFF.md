# CSS Movement Recorder — Project Handoff

Offline Counter-Strike: Source **surf-showcase TAS tool** (input recorder + saveloc-stitched
playback). Client DLL injected into `cstrike_win64.exe` (x64 CS:S branch). Used strictly
offline / on Talan's own server for content creation — no anti-cheat evasion, and none should
ever be added.

**Status: WORKING.** Bit-perfect stitched playback verified 2026-07-19 — a 7,467-frame,
13-segment run played with drift 0.0 for ~2 minutes, forwards AND in backwards mode.

## File locations

| Path | Purpose |
|---|---|
| `C:\Users\Talan\css-menu\src\recorder.cpp` | Core: tape recording, auto-matcher, stitching, playback, strafe optimizer |
| `C:\Users\Talan\css-menu\src\recorder.h` | Recorder API, tape/event structs, telemetry getters |
| `C:\Users\Talan\css-menu\src\hooks.cpp` | Engine hooks (CreateMove, EndScene, WndProc), netvar walker, hotkey dispatch |
| `C:\Users\Talan\css-menu\src\hooks.h` | Hooks API: pos/vel/m_fFlags readers, execute_client_cmd, log() |
| `C:\Users\Talan\css-menu\src\menu.cpp` | ImGui menu (4 tabs), config load/save, keybinds, overlays + strafe gauge |
| `C:\Users\Talan\css-menu\src\menu.h` | Settings globals, bind API, config API |
| `C:\Users\Talan\css-menu\src\dllmain.cpp` | DLL entry |
| `C:\Users\Talan\css-menu\src\sdk.h` | CUserCmd / QAngle structs |
| `C:\Users\Talan\css-menu\CMakeLists.txt` | Build config (MSVC, x64) |
| `C:\Users\Talan\css-menu\rebuild-inject.ps1` | **THE command**: build + force-unload old DLL + inject fresh |
| `C:\Users\Talan\css-menu\inject.ps1`, `unload-only.ps1`, `Xenos64.exe` | Injection tooling |
| `C:\Users\Talan\css-menu\build\Release\css_menu.dll` | Build output |
| `C:\Users\Talan\css-menu\css_menu.log` | Diagnostic log — read this FIRST for any bug |
| `C:\Users\Talan\AppData\Roaming\css-menu\config.cfg` | Persisted settings + keybinds (autoload at inject, autosave ~2s after change) |
| `C:\Users\Talan\AppData\Roaming\css-menu\tapes\` | Saved `.mtape` recordings |
| `C:\Users\Talan\css-menu\vendor\imgui\` | ImGui (dependency — don't touch) |

Build & inject (game must be running; wipes the in-memory tape — save first if it matters):

    powershell -File C:\Users\Talan\css-menu\rebuild-inject.ps1

After injecting: confirm the game process survived and tail `css_menu.log`.

## How it works

**Recording** (`recorder.cpp on_createmove`): each tick captures the CUserCmd (viewangles,
moves, buttons) into `g_tape`, plus parallel arrays `g_positions`, `g_vels` (real
m_vecVelocity, ups), `g_flags` (m_fFlags). Tape file format **v4** (`MAGIC 'MTAP'`); older
versions still load but lack phys data, so matching degrades.

**Saveloc stitching**: hotkeys during recording — E = commit (`sm_saveloc` + Saveloc event),
Q = rollback (`sm_teleport` + Teleport event), 1/3 = prev/next loc (`sm_teleprev` /
`sm_telenext` + Prevloc/Nextloc events). Server plugin semantics are KSF-style and the tape
model mirrors them exactly: the plugin keeps a SELECTED-loc pointer — saveloc appends+selects
newest; sm_teleport goes to the SELECTED loc (pointer unchanged); prev/next move the pointer
and teleport. Q/1/3 also fire while Idle (no marker; disabled during playback — they're
normal game keys).

**Auto-matcher** (`auto_match_delay` + `rebuild_skip_ranges`): for each rollback event, find
skip window [j, i) meaning "play frames < j, resume at i" where state-before-i ≈
state-before-j. Match requires position (<5u), velocity (real, <3 u/tick), and
FL_ONGROUND/FL_DUCKING/FL_INWATER equality; airborne cuts get a score penalty. Guards, each
one earned by a real observed failure:
- **Landing floor**: search only from the teleport's actual landing (first frame whose step
  deviates from recorded velocity by >40u). Kills "shadow frames" — after a landing the
  player retraces the commit path bit-exactly, so Q double-taps otherwise match a clone one
  tick before the next rewind.
- **Freeze skip (TRIX support)**: after the landing, walk past any parked frames (<0.5u
  motion, 400-tick cap) — TRIX-style servers freeze you at the loc briefly; the pause is
  measured from the tape per rollback, so NO server-type setting exists or is needed. KSF =
  0-frame pause = no-op.
- **Rewind guard**: never splice onto a frame whose next recorded step is teleport-sized.
- **NO_TELEPORT**: if no landing exists after a rollback, the server flood-dropped the
  command — the event is fiction; produce NO cut at all (fallbacks suppressed).
- **LANDING_CUT fallback**: if no state match but the landing is known, cut
  [target + teleport-delay, freeze-resume) instead of the blind slider window.
- **Pair-preserving merge**: overlapping windows keep the LARGEST-END pair wholesale —
  min-start/max-end merging spliced mismatched anchors and broke runs by 500+ units.

**Playback** (F5): settles at the start (neutral input until still, then recorded setang),
then drives cmd from the tape each tick, jumping heads over skip windows (re-scanning after
each jump). Timer-safe: fires NO server commands during playback. Playback styles: backwards
(yaw+180, moves negated, W/S + A/D swapped) and sideways (yaw−90, (fwd,side)→(side,−fwd),
key cross rotated) — both preserve the world trajectory bit-exactly; each has a pitch-offset
slider.

**Diagnostics in the log** (the tool is self-diagnosing — always read these before theorizing):
- `PLAY start offset = X units` (warns >2; entry error no cut can remove)
- `stitch report: N cut(s), M risky, worst err E` (risky = err > menu threshold or
  unverified fallback; err scale = pos units + 5× vel mismatch in ups)
- `cut S->E  live pos_d(end-1, end, end+1) vel_d` per splice — healthy: end≈0, end+1≈one
  tick of travel (25–50u); end+1 in the hundreds = teleport inside the spliced tail
- 1 Hz `head/seg/drift` breadcrumbs — seed error starts high; replay divergence ramps
  smoothly; cut error steps at a boundary
- `segment N BROKE` at drift >200u

**Strafe optimizer**: while airborne >300 ups in *genuine free-fall only*, rebuilds
forward/side so wishdir is exactly perpendicular to velocity (max air-accel gain on every
CS:S config); carve side follows the view; S = manual brake. Free-fall gate self-calibrates
from dvz (±10% band, 3 consecutive ticks) so it never fights surf ramps — but ramps steeper
than ~72° are indistinguishable from free-fall; toggle it off on those maps. Top-center
gauge shows speed / gain / sync% / carve side / sparkline. Inactive during playback.

**Threading**: CS:S uses queued rendering — ImGui menu runs on the render thread,
CreateMove on the main thread. ALL mutating recorder entry points take `g_mtx`
(recursive std::mutex; recursive because commit → execute_client_cmd re-enters
record_event through the ClientCmd hook). This fixed a real ntdll heap-corruption crash.
Any new recorder-state mutation MUST take the lock.

**Menu/config/binds**: 4 tabs (Recorder / Settings / Files / Help). Config autoloads at
inject and autosaves on change. Keybinds rebindable (Settings tab), defaults
INSERT/F5/F4/Mouse5/Mouse4/E/Q/1/3, stored in config. ImGui text fields suppress hotkeys
while typing.

## Hard-earned facts — do not relearn these the painful way

1. **Server commands are `sm_teleprev` / `sm_telenext`** — confirmed by Talan on his
   server. A well-meaning rename to `sm_prevloc`/`sm_nextloc` broke them once already.
   Do not "fix" these names.
2. **Playback trajectory is decided ONLY by (server state, usercmd stream).** Client-side
   memory writes cannot fix drift. "Play failed attempts inline" is impossible timer-safe —
   post-Q frames were recorded after the server teleport, so replaying them without
   teleporting desyncs.
3. **Required workflow**: before F5, teleport to the run's FIRST saveloc (the tool saves it
   at RECORD start). Start offset 0.0 in the log = correct. The server's sm_teleport
   restores velocity, so teleport landings are exact-state cut points.
4. **Talan's local server lands teleports in ~3 ticks** (remote ~11) — timing assumptions
   must come from the tape (landing detection), never constants.
5. `rebuild-inject.ps1` force-unloads the DLL → **the in-memory tape is lost**. Save tapes
   worth keeping before rebuilding.
6. A **risky cut in the stitch report means re-record that segment** — the retry never
   precisely re-acquired the commit state; no algorithm can fix that after the fact.
7. Spectator recording was removed deliberately: other players' actual inputs are never
   networked to the client, so it can only ever be a lossy reconstruction.
8. This tool is offline-only, for showcase filming. Keep it that way — no evasion
   features, nothing aimed at playing on other people's servers.
