#include "menu.h"

#include <Windows.h>
#include <ShlObj.h>
#include <ShellAPI.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"
#include "recorder.h"
#include "hooks.h"

namespace menu {

    bool g_open = true;
    bool g_teleport_to_start   = false;
    bool g_variable_walk       = true;
    bool g_auto_saveloc        = true;
    bool g_reverse_playback    = false;
    bool g_saveloc_mode        = false;
    bool g_sideways_playback   = false;
    int  g_saveloc_slot_override = 0;
    int  g_saveloc_delay_frames  = 5;
    bool g_saveloc_auto_delay    = true;
    bool g_match_require_flags   = true;
    float g_stitch_max_err       = 100.f;
    bool g_strafe_optimizer      = false;
    int  g_reverse_pitch_offset  = -15;
    int  g_sideways_pitch_offset = 0;
    bool g_rmb_overdub           = false;  // right-click during playback = take over + re-record from here
    bool g_compact_hud           = false;  // shrink the REC/segment status chip
    bool g_hud_top_left          = false;  // move the status chip to the top-left corner

    // ─── keybinds ─────────────────────────────────────────────────────
    struct Bind { const char* label; int def_vk; int vk; };
    static Bind g_binds[BindCount] = {
        { "toggle menu",              VK_INSERT,   VK_INSERT   },
        { "play from start",          VK_F5,       VK_F5       },
        { "pause playback",           VK_F4,       VK_F4       },
        { "start recording",          VK_XBUTTON2, VK_XBUTTON2 },
        { "stop",                     VK_XBUTTON1, VK_XBUTTON1 },
        { "commit (create savestate)",   'E',         'E'         },
        { "rollback (load savestate)",   'Q',         'Q'         },
        { "prev savestate",              '1',         '1'         },
        { "next savestate",              '3',         '3'         },
    };
    static int g_capture_action = -1;

    int         bind_vk(int action)    { return (action >= 0 && action < BindCount) ? g_binds[action].vk : 0; }
    const char* bind_label(int action) { return (action >= 0 && action < BindCount) ? g_binds[action].label : "?"; }

    // Human name for a VK code. Ring of buffers so several calls can sit
    // in one printf without clobbering each other.
    static const char* vk_name(int vk) {
        static char bufs[4][64];
        static int  bi = 0;
        char* buf = bufs[bi = (bi + 1) & 3];

        switch (vk) {
        case VK_LBUTTON:  return "L-CLICK";
        case VK_RBUTTON:  return "R-CLICK";
        case VK_MBUTTON:  return "MOUSE3";
        case VK_XBUTTON1: return "MOUSE4";
        case VK_XBUTTON2: return "MOUSE5";
        }
        UINT sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
        // Extended keys need the extended bit or GetKeyNameText returns the
        // numpad variant (INSERT would read as "Num 0").
        switch (vk) {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
        case VK_PRIOR:  case VK_NEXT:   case VK_LEFT: case VK_RIGHT:
        case VK_UP:     case VK_DOWN:   case VK_DIVIDE:
            sc |= 0x100; break;
        }
        if (GetKeyNameTextA(static_cast<LONG>(sc << 16), buf, 64) > 0) return buf;
        std::snprintf(buf, 64, "VK 0x%02X", vk);
        return buf;
    }

    const char* bind_key_name(int action) {
        return vk_name(bind_vk(action));
    }

    bool bind_feed_key(int vk) {
        if (g_capture_action < 0) return false;
        if (vk == VK_ESCAPE) { g_capture_action = -1; return true; }
        g_binds[g_capture_action].vk = vk;
        g_capture_action = -1;
        return true;
    }

    bool ui_wants_keyboard() {
        return g_open && ImGui::GetCurrentContext() != nullptr &&
               ImGui::GetIO().WantCaptureKeyboard;
    }

    // ─── paths + config ───────────────────────────────────────────────
    static char        g_tapes_dir[MAX_PATH]   = "";
    static char        g_config_path[MAX_PATH] = "";
    static char        g_name_buf[128]         = "tape.mtape";
    static std::vector<std::string> g_tape_list;
    static std::string g_last_saved_cfg;

    static void config_serialize(std::string& out) {
        char line[96];
        auto kv = [&](const char* k, int v) {
            std::snprintf(line, sizeof(line), "%s=%d\n", k, v);
            out += line;
        };
        kv("saveloc_mode",    g_saveloc_mode);
        kv("auto_saveloc",    g_auto_saveloc);
        kv("auto_delay",      g_saveloc_auto_delay);
        kv("require_flags",   g_match_require_flags);
        kv("delay_frames",    g_saveloc_delay_frames);
        kv("stitch_max_err",  static_cast<int>(g_stitch_max_err));
        kv("slot_override",   g_saveloc_slot_override);
        kv("reverse",         g_reverse_playback);
        kv("sideways",        g_sideways_playback);
        kv("rev_pitch",       g_reverse_pitch_offset);
        kv("side_pitch",      g_sideways_pitch_offset);
        kv("strafe_opt",      g_strafe_optimizer);
        kv("teleport_start",  g_teleport_to_start);
        kv("variable_walk",   g_variable_walk);
        kv("rmb_overdub",     g_rmb_overdub);
        kv("compact_hud",     g_compact_hud);
        kv("hud_top_left",    g_hud_top_left);
        for (int i = 0; i < BindCount; ++i) {
            std::snprintf(line, sizeof(line), "bind_%d=%d\n", i, g_binds[i].vk);
            out += line;
        }
    }

    static void config_apply(const char* key, int v) {
        if      (!std::strcmp(key, "saveloc_mode"))   g_saveloc_mode          = v != 0;
        else if (!std::strcmp(key, "auto_saveloc"))   g_auto_saveloc          = v != 0;
        else if (!std::strcmp(key, "auto_delay"))     g_saveloc_auto_delay    = v != 0;
        else if (!std::strcmp(key, "require_flags"))  g_match_require_flags   = v != 0;
        else if (!std::strcmp(key, "delay_frames"))   g_saveloc_delay_frames  = v;
        else if (!std::strcmp(key, "stitch_max_err")) g_stitch_max_err        = static_cast<float>(v);
        else if (!std::strcmp(key, "slot_override"))  g_saveloc_slot_override = v;
        else if (!std::strcmp(key, "reverse"))        g_reverse_playback      = v != 0;
        else if (!std::strcmp(key, "sideways"))       g_sideways_playback     = v != 0;
        else if (!std::strcmp(key, "rev_pitch"))      g_reverse_pitch_offset  = v;
        else if (!std::strcmp(key, "side_pitch"))     g_sideways_pitch_offset = v;
        else if (!std::strcmp(key, "strafe_opt"))     g_strafe_optimizer      = v != 0;
        else if (!std::strcmp(key, "teleport_start")) g_teleport_to_start     = v != 0;
        else if (!std::strcmp(key, "variable_walk"))  g_variable_walk         = v != 0;
        else if (!std::strcmp(key, "rmb_overdub"))    g_rmb_overdub           = v != 0;
        else if (!std::strcmp(key, "compact_hud"))    g_compact_hud           = v != 0;
        else if (!std::strcmp(key, "hud_top_left"))   g_hud_top_left          = v != 0;
        else if (!std::strncmp(key, "bind_", 5)) {
            int i = std::atoi(key + 5);
            if (i >= 0 && i < BindCount && v != 0)
                g_binds[i].vk = v;   // any input allowed, including mouse L/R
        }
    }

    void config_save() {
        if (!g_config_path[0]) return;
        std::string s;
        config_serialize(s);
        FILE* f = nullptr;
        fopen_s(&f, g_config_path, "w");
        if (!f) return;
        std::fwrite(s.data(), 1, s.size(), f);
        std::fclose(f);
        g_last_saved_cfg = s;
    }

    void config_load() {
        if (!g_config_path[0]) return;
        FILE* f = nullptr;
        fopen_s(&f, g_config_path, "r");
        if (!f) return;
        char line[128];
        while (std::fgets(line, sizeof(line), f)) {
            char* eq = std::strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            config_apply(line, std::atoi(eq + 1));
        }
        std::fclose(f);
        // Snapshot post-load state so autosave doesn't immediately rewrite
        // an identical file.
        g_last_saved_cfg.clear();
        config_serialize(g_last_saved_cfg);
        hooks::log("[Config] loaded %s", g_config_path);
    }

    // Autosave: every ~2s serialize the settings and write only when they
    // differ from the last saved snapshot — any change sticks without a
    // save button, and idle frames cost one small string compare.
    static void config_autosave_tick() {
        static int frames = 0;
        if (++frames < 120) return;
        frames = 0;
        if (!g_config_path[0]) return;
        std::string s;
        config_serialize(s);
        if (s != g_last_saved_cfg) config_save();
    }

    void init_paths() {
        char base[MAX_PATH];
        if (SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, base) != S_OK)
            return;
        std::snprintf(g_tapes_dir, sizeof(g_tapes_dir), "%s\\momentum-menu\\tapes", base);
        std::snprintf(g_config_path, sizeof(g_config_path), "%s\\momentum-menu\\config.cfg", base);

        char parent[MAX_PATH];
        std::snprintf(parent, sizeof(parent), "%s\\momentum-menu", base);
        CreateDirectoryA(parent, nullptr);
        CreateDirectoryA(g_tapes_dir, nullptr);

        config_load();
    }

    static void resolve_path(const char* name, char* out, size_t out_sz) {
        bool absolute = (name[0] && name[1] == ':') || name[0] == '\\';
        if (absolute) std::snprintf(out, out_sz, "%s", name);
        else          std::snprintf(out, out_sz, "%s\\%s", g_tapes_dir, name);
    }

    static void refresh_tape_list() {
        g_tape_list.clear();
        char pattern[MAX_PATH];
        std::snprintf(pattern, sizeof(pattern), "%s\\*.mtape", g_tapes_dir);

        WIN32_FIND_DATAA fd{};
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                g_tape_list.emplace_back(fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    // ─── theme ────────────────────────────────────────────────────────
    // Applied once per frame — cheap, and lets us stay in sync with ImGui's
    // per-frame style stack without persisting global state changes.
    static void apply_theme() {
        ImGuiStyle& s = ImGui::GetStyle();
        s.WindowPadding        = ImVec2(14, 12);
        s.FramePadding         = ImVec2(10, 6);
        s.CellPadding          = ImVec2(8, 4);
        s.ItemSpacing          = ImVec2(10, 8);
        s.ItemInnerSpacing     = ImVec2(6, 4);
        s.IndentSpacing        = 20.f;
        s.ScrollbarSize        = 12.f;
        s.GrabMinSize          = 10.f;
        s.WindowRounding       = 8.f;
        s.ChildRounding        = 6.f;
        s.FrameRounding        = 6.f;
        s.PopupRounding        = 6.f;
        s.ScrollbarRounding    = 10.f;
        s.GrabRounding         = 6.f;
        s.TabRounding          = 6.f;
        s.WindowBorderSize     = 1.f;
        s.FrameBorderSize      = 0.f;
        s.PopupBorderSize      = 1.f;
        s.WindowTitleAlign     = ImVec2(0.f, 0.5f);
        s.ButtonTextAlign      = ImVec2(0.5f, 0.5f);
        s.SeparatorTextBorderSize = 1.f;
        s.SeparatorTextAlign      = ImVec2(0.f, 0.5f);
        s.SeparatorTextPadding    = ImVec2(16, 4);

        ImVec4* c = s.Colors;
        c[ImGuiCol_Text]                 = ImVec4(0.90f, 0.92f, 0.96f, 1.00f);
        c[ImGuiCol_TextDisabled]         = ImVec4(0.44f, 0.47f, 0.53f, 1.00f);
        c[ImGuiCol_WindowBg]             = ImVec4(0.070f, 0.078f, 0.098f, 1.00f);
        c[ImGuiCol_ChildBg]              = ImVec4(0.090f, 0.100f, 0.125f, 1.00f);
        c[ImGuiCol_PopupBg]              = ImVec4(0.090f, 0.100f, 0.125f, 0.98f);
        c[ImGuiCol_Border]               = ImVec4(0.16f, 0.18f, 0.22f, 1.00f);
        c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_FrameBg]              = ImVec4(0.115f, 0.130f, 0.160f, 1.00f);
        c[ImGuiCol_FrameBgHovered]       = ImVec4(0.150f, 0.170f, 0.205f, 1.00f);
        c[ImGuiCol_FrameBgActive]        = ImVec4(0.180f, 0.200f, 0.240f, 1.00f);
        c[ImGuiCol_TitleBg]              = ImVec4(0.080f, 0.090f, 0.110f, 1.00f);
        c[ImGuiCol_TitleBgActive]        = ImVec4(0.090f, 0.100f, 0.125f, 1.00f);
        c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.080f, 0.090f, 0.110f, 1.00f);
        c[ImGuiCol_MenuBarBg]            = ImVec4(0.080f, 0.090f, 0.110f, 1.00f);
        c[ImGuiCol_ScrollbarBg]          = ImVec4(0.070f, 0.078f, 0.098f, 0.00f);
        c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
        c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.25f, 0.28f, 0.34f, 1.00f);
        c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.30f, 0.34f, 0.40f, 1.00f);
        c[ImGuiCol_CheckMark]            = ImVec4(0.35f, 0.78f, 0.98f, 1.00f);
        c[ImGuiCol_SliderGrab]           = ImVec4(0.35f, 0.78f, 0.98f, 1.00f);
        c[ImGuiCol_SliderGrabActive]     = ImVec4(0.55f, 0.85f, 1.00f, 1.00f);
        c[ImGuiCol_Button]               = ImVec4(0.16f, 0.19f, 0.24f, 1.00f);
        c[ImGuiCol_ButtonHovered]        = ImVec4(0.22f, 0.26f, 0.32f, 1.00f);
        c[ImGuiCol_ButtonActive]         = ImVec4(0.28f, 0.32f, 0.40f, 1.00f);
        c[ImGuiCol_Header]               = ImVec4(0.14f, 0.16f, 0.20f, 1.00f);
        c[ImGuiCol_HeaderHovered]        = ImVec4(0.18f, 0.21f, 0.26f, 1.00f);
        c[ImGuiCol_HeaderActive]         = ImVec4(0.22f, 0.26f, 0.32f, 1.00f);
        c[ImGuiCol_Separator]            = ImVec4(0.16f, 0.18f, 0.22f, 1.00f);
        c[ImGuiCol_SeparatorHovered]     = ImVec4(0.35f, 0.78f, 0.98f, 0.60f);
        c[ImGuiCol_SeparatorActive]      = ImVec4(0.35f, 0.78f, 0.98f, 1.00f);
        c[ImGuiCol_ResizeGrip]           = ImVec4(0.18f, 0.20f, 0.24f, 0.80f);
        c[ImGuiCol_ResizeGripHovered]    = ImVec4(0.35f, 0.78f, 0.98f, 0.60f);
        c[ImGuiCol_ResizeGripActive]     = ImVec4(0.35f, 0.78f, 0.98f, 0.90f);
        c[ImGuiCol_Tab]                  = ImVec4(0.090f, 0.100f, 0.125f, 1.00f);
        c[ImGuiCol_TabHovered]           = ImVec4(0.18f, 0.21f, 0.26f, 1.00f);
        c[ImGuiCol_TabActive]            = ImVec4(0.14f, 0.16f, 0.20f, 1.00f);
        c[ImGuiCol_TabUnfocused]         = ImVec4(0.080f, 0.090f, 0.110f, 1.00f);
        c[ImGuiCol_TabUnfocusedActive]   = ImVec4(0.110f, 0.125f, 0.150f, 1.00f);
        c[ImGuiCol_TextSelectedBg]       = ImVec4(0.35f, 0.78f, 0.98f, 0.35f);
        c[ImGuiCol_DragDropTarget]       = ImVec4(0.35f, 0.78f, 0.98f, 0.90f);
        c[ImGuiCol_NavHighlight]         = ImVec4(0.35f, 0.78f, 0.98f, 0.60f);
    }

    static constexpr ImU32 REC_RED  = IM_COL32(240, 80, 90, 255);
    static constexpr ImU32 PLAY_GRN = IM_COL32(90, 210, 130, 255);
    static constexpr ImU32 WARN_YLW = IM_COL32(240, 200, 90, 255);
    static constexpr ImU32 IDLE_GRY = IM_COL32(140, 148, 160, 255);

    // ─── helpers ──────────────────────────────────────────────────────

    static void hint(const char* text) {
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.f);
            ImGui::TextUnformatted(text);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

    static void section(const char* label) {
        ImGui::Dummy(ImVec2(0, 2));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.78f, 0.98f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.35f, 0.78f, 0.98f, 0.60f));
        ImGui::SeparatorText(label);
        ImGui::PopStyleColor(2);
    }

    // Pill: colored dot + label with subtle bg. Draws inline at cursor.
    static void mode_pill() {
        const char* txt = "IDLE";
        ImU32 dot = IDLE_GRY;
        switch (recorder::mode()) {
        case recorder::Mode::Recording: txt = "RECORDING"; dot = REC_RED;  break;
        case recorder::Mode::Playing:   txt = "PLAYING";   dot = PLAY_GRN; break;
        case recorder::Mode::Seeking:   txt = "SEEKING";   dot = WARN_YLW; break;
        case recorder::Mode::Settling:  txt = "SETTLING";  dot = WARN_YLW; break;
        default: break;
        }
        ImVec2 sz = ImGui::CalcTextSize(txt);
        float dot_r  = 5.f;
        float pad_x  = 12.f;
        float pad_y  = 5.f;
        float dot_gap = 10.f;
        float w = sz.x + pad_x * 2.f + dot_r * 2.f + dot_gap;
        float h = sz.y + pad_y * 2.f;
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                          IM_COL32(20, 24, 30, 255), h * 0.5f);
        dl->AddRect      (p, ImVec2(p.x + w, p.y + h),
                          IM_COL32(38, 46, 58, 255), h * 0.5f);
        dl->AddCircleFilled(ImVec2(p.x + pad_x + dot_r, p.y + h * 0.5f), dot_r, dot);
        dl->AddText(ImVec2(p.x + pad_x + dot_r * 2.f + dot_gap, p.y + pad_y),
                    IM_COL32(230, 232, 240, 255), txt);
        ImGui::Dummy(ImVec2(w, h));
    }

    // Header banner: app name + mode pill + tape stats on one line.
    static void draw_header() {
        ImGui::Text("MOMENTUM MOVEMENT RECORDER");
        ImGui::SameLine();
        ImGui::TextDisabled(" v3 ");
        ImGui::SameLine(0.f, 24.f);
        mode_pill();
        ImGui::SameLine(0.f, 24.f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("frame %zu / %zu", recorder::play_head(), recorder::tape_size());
    }

    // ─── welcome toast ────────────────────────────────────────────────
    static float g_welcome_timer = 0.f;

    void start_welcome() { g_welcome_timer = 5.f; }

    static void draw_welcome() {
        if (g_welcome_timer <= 0.f) return;
        g_welcome_timer -= ImGui::GetIO().DeltaTime;

        ImGuiIO& io = ImGui::GetIO();
        ImDrawList* dl = ImGui::GetForegroundDrawList();

        const char* line1 = "MOMENTUM MOVEMENT RECORDER";
        char line2[64];
        std::snprintf(line2, sizeof(line2), "press %s to open", bind_key_name(BindMenu));

        ImVec2 s1 = ImGui::CalcTextSize(line1);
        ImVec2 s2 = ImGui::CalcTextSize(line2);
        float w = (s1.x > s2.x ? s1.x : s2.x) + 60.f;
        float h = s1.y + s2.y + 34.f;

        float cx = io.DisplaySize.x * 0.5f;
        float cy = io.DisplaySize.y * 0.24f;
        float alpha = g_welcome_timer > 1.f ? 1.f : g_welcome_timer;

        ImU32 bg = IM_COL32(18, 20, 26, static_cast<int>(230 * alpha));
        ImU32 accent = IM_COL32(90, 200, 250, static_cast<int>(220 * alpha));
        ImU32 fg = IM_COL32(230, 232, 240, static_cast<int>(240 * alpha));
        ImU32 sub = IM_COL32(140, 148, 160, static_cast<int>(200 * alpha));

        dl->AddRectFilled({ cx - w * 0.5f, cy - h * 0.5f },
                          { cx + w * 0.5f, cy + h * 0.5f },
                          bg, 10.f);
        dl->AddRect      ({ cx - w * 0.5f, cy - h * 0.5f },
                          { cx + w * 0.5f, cy + h * 0.5f },
                          accent, 10.f, 0, 1.5f);
        dl->AddLine({ cx - s1.x * 0.5f, cy - h * 0.5f + s1.y + 12.f },
                    { cx + s1.x * 0.5f, cy - h * 0.5f + s1.y + 12.f },
                    accent, 1.5f);
        dl->AddText({ cx - s1.x * 0.5f, cy - h * 0.5f + 10.f },  fg,  line1);
        dl->AddText({ cx - s2.x * 0.5f, cy + h * 0.5f - s2.y - 10.f }, sub, line2);
    }

    // ─── status chip when menu is closed ──────────────────────────────
    static void draw_status_overlay() {
        recorder::Mode m = recorder::mode();
        if (m == recorder::Mode::Idle) return;

        ImGuiIO& io = ImGui::GetIO();
        ImDrawList* dl = ImGui::GetForegroundDrawList();

        const char* label = "IDLE";
        ImU32 dot = IDLE_GRY;
        switch (m) {
        case recorder::Mode::Recording: label = "REC";  dot = REC_RED;  break;
        case recorder::Mode::Playing:   label = "PLAY"; dot = PLAY_GRN; break;
        case recorder::Mode::Seeking:   label = "SEEK"; dot = WARN_YLW; break;
        case recorder::Mode::Settling:  label = "WAIT"; dot = WARN_YLW; break;
        default: break;
        }

        char line[96];
        std::snprintf(line, sizeof(line), "%s   %zu / %zu",
                      label, recorder::play_head(), recorder::tape_size());

        ImVec2 sz = ImGui::CalcTextSize(line);
        float pad_x = 14.f, pad_y = 8.f, dot_r = 4.5f, dot_gap = 10.f;
        float w = sz.x + pad_x * 2.f + dot_r * 2.f + dot_gap;
        float h = sz.y + pad_y * 2.f;
        ImVec2 pos = { 20.f, io.DisplaySize.y - h - 20.f };
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h),
                          IM_COL32(14, 16, 20, 220), h * 0.5f);
        dl->AddRect      (pos, ImVec2(pos.x + w, pos.y + h),
                          IM_COL32(38, 46, 58, 240), h * 0.5f);
        dl->AddCircleFilled({ pos.x + pad_x + dot_r, pos.y + h * 0.5f }, dot_r, dot);
        dl->AddText({ pos.x + pad_x + dot_r * 2.f + dot_gap, pos.y + pad_y },
                    IM_COL32(230, 232, 240, 240), line);

        // BIG middle-left chip — live verdicts while recording / playing.
        {
            char big_line[128], sub_line[128];
            ImU32 vcol = PLAY_GRN;

            if (m == recorder::Mode::Playing) {
                int   seg    = recorder::playback_current_segment();
                float drift  = recorder::playback_drift_units();
                int   broken = recorder::playback_broken_segment();

                const char* verdict = "ON TRACK";
                if      (drift > 200.f) { verdict = "BROKEN";   vcol = REC_RED; }
                else if (drift >  75.f) { verdict = "DRIFTING"; vcol = WARN_YLW; }

                std::snprintf(big_line, sizeof(big_line), "SEG %d  %s", seg, verdict);
                if (broken > 0)
                    std::snprintf(sub_line, sizeof(sub_line),
                                  "drift %.0f u    seg %d BROKE — redo",
                                  drift, broken);
                else
                    std::snprintf(sub_line, sizeof(sub_line), "drift %.0f units", drift);
            } else if (m == recorder::Mode::Recording) {
                int   commits    = recorder::commits_made();
                float cur_dist   = recorder::current_stitch_dist();
                float min_dist   = recorder::min_stitch_dist_since_commit();
                bool  in_air     = recorder::last_commit_in_air();

                if (commits == 0) {
                    vcol = REC_RED;
                    std::snprintf(big_line, sizeof(big_line), "RECORDING");
                    std::snprintf(sub_line, sizeof(sub_line),
                                  "frame %zu   press %s to commit a segment",
                                  recorder::tape_size(), bind_key_name(BindCommit));
                } else {
                    // Verdict on WHAT WE CAN OBSERVE — retry re-acquires
                    // pos+vel+flags similar to the E-press state.
                    const char* verdict;
                    if      (min_dist > 500.f) { verdict = "NO MATCH — rollback + redo"; vcol = REC_RED; }
                    else if (min_dist > 100.f) { verdict = "RETRY CATCHING UP";          vcol = WARN_YLW; }
                    else if (in_air)           { verdict = "MATCHED (air commit — risky)"; vcol = WARN_YLW; }
                    else                        { verdict = "MATCHED";                    vcol = PLAY_GRN; }

                    std::snprintf(big_line, sizeof(big_line),
                                  "REC  segment %d / %d  %s",
                                  recorder::selected_segment(),
                                  recorder::segment_count(), verdict);
                    std::snprintf(sub_line, sizeof(sub_line),
                                  "%s / %s select a segment    cur %.0f  best %.0f",
                                  bind_key_name(BindPrevloc), bind_key_name(BindNextloc),
                                  cur_dist, min_dist);
                }
            } else if (m == recorder::Mode::Seeking) {
                vcol = WARN_YLW;
                std::snprintf(big_line, sizeof(big_line), "SEEKING");
                std::snprintf(sub_line, sizeof(sub_line),
                              "walking to start... (turn on teleport-to-start for sv_cheats)");
            } else {  // Settling
                vcol = WARN_YLW;
                std::snprintf(big_line, sizeof(big_line), "SETTLING");
                std::snprintf(sub_line, sizeof(sub_line),
                              "stabilizing at start...");
            }

            ImFont* font = ImGui::GetFont();
            // Compact mode shrinks the whole chip ~45%.
            const float s = g_compact_hud ? 0.55f : 1.0f;
            const float big_sz   = 40.f * s;
            const float sub_sz   = 22.f * s;
            const float pad_x2   = 28.f * s, pad_y2 = 18.f * s;
            const float dot_r2   = 11.f * s, dot_gap2 = 20.f * s, line_gap = 8.f * s;

            ImVec2 big_ts = font->CalcTextSizeA(big_sz, FLT_MAX, 0.f, big_line);
            ImVec2 sub_ts = font->CalcTextSizeA(sub_sz, FLT_MAX, 0.f, sub_line);
            float text_w = big_ts.x > sub_ts.x ? big_ts.x : sub_ts.x;
            float text_h = big_ts.y + line_gap + sub_ts.y;

            float w2 = text_w + pad_x2 * 2.f + dot_r2 * 2.f + dot_gap2;
            float h2 = text_h + pad_y2 * 2.f;
            // Top-left corner, or vertically-centered on the left edge (default).
            ImVec2 vpos = g_hud_top_left
                        ? ImVec2{ 24.f, 24.f }
                        : ImVec2{ 24.f, io.DisplaySize.y * 0.5f - h2 * 0.5f };

            dl->AddRectFilled(vpos, ImVec2(vpos.x + w2, vpos.y + h2),
                              IM_COL32(8, 10, 14, 240), 12.f);
            dl->AddRect      (vpos, ImVec2(vpos.x + w2, vpos.y + h2),
                              vcol, 12.f, 0, 3.f);
            dl->AddCircleFilled({ vpos.x + pad_x2 + dot_r2, vpos.y + h2 * 0.5f }, dot_r2, vcol);

            float text_x = vpos.x + pad_x2 + dot_r2 * 2.f + dot_gap2;
            dl->AddText(font, big_sz,
                        { text_x, vpos.y + pad_y2 },
                        IM_COL32(245, 248, 252, 255), big_line);
            dl->AddText(font, sub_sz,
                        { text_x, vpos.y + pad_y2 + big_ts.y + line_gap },
                        IM_COL32(190, 198, 210, 245), sub_line);
        }
    }

    // ─── strafe gauge overlay ─────────────────────────────────────────
    // Top-center panel proving the optimizer is doing something: live
    // speed, gain over the trailing second, sync % (active ticks that
    // gained), carve-side arrows, and a ~4s speed sparkline.
    static void draw_strafe_gauge() {
        if (!g_strafe_optimizer) return;
        recorder::Mode m = recorder::mode();
        if (m != recorder::Mode::Idle && m != recorder::Mode::Recording) return;

        ImGuiIO& io = ImGui::GetIO();
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImFont* font = ImGui::GetFont();

        bool  active = recorder::strafe_opt_active();
        int   side   = recorder::strafe_opt_side();
        float speed  = recorder::strafe_speed();
        float gain   = recorder::strafe_gain_1s();
        int   sync   = recorder::strafe_sync_pct();

        const float w = 380.f, h = 118.f;
        ImVec2 p = { io.DisplaySize.x * 0.5f - w * 0.5f, 14.f };

        ImU32 border = !active        ? IM_COL32(90, 98, 110, 220)
                     : gain >= 0.f    ? PLAY_GRN
                                      : WARN_YLW;
        dl->AddRectFilled(p, { p.x + w, p.y + h }, IM_COL32(8, 10, 14, 235), 10.f);
        dl->AddRect      (p, { p.x + w, p.y + h }, border, 10.f, 0, 2.5f);

        // Label row.
        dl->AddText(font, 15.f, { p.x + 14.f, p.y + 8.f },
                    IM_COL32(140, 148, 160, 255), "STRAFE OPT");
        const char* state = active ? "ACTIVE"
                          : speed > 1.f ? "IDLE (on ramp / grounded / slow)"
                                        : "IDLE";
        dl->AddText(font, 15.f, { p.x + 104.f, p.y + 8.f },
                    active ? PLAY_GRN : IDLE_GRY, state);

        // Big speed readout.
        char big[48];
        std::snprintf(big, sizeof(big), "%.0f ups", speed);
        dl->AddText(font, 32.f, { p.x + 14.f, p.y + 26.f },
                    IM_COL32(245, 248, 252, 255), big);

        // Gain + sync.
        char sub[64];
        if (sync >= 0)
            std::snprintf(sub, sizeof(sub), "%+.0f ups/s    sync %d%%", gain, sync);
        else
            std::snprintf(sub, sizeof(sub), "%+.0f ups/s    sync --", gain);
        dl->AddText(font, 16.f, { p.x + 14.f, p.y + 60.f },
                    IM_COL32(190, 198, 210, 245), sub);

        // Carve-side arrows, right-aligned.
        {
            const char* txt = side > 0 ? "<< LEFT" : side < 0 ? "RIGHT >>" : "--";
            ImU32 col = side != 0 ? IM_COL32(90, 200, 250, 255) : IDLE_GRY;
            ImVec2 ts = font->CalcTextSizeA(20.f, FLT_MAX, 0.f, txt);
            dl->AddText(font, 20.f, { p.x + w - ts.x - 14.f, p.y + 32.f }, col, txt);
        }

        // Sparkline strip along the bottom.
        {
            static float samples[256];
            int n = recorder::strafe_history(samples, 256);
            if (n >= 2) {
                float lo = samples[0], hi = samples[0];
                for (int i = 1; i < n; ++i) {
                    if (samples[i] < lo) lo = samples[i];
                    if (samples[i] > hi) hi = samples[i];
                }
                // Keep a minimum span so flat lines don't zoom into noise.
                if (hi - lo < 50.f) { float mid = (hi + lo) * 0.5f; lo = mid - 25.f; hi = mid + 25.f; }

                float gx = p.x + 14.f, gy = p.y + h - 34.f;
                float gw = w - 28.f,  gh = 26.f;
                dl->AddRectFilled({ gx, gy }, { gx + gw, gy + gh },
                                  IM_COL32(20, 24, 30, 255), 4.f);
                ImVec2 pts[256];
                for (int i = 0; i < n; ++i) {
                    float t = static_cast<float>(i) / static_cast<float>(n - 1);
                    float v = (samples[i] - lo) / (hi - lo);
                    pts[i] = { gx + t * gw, gy + gh - v * gh };
                }
                dl->AddPolyline(pts, n, IM_COL32(90, 200, 250, 255), 0, 1.5f);
            }
        }
    }

    // ─── recorder tab ─────────────────────────────────────────────────

    static void transport_controls() {
        section("TRANSPORT");
        const ImVec2 btn(120, 30);

        if (recorder::mode() == recorder::Mode::Recording) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.72f, 0.24f, 0.28f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.82f, 0.30f, 0.34f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.62f, 0.20f, 0.24f, 1.00f));
            if (ImGui::Button("stop rec", btn)) recorder::stop();
            ImGui::PopStyleColor(3);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.14f, 0.42f, 0.62f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.52f, 0.72f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.12f, 0.36f, 0.54f, 1.00f));
            if (ImGui::Button("record", btn)) recorder::start_record();
            ImGui::PopStyleColor(3);
        }
        ImGui::SameLine();

        if (recorder::mode() == recorder::Mode::Playing) {
            if (ImGui::Button("pause", btn)) recorder::pause();
        } else {
            ImGui::BeginDisabled(recorder::tape_size() == 0);
            if (ImGui::Button("play", btn)) {
                if (recorder::play_head() >= recorder::tape_size())
                    recorder::play_from_start();
                else
                    recorder::resume_playback();
            }
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (ImGui::Button("rewind", btn)) recorder::set_play_head(0);
        ImGui::SameLine();
        if (ImGui::Button("stop",   btn)) recorder::stop();

        bool loop = recorder::looping();
        if (ImGui::Checkbox("loop playback", &loop)) recorder::set_looping(loop);

        ImGui::Checkbox("right-click during playback = overdub from here", &g_rmb_overdub);
        hint("While a tape is playing, right-click to take over live and "
             "re-record from the current frame onward — the tail is discarded "
             "and you keep surfing. Use it to fix a bad segment without redoing "
             "the whole run: play back, right-click at a clean spot BEFORE the "
             "break (drift still ~0), and re-run the rest. Off by default so a "
             "stray click can't truncate your tape.");

        float spd = recorder::play_speed();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##speed", &spd, 0.1f, 3.0f, "speed  %.2fx"))
            recorder::set_play_speed(spd);

        if (recorder::tape_size() > 0) {
            int head = static_cast<int>(recorder::play_head());
            int max  = static_cast<int>(recorder::tape_size());
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("##scrub", &head, 0, max, "scrub  frame %d"))
                recorder::set_play_head(head < 0 ? 0 : head);
        } else {
            ImGui::TextDisabled("empty tape — press RECORD or load one from Files.");
        }
    }

    static void saveloc_controls() {
        section("SAVELOC STITCHING");
        char lbl[128];
        std::snprintf(lbl, sizeof(lbl), "SaveLoc mode  (%s/%s/%s/%s build a stitched run)",
                      bind_key_name(BindCommit), bind_key_name(BindRollback),
                      bind_key_name(BindPrevloc), bind_key_name(BindNextloc));
        ImGui::Checkbox(lbl, &g_saveloc_mode);
        hint("Records commit and rollback markers in the tape. During "
             "playback, the failed segments between commit and rollback are "
             "cut out so the run plays back as one seamless piece. Works on "
             "KSF and TRIX-style servers alike: the post-teleport freeze "
             "TRIX adds is measured from the tape per rollback and skipped "
             "automatically — no setting needed.");

        ImGui::Checkbox("auto-savestate the start of every RECORD", &g_auto_saveloc);
        hint("RECORD fires mom_savestate_create so the run's start state is "
             "saved. Before pressing PLAY, use mom_savestate_first to jump "
             "back to that start for an exact entry — the log warns if "
             "you're off.");

        {
            int cuts = recorder::stitch_cut_count();
            if (cuts > 0) {
                int risky = recorder::stitch_risky_count();
                ImGui::TextColored(risky ? ImVec4(1.f, 0.75f, 0.25f, 1.f)
                                         : ImVec4(0.4f, 0.9f, 0.4f, 1.f),
                                   "stitch report: %d cut(s), %d risky, worst err %.0f%s",
                                   cuts, risky, recorder::stitch_worst_err(),
                                   recorder::tape_has_phys_state()
                                       ? "" : "  (old tape - no phys data)");
            } else {
                ImGui::TextDisabled("stitch report appears here after a playback with cuts.");
            }
        }

        if (ImGui::TreeNode("advanced stitching")) {
            ImGui::Checkbox("auto-match skip windows (pos+vel+flags)", &g_saveloc_auto_delay);
            hint("Finds each rollback's exact teleport landing by matching "
                 "recorded position, velocity and physics flags. Leave ON — "
                 "the fixed fallback below is only used when no match exists.");
            ImGui::Checkbox("require ground/duck state match", &g_match_require_flags);
            hint("Stitch pairs must agree on FL_ONGROUND / FL_DUCKING / "
                 "FL_INWATER, not just position and velocity. Filters the "
                 "worst class of false match. Needs a tape from this build.");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderInt("##delay", &g_saveloc_delay_frames, 0, 20,
                             "fallback delay-comp  %d frames");
            hint("Used only when auto-match finds no snap for a rollback. "
                 "Tune per ping — roughly 3-4 @ 50ms, 5-6 @ 100ms.");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##stitcherr", &g_stitch_max_err, 25.f, 500.f,
                               "risky-cut threshold  err > %.0f");
            hint("Cuts with match error above this are flagged RISKY in the "
                 "report and the log — re-record those segments if playback "
                 "drifts there.");

            ImGui::Text("last captured slot: %u", recorder::record_slot());
            ImGui::SameLine();
            if (ImGui::SmallButton("reset counter")) recorder::reset_saveloc_counter();
            ImGui::SetNextItemWidth(120);
            ImGui::InputInt("slot override", &g_saveloc_slot_override);
            hint("Force the record-slot number if other players' savelocs "
                 "shift the plugin's counter. 0 = auto.");
            ImGui::TreePop();
        }
    }

    static void playback_style_controls() {
        section("PLAYBACK STYLE");
        ImGui::Checkbox("backwards  (server sees you surf facing 180)", &g_reverse_playback);
        hint("Cmd yaw rotates 180 and fwd/side inputs are negated — the "
             "world-space motion is IDENTICAL to the recording, but the "
             "server, spectators and demos see a genuine backwards surf.");
        ImGui::BeginDisabled(!g_reverse_playback);
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderInt("##rev_pitch", &g_reverse_pitch_offset, -45, 45,
                         "backwards pitch offset  %d deg");
        hint("Extra pitch on top of the recorded pitch during backwards "
             "playback. Negative = look down. Spectators see it too.");
        ImGui::EndDisabled();
        ImGui::Checkbox("sideways  (server sees you surf facing 90)", &g_sideways_playback);
        hint("Same trick as backwards: cmd yaw rotates 90 CW and the move "
             "inputs rotate with it (W-A-S-D cross shifts one key) — the "
             "world-space motion is IDENTICAL to the recording, but the "
             "server and spectators see a sideways surf.");
        ImGui::BeginDisabled(!g_sideways_playback);
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderInt("##side_pitch", &g_sideways_pitch_offset, -45, 45,
                         "sideways pitch offset  %d deg");
        hint("Extra pitch on top of the recorded pitch during sideways "
             "playback. Negative = look down. Spectators see it too.");
        ImGui::EndDisabled();
    }

    static void assist_controls() {
        section("ASSISTS");
        ImGui::Checkbox("strafe optimizer  (free-air perpendicular-wish accel)", &g_strafe_optimizer);
        hint("Only engages in genuine free-fall — the air gaps between ramps "
             "and bhop hops — never while a ramp is supporting you, so it "
             "won't fight your surf. When it fires (airborne, >300 ups) it "
             "rebuilds forward/side so the wish direction sits 90 degrees off "
             "your velocity, the maximal air-accel gain on every Source-engine config. "
             "It moves the W/A/S/D inputs, NOT your mouse — you still aim; "
             "the carve follows your view. Hold S to brake. Top-center gauge "
             "shows speed, gain and sync. Inactive during playback so tapes "
             "replay untouched; recordings made with it ON bake the inputs in.");
    }

    static void draw_recorder_tab() {
        transport_controls();
        saveloc_controls();
        playback_style_controls();
        assist_controls();
    }

    // ─── settings tab ─────────────────────────────────────────────────

    static void binds_controls() {
        section("KEYBINDS");
        ImGui::TextDisabled("click a bind, then press ANY input to set it. ESC cancels.");
        ImGui::TextDisabled("keyboard and every mouse button work — L/R-click, 3/4/5.");

        for (int i = 0; i < BindCount; ++i) {
            ImGui::PushID(i);
            bool capturing = (g_capture_action == i);
            if (capturing) {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.14f, 0.42f, 0.62f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.52f, 0.72f, 1.00f));
            }
            const char* key_txt = capturing ? "press a key..." : vk_name(g_binds[i].vk);
            if (ImGui::Button(key_txt, ImVec2(150, 0)))
                g_capture_action = capturing ? -1 : i;
            if (capturing) ImGui::PopStyleColor(2);
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(g_binds[i].label);
            ImGui::PopID();
        }

        // Flag duplicate keys — first collision wins at dispatch time, so
        // the user should know when two actions share a bind.
        for (int i = 0; i < BindCount; ++i)
            for (int j = i + 1; j < BindCount; ++j)
                if (g_binds[i].vk == g_binds[j].vk)
                    ImGui::TextColored(ImVec4(1.f, 0.75f, 0.25f, 1.f),
                                       "warning: '%s' and '%s' share %s",
                                       g_binds[i].label, g_binds[j].label,
                                       vk_name(g_binds[i].vk));

        if (ImGui::SmallButton("reset all to defaults")) {
            for (int i = 0; i < BindCount; ++i) g_binds[i].vk = g_binds[i].def_vk;
            g_capture_action = -1;
        }
    }

    static void hud_controls() {
        section("ON-SCREEN OVERLAY");
        ImGui::Checkbox("compact status chip (smaller)", &g_compact_hud);
        hint("Shrinks the REC / segment / drift chip to about half size so it "
             "takes up less of the screen while recording and playing back.");
        ImGui::Checkbox("move overlay to top-left corner", &g_hud_top_left);
        hint("Puts the status chip in the top-left instead of centered on the "
             "left edge. Pairs well with compact mode for a minimal HUD.");
    }

    static void start_behavior_controls() {
        section("PLAY START");
        ImGui::Checkbox("teleport to start (needs sv_cheats 1)", &g_teleport_to_start);
        hint("Pixel-perfect start via setpos+setang. Requires sv_cheats. "
             "Off = walk-to-start + settle, or teleport to the run's first "
             "saveloc yourself before playing.");
        ImGui::Checkbox("variable walk speed (taper on approach)", &g_variable_walk);
        hint("Ramps down walk speed while approaching the start position "
             "for a precise landing. Off = always 450.");
    }

    static void config_controls() {
        section("CONFIG");
        ImGui::TextDisabled("%s", g_config_path[0] ? g_config_path : "(config path unavailable)");
        ImGui::TextDisabled("settings + keybinds autoload at inject and autosave on change.");
        if (ImGui::SmallButton("save now")) config_save();
    }

    static void draw_settings_tab() {
        binds_controls();
        hud_controls();
        start_behavior_controls();
        config_controls();

        ImGui::Dummy(ImVec2(0, 6));
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.72f, 0.24f, 0.28f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.82f, 0.30f, 0.34f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.62f, 0.20f, 0.24f, 1.00f));
        if (ImGui::Button("UNLOAD DLL  (rebuild without restarting Momentum)", ImVec2(-1, 34))) {
            config_save();
            hooks::request_unload();
        }
        ImGui::PopStyleColor(3);
        ImGui::TextDisabled("click, wait ~1s, then rebuild + re-inject.");
    }

    // ─── files tab ────────────────────────────────────────────────────

    static void draw_files_tab() {
        section("TAPES DIRECTORY");
        ImGui::TextDisabled("%s", g_tapes_dir);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##filename", g_name_buf, sizeof(g_name_buf));

        char full[MAX_PATH];
        resolve_path(g_name_buf, full, sizeof(full));

        static const char* last_msg = "";
        static float       last_msg_timer = 0.f;
        auto set_msg = [&](const char* m) { last_msg = m; last_msg_timer = 4.f; };

        const ImVec2 btn(120, 28);
        if (ImGui::Button("save", btn)) set_msg(recorder::save(full) ? "saved." : "save failed.");
        ImGui::SameLine();
        if (ImGui::Button("load", btn)) set_msg(recorder::load(full) ? "loaded." : "load failed.");
        ImGui::SameLine();
        if (ImGui::Button("open folder", btn))
            ShellExecuteA(nullptr, "open", g_tapes_dir, nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::SameLine();
        if (ImGui::Button("refresh", btn)) refresh_tape_list();

        if (last_msg_timer > 0.f) {
            last_msg_timer -= ImGui::GetIO().DeltaTime;
            ImGui::TextDisabled("%s", last_msg);
        }

        section("TAPES");
        if (g_tape_list.empty()) refresh_tape_list();
        if (ImGui::BeginListBox("##tapes", ImVec2(-1, 220))) {
            for (const auto& name : g_tape_list) {
                bool selected = std::strcmp(name.c_str(), g_name_buf) == 0;
                if (ImGui::Selectable(name.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                    std::snprintf(g_name_buf, sizeof(g_name_buf), "%s", name.c_str());
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        char p[MAX_PATH];
                        resolve_path(name.c_str(), p, sizeof(p));
                        set_msg(recorder::load(p) ? "loaded." : "load failed.");
                    }
                }
            }
            ImGui::EndListBox();
        }
        ImGui::TextDisabled("single-click to select, double-click to load.");
    }

    // ─── help tab ─────────────────────────────────────────────────────

    static void draw_help_tab() {
        section("HOTKEYS  (work with menu closed — rebind in Settings)");
        auto kv = [](const char* key, const char* desc) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.78f, 0.98f, 1.f));
            ImGui::Text("%-12s", key);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", desc);
        };
        kv(bind_key_name(BindMenu),   "toggle menu");
        kv(bind_key_name(BindPlay),   "play from start");
        kv(bind_key_name(BindPause),  "pause playback (keeps tape + head)");
        kv(bind_key_name(BindRecord), "start recording  (blocked during playback)");
        kv(bind_key_name(BindStop),   "stop");

        section("SAVESTATE HOTKEYS");
        kv(bind_key_name(BindCommit),   "commit — mom_savestate_create + commit marker (recording only)");
        kv(bind_key_name(BindRollback), "rollback — mom_savestate_load teleports to the CURRENT savestate, cuts the fail");
        kv(bind_key_name(BindPrevloc),  "prev savestate + teleport — mom_savestate_prev");
        kv(bind_key_name(BindNextloc),  "next savestate + teleport — mom_savestate_next");
        ImGui::TextDisabled("rollback/prev/next also work while idle (no marker) — never during playback.");

        section("WORKFLOW");
        ImGui::BulletText("record, commit at safe spots, rollback fails, stop.");
        ImGui::BulletText("mom_savestate_first back to the run's FIRST savestate, then play.");
        ImGui::BulletText("green stitch report = clean run; risky cuts name the segment to redo.");

        section("SYSTEM");
        ImGui::TextDisabled("log:   C:\\Users\\Talan\\momentum-menu\\momentum_menu.log");
        ImGui::TextDisabled("frame: %.2f ms   (%.0f fps)",
                    1000.f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);

        if (ImGui::CollapsingHeader("diagnostics")) {
            ImGui::TextDisabled("hook call counters — all should climb while in-game.");
            ImGui::Text("Present %d    CreateMove %d    IClientMode %d    WriteDelta %d",
                        hooks::endscene_calls(), hooks::createmove_calls(),
                        hooks::cm_createmove_calls(), hooks::write_delta_calls());

            ImGui::Dummy(ImVec2(0, 4));
            ImGui::TextDisabled("IInput vtable probe — the slot climbing at ~66/s is CreateMove.");
            int n = hooks::probe_count();
            int best = -1, best_hits = 0;
            for (int i = 0; i < n; ++i) {
                if (hooks::probe_hits(i) > best_hits) {
                    best_hits = hooks::probe_hits(i);
                    best = i;
                }
            }
            for (int i = 0; i < n; ++i) {
                int slot = hooks::probe_slot(i);
                int hits = hooks::probe_hits(i);
                bool winner = (i == best && hits > 0);
                ImU32 col = winner ? PLAY_GRN
                        : hits > 0 ? WARN_YLW
                                   : IDLE_GRY;
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                ImGui::Text("iv[%2d]   hits: %6d   %s",
                            slot, hits, winner ? " <- likely CreateMove" : "");
                ImGui::PopStyleColor();
            }
        }
    }

    // ─── main ─────────────────────────────────────────────────────────
    void draw() {
        apply_theme();
        draw_welcome();
        draw_status_overlay();
        draw_strafe_gauge();
        config_autosave_tick();
        if (!g_open) {
            // Nothing to click — cancel any half-finished rebind so a
            // stray keypress in-game doesn't get eaten by the capture.
            g_capture_action = -1;
            return;
        }

        ImGui::SetNextWindowSize(ImVec2(620, 560), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(520, 400), ImVec2(FLT_MAX, FLT_MAX));

        ImGui::Begin("##recorder", &g_open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

        draw_header();
        ImGui::Separator();

        if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_None)) {
            if (ImGui::BeginTabItem("Recorder")) { draw_recorder_tab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Settings")) { draw_settings_tab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Files"))    { draw_files_tab();    ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Help"))     { draw_help_tab();     ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }

        ImGui::Dummy(ImVec2(0, 4));
        ImGui::TextDisabled("%s to close.", bind_key_name(BindMenu));
        ImGui::End();
    }
}
