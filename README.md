# Momentum Movement Recorder

An **offline** input recorder + saveloc-stitched playback tool for **Momentum Mod
Playtest** (Strata Source, x64). Built for making surf-showcase / TAS-style videos:
record a run in segments, roll back and retry the parts you flub, and it stitches the
good pieces into one clean playback — with the camera following your recorded view.

> Offline / content-creation use only. Run it on your own machine for showcase footage.
> Don't use it to submit to ranked leaderboards.

## Just want to use it?

1. **Download the repo** (green "Code" button → Download ZIP, or `git clone`).
2. Launch **Momentum Mod** and load into a map.
3. Double-click **`Inject Tool.bat`**.
   - If the game's running, it injects immediately.
   - If not, it waits and keeps trying — you can run it first, then start the game.
4. Press **`INSERT`** in-game to open the menu.

That's it. The tool is a single self-contained DLL (`momentum_menu.dll`) sitting right
next to the launcher — no Visual C++ redistributable, no dependencies to install.

**To update later:** double-click **`Update.bat`** — it pulls the newest version from
the repo. (Requires that you *cloned* it with `git`; if you downloaded the ZIP, just grab
a fresh ZIP instead.)

**Requirements:** 64-bit Windows, Momentum Mod Playtest.

**Heads-up on SmartScreen / antivirus:** this injects a DLL into the game (that's how
overlays/tools like this work), so Windows SmartScreen or your AV may warn about the
`.bat` or flag the injection. That's expected for this kind of tool — allow it if you
trust the source.

## Controls (defaults — all rebindable in the menu)

| Key | Action |
|-----|--------|
| `INSERT` | toggle menu |
| `MOUSE5` | start recording |
| `MOUSE4` | stop |
| `F5` | play from start |
| `F4` | pause |
| `E` | commit a segment (savestate + tape marker) |
| `Q` | rollback (teleport to current savestate) |
| `4` / `3` | select previous / next recorded segment |

Rebind anything to **any key or mouse button** (including left/right-click) in
**Settings → KEYBINDS**. Overlay size/position is under **Settings → ON-SCREEN OVERLAY**.

## Building it yourself

- **Toolchain:** Visual Studio 2022 (MSVC), CMake 3.20+.
- **One-shot rebuild + inject** (game must be running):
  ```
  powershell -File rebuild-inject.ps1
  ```
- Or configure/build manually:
  ```
  cmake -S . -B build -G "Visual Studio 17 2022" -A x64
  cmake --build build --config Release
  ```
The DLL links the CRT statically (`/MT`), so the output runs on any x64 Windows as-is.
`rebuild-inject.ps1` copies the fresh DLL next to the launcher automatically.

## Notes

- Recording, bit-perfect stitched playback, saveloc teleport, and camera-follow are all
  runtime-resolved against the shipped game binaries — most of it adapts across game
  updates on its own.
- The first-person **camera-follow** uses a couple of hardcoded player-struct offsets for
  this Momentum build. If a game update moves them, playback movement still works but the
  camera may stop following until the offsets are re-derived.
