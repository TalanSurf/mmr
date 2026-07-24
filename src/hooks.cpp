#include "hooks.h"

#include <Windows.h>
#include <ShlObj.h>
#include <ShellAPI.h>
#include <Psapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "sdk.h"
#include "menu.h"
#include "recorder.h"

// Forward decl at global scope for the ClientCmd hook function defined at
// the bottom of this file.
void __fastcall hk_engine_client_cmd(void*, const char*);

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ─── logging ──────────────────────────────────────────────────────────
static const char* LOG_PATH = "C:\\Users\\Talan\\momentum-menu\\momentum_menu.log";

static void log(const char* fmt, ...) {
    FILE* f = nullptr;
    fopen_s(&f, LOG_PATH, "a");
    if (!f) return;

    time_t t = time(nullptr);
    struct tm tm{};
    localtime_s(&tm, &t);
    fprintf(f, "[%02d:%02d:%02d] ", tm.tm_hour, tm.tm_min, tm.tm_sec);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

// External-facing wrapper so other TUs (recorder.cpp, menu.cpp) can log too.
void hooks::log(const char* fmt, ...) {
    FILE* f = nullptr;
    fopen_s(&f, LOG_PATH, "a");
    if (!f) return;
    time_t t = time(nullptr);
    struct tm tm{};
    localtime_s(&tm, &t);
    fprintf(f, "[%02d:%02d:%02d] ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

// Get the game's top-level window without depending on state that install()
// may not have set yet.
static HWND find_top_window_this_process() {
    struct S { HWND out; };
    S s{ nullptr };
    EnumWindows([](HWND h, LPARAM p) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid != GetCurrentProcessId() || GetWindow(h, GW_OWNER) || !IsWindowVisible(h))
            return TRUE;
        reinterpret_cast<S*>(p)->out = h;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&s));
    return s.out;
}

// Drop a visible error file on the desktop and try to open it. Even if the
// game is exclusive-fullscreen and swallows every popup, the file survives.
static void die(const char* stage) {
    log("FAILED at: %s", stage);

    // triple error beep — audible even if UI is hidden
    for (int i = 0; i < 3; ++i) { MessageBeep(MB_ICONERROR); Sleep(120); }

    // grab last ~2 KB of the log for the error text
    char log_tail[2048] = "(couldn't read log)";
    FILE* lf = nullptr;
    fopen_s(&lf, LOG_PATH, "r");
    if (lf) {
        fseek(lf, 0, SEEK_END);
        long sz = ftell(lf);
        long start = sz > 1900 ? sz - 1900 : 0;
        fseek(lf, start, SEEK_SET);
        size_t n = fread(log_tail, 1, sizeof(log_tail) - 1, lf);
        log_tail[n] = '\0';
        fclose(lf);
    }

    // write the error to the desktop so he can double-click it
    char desktop[MAX_PATH] = "";
    char err_path[MAX_PATH] = "";
    if (SHGetFolderPathA(nullptr, CSIDL_DESKTOP, nullptr, 0, desktop) == S_OK) {
        std::snprintf(err_path, sizeof(err_path), "%s\\momentum_menu_ERROR.txt", desktop);
        FILE* ef = nullptr;
        fopen_s(&ef, err_path, "w");
        if (ef) {
            fprintf(ef,
                "momentum_menu — hook failed\r\n"
                "=========================\r\n\r\n"
                "Stage that failed:\r\n  %s\r\n\r\n"
                "Recent log tail:\r\n%s\r\n\r\n"
                "Full log: %s\r\n",
                stage, log_tail, LOG_PATH);
            fclose(ef);
        }
    }

    // yank the game out of fullscreen so subsequent popups can render
    HWND game = find_top_window_this_process();
    if (game) {
        ShowWindow(game, SW_MINIMIZE);
        Sleep(300);
    }

    // now the messagebox has a chance to appear
    char msg[3072];
    std::snprintf(msg, sizeof(msg),
        "momentum_menu couldn't hook: %s\n\n"
        "Details written to:\n  %s\n\n"
        "Recent log:\n%s",
        stage, err_path[0] ? err_path : "(no desktop)", log_tail);

    MessageBoxA(nullptr, msg, "momentum_menu — hook failed",
                MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);

    // and open the desktop file in notepad as a final fallback
    if (err_path[0]) {
        ShellExecuteA(nullptr, "open", err_path, nullptr, nullptr, SW_SHOWNORMAL);
    }
}

// ─── types ────────────────────────────────────────────────────────────
// Momentum Playtest ships on Strata Source — a modernized fork of Source
// SDK 2013 MP (DX11, panorama, KV3 tools, but the gameplay ABI is still
// the 2013-MP one). IBaseClientDLL::CreateMove has a VOID return here
// too — earlier leaked headers had it as bool; the wrong signature can
// smash the caller's stack.
using Present_t        = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffers_t  = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using WndProc_t        = LRESULT(CALLBACK*)(HWND, UINT, WPARAM, LPARAM);
using CreateMove_t     = void(*)(void*, int, float, bool);
// Reverted to 1-arg — the 2-arg version wasn't producing usable pointers on
// this build. slot 8's function returns something the outer hook was able
// to record with previously.
using GetUserCmd_t     = CUserCmd*(*)(void*, int);

constexpr int VT_SWAPCHAIN_PRESENT        = 8;  // IDXGISwapChain::Present
constexpr int VT_SWAPCHAIN_RESIZE_BUFFERS = 13; // IDXGISwapChain::ResizeBuffers
constexpr int VT_CLIENT_CM                = 21; // IBaseClientDLL::CreateMove — SDK 2013 MP slot. Momentum's client.dll should share it; the vtable dump at install-time is the ground truth if this stops working.
constexpr int VT_INPUT_GETCMD             = 8;  // Old, worked for recording capture on this build

static void**            g_swapchain_vt   = nullptr;
static Present_t         o_present_fn     = nullptr; // trampoline into original code
static ResizeBuffers_t   o_resize_buffers = nullptr;
static WndProc_t         o_wndproc        = nullptr;

// ─── minimal x64 length disassembler + inline hook ────────────────────
// Handles common Microsoft-compiled prologue instructions:
//   REX prefix, push r64, pop r64, mov r/m64 r64, sub rsp imm, lea, xor r r
// Returns 0 on unknown opcode; caller must abort in that case.
static int insn_length(const uint8_t* code) {
    uint8_t b0 = code[0];
    if (b0 >= 0x40 && b0 <= 0x4F) { int r = insn_length(code + 1); return r ? r + 1 : 0; }
    if (b0 >= 0x50 && b0 <= 0x5F) return 1;                    // push/pop r64
    if (b0 == 0x90)               return 1;                    // nop

    auto modrm_len = [&](int base) -> int {
        uint8_t m  = code[1];
        uint8_t mod = m >> 6;
        uint8_t rm  = m & 7;
        int    len  = base;
        if (mod != 3 && rm == 4) len++;                        // SIB
        if      (mod == 1) len += 1;
        else if (mod == 2) len += 4;
        else if (mod == 0 && rm == 5) len += 4;                // rip-rel
        return len;
    };

    switch (b0) {
    case 0x31: case 0x33:        // xor r,r/m
    case 0x89: case 0x8B:        // mov
    case 0x8D:                   // lea
    case 0x85:                   // test
        return modrm_len(2);
    case 0x83:                   // grp1 imm8
        return modrm_len(3);
    case 0x81: case 0xC7:        // grp1 imm32
        return modrm_len(6);
    }
    return 0;
}

static size_t safe_prologue_size(uint8_t* code, size_t min_bytes) {
    size_t total = 0;
    while (total < min_bytes) {
        int sz = insn_length(code + total);
        if (sz == 0 || total + sz > 32) return 0;
        total += sz;
    }
    return total;
}

struct FnHook {
    uint8_t* target      = nullptr;
    uint8_t* trampoline  = nullptr;
    uint8_t  saved[32]   = {};
    size_t   saved_sz    = 0;

    bool install(void* fn, void* replacement) {
        target = reinterpret_cast<uint8_t*>(fn);
        saved_sz = safe_prologue_size(target, 14);
        if (saved_sz == 0) return false;

        trampoline = reinterpret_cast<uint8_t*>(VirtualAlloc(
            nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!trampoline) return false;

        memcpy(saved, target, saved_sz);
        memcpy(trampoline, target, saved_sz);

        // trampoline tail — jmp qword ptr [rip+0]; <addr of target+saved_sz>
        uint8_t* jb = trampoline + saved_sz;
        jb[0] = 0xFF; jb[1] = 0x25;
        *reinterpret_cast<uint32_t*>(jb + 2) = 0;
        *reinterpret_cast<uint64_t*>(jb + 6) = reinterpret_cast<uint64_t>(target + saved_sz);

        DWORD op = 0;
        if (!VirtualProtect(target, saved_sz, PAGE_EXECUTE_READWRITE, &op)) return false;
        target[0] = 0xFF; target[1] = 0x25;
        *reinterpret_cast<uint32_t*>(target + 2) = 0;
        *reinterpret_cast<uint64_t*>(target + 6) = reinterpret_cast<uint64_t>(replacement);
        for (size_t i = 14; i < saved_sz; ++i) target[i] = 0x90;
        VirtualProtect(target, saved_sz, op, &op);
        FlushInstructionCache(GetCurrentProcess(), target, saved_sz);
        return true;
    }

    void uninstall() {
        if (!target || saved_sz == 0) return;
        DWORD op;
        VirtualProtect(target, saved_sz, PAGE_EXECUTE_READWRITE, &op);
        memcpy(target, saved, saved_sz);
        VirtualProtect(target, saved_sz, op, &op);
        FlushInstructionCache(GetCurrentProcess(), target, saved_sz);
        if (trampoline) VirtualFree(trampoline, 0, MEM_RELEASE);
        target = nullptr; trampoline = nullptr; saved_sz = 0;
    }

    template <typename Fn>
    Fn get_original() const { return reinterpret_cast<Fn>(trampoline); }
};

static FnHook g_hook_present;
static FnHook g_hook_resize_buffers;

// DX11 objects captured from the game's swap chain on the first Present.
// We AddRef via GetDevice / GetImmediateContext, so we own these refs and
// must Release them on uninstall.
static ID3D11Device*           g_dev = nullptr;
static ID3D11DeviceContext*    g_ctx = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;

// ─── SetCursorPos byte-patch (no disassembler needed) ─────────────────
// user32!SetCursorPos on Win10 starts with a `call rel32` which my minimal
// disassembler can't relocate. Instead of a trampoline hook, we swap the
// first 6 bytes with `mov eax, 1; ret` on menu-open and restore them on
// close. Byte 0 is written last / first-out so any thread mid-call either
// sees the original opcode or our stub — never a torn instruction.
static uint8_t*  g_scp_addr        = nullptr;
static uint8_t   g_scp_saved[6]    = {};
static bool      g_scp_patched     = false;
static const uint8_t g_scp_ret_true[6] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };

static void patch_setcursorpos_noop() {
    if (!g_scp_addr || g_scp_patched) return;
    DWORD op;
    if (!VirtualProtect(g_scp_addr, 6, PAGE_EXECUTE_READWRITE, &op)) return;
    memcpy(g_scp_addr + 1, g_scp_ret_true + 1, 5);
    *g_scp_addr = g_scp_ret_true[0];
    VirtualProtect(g_scp_addr, 6, op, &op);
    FlushInstructionCache(GetCurrentProcess(), g_scp_addr, 6);
    g_scp_patched = true;
}

static void unpatch_setcursorpos() {
    if (!g_scp_addr || !g_scp_patched) return;
    DWORD op;
    if (!VirtualProtect(g_scp_addr, 6, PAGE_EXECUTE_READWRITE, &op)) return;
    *g_scp_addr = g_scp_saved[0];
    memcpy(g_scp_addr + 1, g_scp_saved + 1, 5);
    VirtualProtect(g_scp_addr, 6, op, &op);
    FlushInstructionCache(GetCurrentProcess(), g_scp_addr, 6);
    g_scp_patched = false;
}

static void*             g_client        = nullptr;
static void*             g_input         = nullptr;
static void*             g_engine        = nullptr;
static void*             g_ent_list      = nullptr;
static intptr_t          g_origin_offset = -1;
static void**            g_client_vtable = nullptr;
static CreateMove_t      o_createmove    = nullptr;

static HWND              g_hwnd          = nullptr;
static bool              g_imgui_up      = false;

static int               g_endscene_calls   = 0;
static int               g_createmove_calls = 0;
static bool              g_ready            = false;

// ─── vtable slot swap ─────────────────────────────────────────────────
static void* swap_slot(void** vt, int idx, void* new_fn) {
    DWORD old_prot = 0;
    VirtualProtect(&vt[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &old_prot);
    void* orig = vt[idx];
    vt[idx] = new_fn;
    VirtualProtect(&vt[idx], sizeof(void*), old_prot, &old_prot);
    return orig;
}

// ─── interface loader with per-attempt logging ────────────────────────
using CreateInterfaceFn = void*(*)(const char*, int*);

static void* try_iface(const char* mod, const char* name) {
    HMODULE m = GetModuleHandleA(mod);
    if (!m) return nullptr;
    auto ci = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(m, "CreateInterface"));
    if (!ci) return nullptr;
    int ret = 0;
    return ci(name, &ret);
}

// Sweep versions from `hi` down to `lo` and log each attempt. Returns the
// highest-version pointer that resolves.
static void* sweep(const char* mod, const char* prefix, int lo, int hi) {
    for (int v = hi; v >= lo; --v) {
        char name[64];
        std::snprintf(name, sizeof(name), "%s%03d", prefix, v);
        void* p = try_iface(mod, name);
        log("  try %-24s in %-12s -> %p", name, mod, p);
        if (p) return p;
    }
    return nullptr;
}

// ─── sig-scan fallback for g_pInput ───────────────────────────────────
// SDK 2013 MP: CHLClient::IN_ActivateMouse (client vtable slot 15) is
// literally `input->ActivateMouse()`. Its first RIP-relative `mov` loads
// the global `input` pointer. We decode that to recover g_pInput.
static bool region_readable(const void* p, size_t sz) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD prot = mbi.Protect & 0xFF;
    if (prot == 0 || prot == PAGE_NOACCESS) return false;
    if (mbi.Protect & PAGE_GUARD) return false;
    uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return reinterpret_cast<uintptr_t>(p) + sz <= end;
}

// Return true if the given vtable's first `n` slots all look like valid
// function pointers in executable memory. Rules out non-vtable coincidences.
static bool vtable_looks_valid(void** vt, int n = 12) {
    if (!region_readable(vt, n * sizeof(void*))) return false;
    for (int i = 0; i < n; ++i) {
        void* fn = vt[i];
        if (!fn) return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(fn, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        DWORD prot = mbi.Protect & 0xFF;
        // must be executable
        bool executable = (prot == PAGE_EXECUTE) || (prot == PAGE_EXECUTE_READ)
                       || (prot == PAGE_EXECUTE_READWRITE) || (prot == PAGE_EXECUTE_WRITECOPY);
        if (!executable) return false;
    }
    return true;
}

static void* extract_rip_relative_global(void* fn_ptr) {
    auto* fn = reinterpret_cast<uint8_t*>(fn_ptr);
    if (!region_readable(fn, 64)) return nullptr;

    // if the function is a JMP thunk (E9 rel32), follow it
    if (fn[0] == 0xE9) {
        int32_t rel = *reinterpret_cast<int32_t*>(fn + 1);
        fn = fn + 5 + rel;
        if (!region_readable(fn, 64)) return nullptr;
    }

    // scan for: 48 8B ?? XX XX XX XX     (mov r64, [rip+disp32])
    // modrm high bits mod=00, rm=101 → low nibble is 0x05, reg-field varies (0x05/0D/15/1D/25/2D/35/3D)
    for (int i = 0; i < 48; ++i) {
        if (fn[i] != 0x48 || fn[i + 1] != 0x8B) continue;
        uint8_t modrm = fn[i + 2];
        if ((modrm & 0xC7) != 0x05) continue;

        int32_t rel = *reinterpret_cast<int32_t*>(fn + i + 3);
        void** ref = reinterpret_cast<void**>(fn + i + 7 + rel);
        if (!region_readable(ref, sizeof(void*))) continue;

        void* candidate = *ref;
        if (!candidate || !region_readable(candidate, 16)) continue;

        void** vt = *reinterpret_cast<void***>(candidate);
        if (!vt || !vtable_looks_valid(vt, 12)) continue;

        return candidate;
    }
    return nullptr;
}

static void* find_input_via_sigscan(void** client_vtable) {
    // slots that pass straight through to `input->something()` in SDK 2013 MP.
    // 15 = IN_ActivateMouse, 16 = IN_DeactivateMouse, 17 = IN_Accumulate,
    // 18 = IN_ClearStates, 19 = IN_IsKeyDown, 20 = IN_KeyEvent.
    const int slots[] = { 15, 16, 17, 18, 19, 20 };
    for (int slot : slots) {
        void* fn = client_vtable[slot];
        void* p  = extract_rip_relative_global(fn);
        log("  scan client vtable[%d] @ %p  -> g_pInput candidate %p", slot, fn, p);
        if (p) return p;
    }
    return nullptr;
}

// ─── window / device helpers ──────────────────────────────────────────
static BOOL CALLBACK enum_wnd_cb(HWND h, LPARAM p) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || GetWindow(h, GW_OWNER) || !IsWindowVisible(h))
        return TRUE;
    *reinterpret_cast<HWND*>(p) = h;
    return FALSE;
}
static HWND find_game_window() {
    HWND h = nullptr;
    EnumWindows(enum_wnd_cb, reinterpret_cast<LPARAM>(&h));
    return h;
}

static bool grab_swapchain_vtable() {
    // Momentum renders through Strata Source's shaderapidx11 backend, so
    // the game creates one or more IDXGISwapChain instances. We spin up a
    // throwaway swap chain of our own just long enough to snapshot its
    // vtable — Present + ResizeBuffers live in dxgi.dll's code section
    // and are shared across every swap chain in the process, so hooking
    // them once catches the game's future swap chains too.
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount        = 1;
    scd.BufferDesc.Format  = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow       = GetDesktopWindow();
    scd.SampleDesc.Count   = 1;
    scd.Windowed           = TRUE;
    scd.SwapEffect         = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    IDXGISwapChain*      swap = nullptr;
    ID3D11Device*        dev  = nullptr;
    ID3D11DeviceContext* ctx  = nullptr;
    D3D_FEATURE_LEVEL    got  = D3D_FEATURE_LEVEL_11_0;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        fls, ARRAYSIZE(fls), D3D11_SDK_VERSION,
        &scd, &swap, &dev, &got, &ctx);
    if (FAILED(hr) || !swap) {
        log("D3D11CreateDeviceAndSwapChain failed: 0x%08X", hr);
        return false;
    }

    g_swapchain_vt = *reinterpret_cast<void***>(swap);
    log("IDXGISwapChain vtable @ %p (feature level 0x%X)",
        (void*)g_swapchain_vt, got);

    if (ctx)  ctx->Release();
    if (dev)  dev->Release();
    if (swap) swap->Release();
    return g_swapchain_vt != nullptr;
}

// ─── hooks ────────────────────────────────────────────────────────────
// Forward decls — scanner defined later in the file.
static void scan_for_pcommands(void* input_this);
static intptr_t g_pcommands_real_offset = -1;

// Access m_pCommands directly using the offset the scanner discovered.
// Ring geometry measured from live GetUserCmd return addresses 2026-07-23:
// 150 entries, 120-byte stride (Momentum's extended CUserCmd).
static constexpr size_t MP_BACKUP   = 150;
static constexpr size_t CMD_STRIDE  = 120;

static CUserCmd* raw_get_cmd(int seq_num) {
    if (!g_input || g_pcommands_real_offset < 0) return nullptr;
    CUserCmd** ppCommands = reinterpret_cast<CUserCmd**>(
        reinterpret_cast<uint8_t*>(g_input) + g_pcommands_real_offset);
    if (!region_readable(ppCommands, sizeof(void*))) return nullptr;
    CUserCmd* base = *ppCommands;
    if (!base) return nullptr;
    int idx = ((seq_num % static_cast<int>(MP_BACKUP)) + MP_BACKUP) % MP_BACKUP;
    return reinterpret_cast<CUserCmd*>(
        reinterpret_cast<uint8_t*>(base) + idx * CMD_STRIDE);
}

namespace hooks {
    void bump_cache_age();
    void maybe_reset_caches();
    using EngineClientCmd_t = void(*)(void*, const char*);
    extern EngineClientCmd_t o_engine_client_cmd;
}

static void safe_process_cmd(int seq_num) {
    __try {
        if (!g_input) return;
        hooks::bump_cache_age();
        hooks::maybe_reset_caches();
        // Lazy scan on first call once the game has populated cmd data.
        scan_for_pcommands(g_input);
        // Also keep the origin scanner ticking so it's ready by RECORD-time.
        {
            hooks::Vec3 dummy;
            hooks::get_local_player_pos(dummy);
        }
        CUserCmd* cmd = raw_get_cmd(seq_num);
        if (!cmd) return;
        // Sanity: this tick's cmd->command_number should equal seq_num.
        // If it doesn't, our layout is off — bail so we don't corrupt data.
        if (cmd->command_number != seq_num) return;
        recorder::on_createmove(cmd);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        // swallow — try again next tick.
    }
}

// Signature matches SDK 2013 MP: void CreateMove(int, float, bool)
// Both this and hk_cm_createmove call recorder::on_createmove(cmd). The
// recorder dedupes on command_number so whichever fires first wins.
static void __fastcall hk_createmove(void* self, int seq_num, float sample_time, bool active) {
    o_createmove(self, seq_num, sample_time, active);
    ++g_createmove_calls;
    if (!active || !g_input) return;
    safe_process_cmd(seq_num);
}

// IClientMode::CreateMove(float ft, CUserCmd* cmd) — inline-hooked by
// function address (not vtable slot) so we catch direct calls too — the
// compiler devirtualizes ClientModeShared::CreateMove calls.
using CM_CreateMove_t = bool(*)(void*, float, CUserCmd*);
static void*  g_pClientMode      = nullptr;
static void** g_cm_vtable        = nullptr;
static int    g_cm_createmove_calls = 0;
static FnHook g_hook_cm_createmove;
static CM_CreateMove_t o_cm_createmove_fn = nullptr;

// ─── verified-cmd bypass (Emily's css-tas approach) ─────────────────
// Source SDK 2013 MP servers reject modified cmds via a checksum check
// inside WriteUsercmdDeltaToBuffer: if verified.m_crc != cur.GetChecksum()
// the server silently reverts cur back to the saved verified.m_cmd copy,
// so our modifications never reach it. Fix: sync verified.m_cmd + m_crc
// to the modified cmd BEFORE the check runs, by hooking IInput slot 5.
// Momentum runs single-player servers locally, so the check MAY be a
// no-op there — keeping the bypass anyway; it's harmless when unused.

struct CVerifiedUserCmd {
    CUserCmd  m_cmd;
    uint32_t  m_crc;
};

static uint32_t g_crc32_table[256] = {};
static bool     g_crc32_ready      = false;

static void crc32_init() {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int j = 0; j < 8; ++j)
            c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
        g_crc32_table[i] = c;
    }
    g_crc32_ready = true;
}

static uint32_t crc32_step(uint32_t crc, const void* data, size_t len) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
    while (len--) crc = (crc >> 8) ^ g_crc32_table[(crc ^ *p++) & 0xFF];
    return crc;
}

// (cmd_checksum removed — dead code from the abandoned CRC-bypass path;
// it also modeled classic-2013 fields that don't exist on Momentum.)

static intptr_t g_pcommands_offset  = -1;
static intptr_t g_pverified_offset  = -1;

static void dump_input_vtable() {
    if (!g_input) return;
    void** vt = *reinterpret_cast<void***>(g_input);
    if (!region_readable(vt, 24 * sizeof(void*))) return;
    log("IInput vtable dump (looking for GetUserCmd — should load m_pCommands via `mov rXX, [rcx+off]`):");
    for (int slot = 0; slot < 24; ++slot) {
        void* fn = vt[slot];
        if (!fn || !region_readable(fn, 24)) { log("  iv[%2d] = null/bad", slot); continue; }
        uint8_t* p = reinterpret_cast<uint8_t*>(fn);
        char hex[64] = "";
        for (int i = 0; i < 20; ++i) {
            char b[4]; std::snprintf(b, sizeof(b), "%02X ", p[i]);
            strcat(hex, b);
        }
        log("  iv[%2d] @ %p  bytes: %s", slot, fn, hex);
    }
}

// Read-only dump of IVEngineClient's vtable so we can identify the real
// ClientCmd / ServerCmd / ExecuteClientCmd slots by disassembly. On this
// Strata build the 2013-MP slot numbers are shifted (slot 7 is NOT ClientCmd
// — our interceptor there never once fired on user-typed commands). Never
// calls anything; just prints addresses + prologue bytes + module offset.
static void dump_engine_vtable() {
    if (!g_engine) return;
    void** vt = *reinterpret_cast<void***>(g_engine);
    if (!region_readable(vt, 48 * sizeof(void*))) return;
    uint8_t* eng_base = nullptr; size_t eng_size = 0;
    HMODULE em = GetModuleHandleA("engine.dll");
    if (em) eng_base = reinterpret_cast<uint8_t*>(em);
    log("IVEngineClient vtable dump (hunting ClientCmd/ServerCmd — (this, const char*) sinks):");
    for (int slot = 0; slot < 48; ++slot) {
        void* fn = vt[slot];
        if (!fn || !region_readable(fn, 24)) { log("  ev[%2d] = null/bad", slot); continue; }
        uint8_t* p = reinterpret_cast<uint8_t*>(fn);
        char hex[80] = "";
        for (int i = 0; i < 24; ++i) {
            char b[4]; std::snprintf(b, sizeof(b), "%02X ", p[i]);
            strcat(hex, b);
        }
        long long off = eng_base ? (long long)(p - eng_base) : -1;
        // Follow `add rcx,8; jmp rel32` adjustor thunks to the real impl and
        // dump ITS bytes — SetViewAngles(QAngle&) copies 12 bytes from [rdx]
        // to a rip-relative global; GetViewAngles copies the other direction.
        char thex[96] = "";
        if (p[0]==0x48 && p[1]==0x83 && p[2]==0xC1 && p[3]==0x08 && p[4]==0xE9) {
            int32_t rel = *reinterpret_cast<int32_t*>(p + 5);
            uint8_t* tgt = p + 9 + rel;
            if (region_readable(tgt, 28)) {
                for (int i = 0; i < 28; ++i) {
                    char b[4]; std::snprintf(b, sizeof(b), "%02X ", tgt[i]);
                    strcat(thex, b);
                }
                long long toff = eng_base ? (long long)(tgt - eng_base) : -1;
                log("  ev[%2d] @ engine+0x%llX  THUNK->engine+0x%llX  tgt: %s",
                    slot, off, toff, thex);
                continue;
            }
        }
        log("  ev[%2d] @ %p  engine+0x%llX  bytes: %s", slot, fn, off, hex);
    }
}

// Scan a function's prologue for ANY `mov r64, [rcx + disp]` — any dest reg,
// disp8 OR disp32. That's what CInput::GetUserCmd starts with.
static intptr_t scan_rcx_load_offset(uint8_t* fn) {
    if (!region_readable(fn, 64)) return INT64_MIN;
    for (int i = 0; i < 48; ++i) {
        // REX prefix range 48-4F, then 8B (mov r64, r/m64), modrm with rm=1 (rcx)
        // and mod=1 (disp8) or mod=2 (disp32). reg field varies.
        if ((fn[i] & 0xF8) != 0x48) continue;
        if (fn[i+1] != 0x8B) continue;
        uint8_t modrm = fn[i+2];
        uint8_t mod   = modrm >> 6;
        uint8_t rm    = modrm & 0x07;
        if (rm != 1) continue;                    // not [rcx + ...]
        if (mod == 1) return static_cast<int8_t>(fn[i+3]);
        if (mod == 2) return *reinterpret_cast<int32_t*>(fn + i + 3);
    }
    return INT64_MIN;
}

// Scan CInput's memory looking for the actual m_pCommands pointer. It has
// two signatures: (1) its target's first qword is a code-region vtable
// pointer, and (2) command_number at offset 8 is a small positive integer.
// Called on the FIRST WriteDelta invocation once the array is populated.
static intptr_t g_pverified_real_offset = -1;

// Verified cmd array signature: each element is CVerifiedUserCmd { CUserCmd m_cmd; uint32_t m_crc; }
// Same head as CUserCmd (vtable ptr + command_number/tick_count), so it'll
// also match the m_pCommands scan. We pick m_pVerifiedCommands as the
// second matching pointer (right after m_pCommands in the CInput layout).
static void scan_for_pcommands(void* input_this) {
    if (g_pcommands_real_offset >= 0 && g_pverified_real_offset >= 0) return;
    if (!input_this) return;
    uint8_t* base = reinterpret_cast<uint8_t*>(input_this);

    for (intptr_t off = 0x08; off < 0x400; off += 8) {
        if (!region_readable(base + off, 8)) continue;
        uint8_t* ptr = *reinterpret_cast<uint8_t**>(base + off);
        if (!ptr || !region_readable(ptr, 128)) continue;

        uintptr_t vt = *reinterpret_cast<uintptr_t*>(ptr);
        if (vt < 0x00007FF000000000ULL || vt > 0x00007FFFFFFFFFFFULL) continue;

        int cn = *reinterpret_cast<int*>(ptr + 8);
        if (cn < 0 || cn > 500000) continue;
        int tick = *reinterpret_cast<int*>(ptr + 12);
        if (tick < 0 || tick > 500000) continue;

        if (g_pcommands_real_offset < 0) {
            log("SCAN: m_pCommands @ CInput+0x%llX -> %p  cn=%d tick=%d",
                (long long)off, (void*)ptr, cn, tick);
            g_pcommands_real_offset = off;
            continue; // keep looking for the verified array
        }

        // Second matching pointer — likely m_pVerifiedCommands.
        // Its element stride is sizeof(CUserCmd) + sizeof(uint32_t) = 76 bytes.
        // Sanity check: skip if it's the same pointer we already found.
        if (off == g_pcommands_real_offset) continue;

        log("SCAN: m_pVerifiedCommands @ CInput+0x%llX -> %p  cn=%d tick=%d",
            (long long)off, (void*)ptr, cn, tick);
        g_pverified_real_offset = off;
        return;
    }
    if (g_pcommands_real_offset < 0)
        log("SCAN: no m_pCommands candidate found in CInput+[8..0x400]");
    if (g_pverified_real_offset < 0)
        log("SCAN: m_pVerifiedCommands not found — CRC bypass unavailable");
}

static void find_input_data_offsets() {
    if (g_pcommands_offset >= 0 || !g_input) return;
    void** vt = *reinterpret_cast<void***>(g_input);
    if (!region_readable(vt, 16 * sizeof(void*))) return;

    // Try slots 6..14 — GetUserCmd could have shifted in the 2025 rebuild.
    for (int slot = 6; slot <= 14; ++slot) {
        uint8_t* fn = reinterpret_cast<uint8_t*>(vt[slot]);
        intptr_t off = scan_rcx_load_offset(fn);
        if (off > 0 && off < 0x10000) {
            g_pcommands_offset = off;
            g_pverified_offset = off + 8;
            log("m_pCommands offset found via IInput slot %d: 0x%llX",
                slot, (long long)off);
            return;
        }
    }
    log("failed to decode m_pCommands offset — vtable dump above shows prologues");
}

// Hook IInput::WriteUsercmdDeltaToBuffer — slot 5.
using WriteDelta_t = bool(*)(void*, void*, int, int, bool);
static WriteDelta_t o_write_delta       = nullptr;
static int          g_write_delta_calls = 0;

static bool __fastcall hk_write_delta(void* self, void* buf, int from, int to, bool isnew) {
    ++g_write_delta_calls;

    __try {
        // First call: scan CInput's memory for the real m_pCommands pointer.
        scan_for_pcommands(self);
        if (g_pcommands_real_offset < 0) goto done;

        CUserCmd** ppCmds = reinterpret_cast<CUserCmd**>(
            reinterpret_cast<uint8_t*>(self) + g_pcommands_real_offset);
        if (!region_readable(ppCmds, sizeof(void*))) goto done;

        CUserCmd* base = *ppCmds;
        if (!base) goto done;

        int idx = ((to % 90) + 90) % 90;
        CUserCmd* cmd = reinterpret_cast<CUserCmd*>(
            reinterpret_cast<uint8_t*>(base) + idx * sizeof(CUserCmd));

        if (!region_readable(cmd, sizeof(CUserCmd))) goto done;

        // Per-tick capture path — WriteDelta fires reliably every tick on
        // Momentum (verified via earlier session logs). Read the cmd out
        // of m_pCommands at slot `to`. Validate cmd->command_number matches
        // `to` before trusting it: if stride is wrong, cn won't line up.
        // Momentum's client CUserCmd is essentially the CS:S layout (its
        // source-sdk-2013 heritage — server_random_seed is server-only),
        // so sizeof(CUserCmd)=72 should still be right.
        static int diag = 8;
        if (diag > 0) {
            --diag;
            log("WriteDelta to=%d idx=%d cmd@%p cn=%d tick=%d fw=%.1f yaw=%.1f",
                to, idx, cmd, cmd->command_number, cmd->tick_count,
                cmd->forwardmove, cmd->viewangles.yaw);
        }

        // Feed the recorder only when the cmd we read looks real.
        // command_number == to is the tell that stride+base are correct.
        if (cmd->command_number == to) {
            recorder::on_createmove(cmd);
        }

        // The CS:S CRC-bypass memcpy that used to live here has been
        // removed for Momentum — it clobbered the verified-cmd vtable
        // pointer with random memory (crash on next virtual call) AND
        // Momentum's saveloc is client-authoritative anyway, so there
        // is no server-side revert to defeat.

    done: ;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    return o_write_delta(self, buf, from, to, isnew);
}

static bool __fastcall hk_cm_createmove_inline(void* self, float ft, CUserCmd* cmd) {
    ++g_cm_createmove_calls;
    bool ret = true;
    __try {
        ret = o_cm_createmove_fn(self, ft, cmd);
        if (cmd) recorder::on_createmove(cmd);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // wrong slot / signature — swallow
    }
    return ret;
}

// Mirror the (possibly recorder-modified) cmd into the engine's SEND array —
// the m_pCommands member at CInput+g_pverified_real_offset (the second
// pointer the scanner finds; GetUserCmd's array is the first). Stride and
// ring modulus are unknown on this closed-source build, so both are
// detected at runtime: an entry qualifies only if its command_number field
// (+8, after the vptr) equals the current seq — a 32-bit check tied to the
// ring position, so a wrong (modulus, stride) pair can't false-positive
// twice in a row on different seqs. No detection → no writes → no risk.
static void mirror_cmd_to_send_array(int seq_num, const CUserCmd* cmd) {
    if (!g_input || g_pverified_real_offset < 0) return;

    uint8_t** ppArr = reinterpret_cast<uint8_t**>(
        reinterpret_cast<uint8_t*>(g_input) + g_pverified_real_offset);
    if (!region_readable(ppArr, sizeof(void*))) return;
    uint8_t* base = *ppArr;
    if (!base) return;

    static int  s_mod = 0, s_stride = 0;   // cached once proven
    static int  s_cand_mod = 0, s_cand_stride = 0, s_cand_hits = 0;
    static bool s_gave_up = false;
    if (s_gave_up) return;

    if (!s_mod) {
        static const int MODS[]    = { 150, 90, 128, 182 };
        static const int STRIDES[] = { 64, 72, 80, 88, 96, 104, 112, 116, 120, 128 };
        int fm = 0, fs = 0;
        for (int m : MODS) {
            int idx = seq_num % m;
            for (int s : STRIDES) {
                uint8_t* e = base + static_cast<size_t>(idx) * s;
                if (!region_readable(e, 72)) continue;
                if (*reinterpret_cast<const int*>(e + 8) == seq_num) { fm = m; fs = s; break; }
            }
            if (fm) break;
        }
        if (fm && fm == s_cand_mod && fs == s_cand_stride) {
            if (++s_cand_hits >= 2) {
                s_mod = fm; s_stride = fs;
                log("send-array geometry resolved: modulus=%d stride=%d (base member CInput+0x%llX)",
                    s_mod, s_stride, (long long)g_pverified_real_offset);
            }
        } else {
            s_cand_mod = fm; s_cand_stride = fs; s_cand_hits = fm ? 1 : 0;
            // ~10 s of ticks with no consistent geometry — stop scanning.
            static int s_misses = 0;
            if (!fm && ++s_misses > 660) {
                s_gave_up = true;
                log("send-array geometry NOT found — playback writes will not reach the engine");
            }
        }
        if (!s_mod) return;
    }

    uint8_t* entry = base + static_cast<size_t>(seq_num % s_mod) * s_stride;
    if (!region_readable(entry, 72)) return;
    // The engine's own CreateMove stamped this entry's command_number this
    // tick; if it doesn't match, geometry is stale (map change?) — re-detect.
    if (*reinterpret_cast<const int*>(entry + 8) != seq_num) {
        s_mod = 0; s_cand_hits = 0;
        return;
    }
    // Copy bytes 8..71: cn, tick, viewangles, moves, buttons, impulse,
    // weapon, seed, mouse. Skip the vptr (entry keeps its own) and any
    // Momentum-extended fields past the classic head (engine keeps those).
    std::memcpy(entry + 8, reinterpret_cast<const uint8_t*>(cmd) + 8, 64);
}

// ─── viewangle memory location (read-only discovery) ──────────────────
// The vtable-sweep approach was REMOVED — blind-calling unknown engine
// functions with fabricated arguments crashed the game. This replacement
// NEVER calls a function; it scans MEMORY for a QAngle that tracks the live
// view over time, then writes it during playback. Pure read to find, pure
// write to apply — nothing to crash.
//
// TARGET: the engine's own view-angle global (`cl.viewangles` in
// engine.dll). The FIRST attempt scanned the player ENTITY and found the
// networked eye-angle field — that drives movement + the server (which is
// why drift is already 0.0), but NOT the on-screen first-person camera.
// The camera reads cl.viewangles, an engine global, so we scan engine.dll's
// writable data for it instead.
static QAngle g_last_cmd_angles{};
static int    g_last_cmd_angles_seq = -1;
// The first-person CAMERA and the engine's movement/server viewangles are
// SEPARATE copies on this build: writing engine.dll's cl.viewangles moved the
// server angle (drift 0) but not the on-screen camera, which renders from a
// DIFFERENT global (almost certainly in client.dll). So we scan BOTH modules
// and write EVERY confirmed view-tracking global — whichever one the renderer
// reads, we cover it. The two-pass "moved AND still matches" test makes false
// positives (coincidental angle-shaped data) essentially impossible.
static hooks::QAngle3 g_last_set_va{};       // last angle handed to set_view_angles
static bool           g_have_set_va = false;

struct VACandidate { float* ptr; float p, y; };
static std::vector<VACandidate> g_va_candidates;
static std::vector<float*>      g_view_angle_ptrs;   // all confirmed globals
static bool g_va_scanned    = false;
static bool g_va_first_view = false;                 // captured a distinctive view yet

// The first-person camera renders from the local player ENTITY's view angle
// (v_angle / eye angles), which is a heap object the module scan can't reach.
// So we ALSO scan the entity by OFFSET (the entity address changes each spawn,
// the offset doesn't) and re-resolve+write entity+offset every frame.
struct EntCandidate { intptr_t off; float p, y; };
static std::vector<EntCandidate> g_ent_candidates;
static std::vector<intptr_t>     g_ent_view_offs;    // confirmed offsets into the player entity
static bool g_ent_scanned = false;

namespace hooks { void* resolve_local_player_bridge(); }
static inline void* hooks_resolve_local_player() {
    return hooks::resolve_local_player_bridge();
}

static bool get_module_range(const char* name, uint8_t** base, size_t* size) {
    HMODULE m = GetModuleHandleA(name);
    if (!m) return false;
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(m);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<uint8_t*>(m) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    *base = reinterpret_cast<uint8_t*>(m);
    *size = nt->OptionalHeader.SizeOfImage;
    return true;
}

static void scan_module_for_view(const char* mod, const QAngle& live) {
    uint8_t* base; size_t size;
    if (!get_module_range(mod, &base, &size)) return;
    uint8_t* p   = base;
    uint8_t* end = base + size;
    while (p < end && g_va_candidates.size() < 8192) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) break;
        uint8_t* rbeg = reinterpret_cast<uint8_t*>(mbi.BaseAddress);
        uint8_t* rend = rbeg + mbi.RegionSize;
        DWORD prot = mbi.Protect & 0xFF;
        bool writable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
            (prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
             prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY);
        if (writable) {
            if (rbeg < base) rbeg = base;
            for (uint8_t* q = rbeg; q + 12 <= rend; q += 4) {
                float pi = *reinterpret_cast<float*>(q);
                float ya = *reinterpret_cast<float*>(q + 4);
                float ro = *reinterpret_cast<float*>(q + 8);
                if (!(pi == pi) || std::fabs(pi) > 89.f) continue;
                if (!(ya == ya) || std::fabs(ya) > 180.f) continue;
                if (!(ro == ro) || std::fabs(ro) > 1.f) continue;   // roll ~ 0
                if (std::fabs(std::remainderf(pi - live.pitch, 360.f)) > 1.f) continue;
                if (std::fabs(std::remainderf(ya - live.yaw,   360.f)) > 1.f) continue;
                g_va_candidates.push_back({ reinterpret_cast<float*>(q), pi, ya });
                if (g_va_candidates.size() >= 8192) break;
            }
        }
        p = rend;
    }
}

// First call (distinctive view): sweep engine.dll AND client.dll writable
// regions for every QAngle matching the live view. Later calls (view has
// changed): keep ALL that MOVED with the view AND still match — those are the
// real view-angle globals (engine cl.viewangles + the client render copy).
static QAngle  g_va_ref_view{};    // view captured at first-pass scan time
static bool    g_va_have_ref = false;

static void locate_view_angle_mem(const QAngle& live) {
    if (!g_engine) return;
    bool modules_done = !g_view_angle_ptrs.empty() || (g_va_scanned && g_va_candidates.empty());
    bool entity_done  = !g_ent_view_offs.empty() || (g_ent_scanned && g_ent_candidates.empty());
    if (modules_done && entity_done) return;
    if (std::fabs(live.pitch) < 3.f && std::fabs(live.yaw) < 3.f) return; // distinctive

    // Throttle by seq only (no hard attempt cap). The confirm pass below is
    // gated on the view having actually MOVED from the first-pass reference,
    // so standing still never burns the scan — it locks whenever you look
    // around, however long that takes.
    static int next_seq = 0;
    if (g_last_cmd_angles_seq < next_seq) return;
    next_seq = g_last_cmd_angles_seq + 20;

    // Once modules have been scanned, only run the confirm passes after the
    // live view has swung at least 5° from where the first pass captured it —
    // that swing is exactly what separates the real trackers from coincidences.
    auto angle_moved = [](const QAngle& a, const QAngle& b) {
        float dp = std::fabs(a.pitch - b.pitch);
        float dy = std::fabs(std::remainderf(a.yaw - b.yaw, 360.f));
        return dp > 5.f || dy > 5.f;
    };
    if (g_va_have_ref && !angle_moved(live, g_va_ref_view)) return;

    auto near_ang = [](float a, float b) {
        return std::fabs(std::remainderf(a - b, 360.f)) < 1.0f;
    };

    // ── entity scan (heap object, by offset) ──────────────────────────
    void* ent = hooks_resolve_local_player();
    if (ent && !g_ent_view_offs.empty()) { /* done */ }
    else if (ent && !g_ent_scanned) {
        uint8_t* eb = reinterpret_cast<uint8_t*>(ent);
        // Two layouts: (pitch,yaw) adjacent AND (yaw,pitch) adjacent — Source
        // stores eye angles both ways in different fields. Wide range: player
        // entities run large. roll ~0 filter dropped (some copies pack it).
        for (intptr_t off = 0x40; off + 8 < 0x6000; off += 4) {
            if (!region_readable(eb + off, 8)) continue;
            float a = *reinterpret_cast<float*>(eb + off);
            float b = *reinterpret_cast<float*>(eb + off + 4);
            if (!(a==a) || !(b==b)) continue;
            // (pitch, yaw)
            if (std::fabs(a) <= 89.f && std::fabs(b) <= 180.f &&
                near_ang(a, live.pitch) && near_ang(b, live.yaw))
                g_ent_candidates.push_back({ off, a, b });
        }
        g_ent_scanned = true;
        log("viewangle entity scan: %zu candidates (%.1f/%.1f)",
            g_ent_candidates.size(), live.pitch, live.yaw);
    } else if (ent && g_ent_scanned && g_ent_view_offs.empty()) {
        uint8_t* eb = reinterpret_cast<uint8_t*>(ent);
        for (auto& c : g_ent_candidates) {
            if (!region_readable(eb + c.off, 12)) continue;
            float p = *reinterpret_cast<float*>(eb + c.off);
            float y = *reinterpret_cast<float*>(eb + c.off + 4);
            bool moved   = std::fabs(p - c.p) > 0.5f || std::fabs(y - c.y) > 0.5f;
            bool matches = near_ang(p, live.pitch) && near_ang(y, live.yaw);
            if (moved && matches) g_ent_view_offs.push_back(c.off);
        }
        for (intptr_t off : g_ent_view_offs)
            log("viewangle entity CONFIRMED @ player+0x%llX", (long long)off);
        if (!g_ent_view_offs.empty()) { g_ent_candidates.clear(); g_ent_candidates.shrink_to_fit(); }
    }

    // ── module scan (engine.dll + client.dll globals) ─────────────────
    if (!g_va_scanned) {
        scan_module_for_view("engine.dll", live);
        scan_module_for_view("client.dll", live);
        g_va_scanned    = true;
        g_va_ref_view   = live;   // baseline for the "view has moved" gate
        g_va_have_ref   = true;
        log("viewangle scan (engine+client): %zu candidates matching view (%.1f/%.1f) — "
            "now look around to lock", g_va_candidates.size(), live.pitch, live.yaw);
        return;
    }
    if (!g_view_angle_ptrs.empty()) return;

    uint8_t *eb=nullptr,*cb=nullptr; size_t es,cs;
    get_module_range("engine.dll", &eb, &es);
    get_module_range("client.dll", &cb, &cs);
    for (auto& c : g_va_candidates) {
        if (!region_readable(c.ptr, 12)) continue;
        float p = c.ptr[0], y = c.ptr[1];
        bool moved   = std::fabs(p - c.p) > 0.5f || std::fabs(y - c.y) > 0.5f;
        bool matches = near_ang(p, live.pitch) && near_ang(y, live.yaw);
        if (moved && matches) g_view_angle_ptrs.push_back(c.ptr);
    }
    if (!g_view_angle_ptrs.empty()) {
        for (float* ptr : g_view_angle_ptrs) {
            uint8_t* u = reinterpret_cast<uint8_t*>(ptr);
            const char* mod = (u >= cb && u < cb + cs) ? "client.dll" :
                              (u >= eb && u < eb + es) ? "engine.dll" : "?";
            uint8_t* mb = (mod[0]=='c') ? cb : eb;
            log("viewangle CONFIRMED @ %s+0x%llX", mod, (long long)(u - mb));
        }
        g_va_candidates.clear();
        g_va_candidates.shrink_to_fit();
    }
}

// Layout auditor — every 33rd tick while RECORDING, dump the raw first 96
// bytes of the engine-built cmd plus its classic-layout interpretation.
// With known keys held (W-only, then A-only) the dump pins every real
// field offset of Momentum's ~120-byte CUserCmd. Remove once resolved.
static void dump_cmd_layout(int seq, const CUserCmd* cmd) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(cmd);
    char hex[3 * 96 + 1] = {};
    for (int i = 0; i < 96; ++i)
        std::snprintf(hex + i * 3, 4, "%02X ", p[i]);
    log("[LayoutAudit] seq=%d bytes[0..95]: %s", seq, hex);
    log("[LayoutAudit] as-classic: ang=(%.2f %.2f %.2f) fw=%.1f side=%.1f up=%.1f btns=0x%X",
        cmd->viewangles.pitch, cmd->viewangles.yaw, cmd->viewangles.roll,
        cmd->forwardmove, cmd->sidemove, cmd->upmove, cmd->buttons);
}

// IInput::CreateMove(int seq, float ft, bool active). Slot 3 identified
// from vtable byte-dump analysis: iv[3] has the largest prologue (8 pushes,
// 0x78 stack, XMM save) consistent with heavy float/movement math + reads
// xmm2 as the ft arg. iv[11] is KeyEvent (~31 calls/session, first int
// always 1 = `down`) — that's the wrong slot the last port attempt sat on.
constexpr int PROBE_SLOTS[] = { 3 };
constexpr int N_PROBES      = sizeof(PROBE_SLOTS) / sizeof(PROBE_SLOTS[0]);
static int   cm_probe_counts[N_PROBES]    = {};
static void* cm_probe_originals[N_PROBES] = {};

template <int IDX>
static void __fastcall hk_cm_probe(void* self, int seq_num, float sample_time, bool active) {
    ++cm_probe_counts[IDX];
    ++g_cm_createmove_calls;

    __try {
        // Call original IInput::CreateMove FIRST — that's what populates
        // the command ring with the freshly-sampled cmd for this seq_num.
        using Fn = void(*)(void*, int, float, bool);
        reinterpret_cast<Fn>(cm_probe_originals[IDX])(self, seq_num, sample_time, active);

        if (!active || !g_input) return;

        // Resolve the freshly-built cmd by calling the game's OWN
        // IInput::GetUserCmd (slot 8) — its code knows the real container
        // layout and stride, so we never hardcode offsets. The disasm
        // scanner confirmed slot 8 loads m_pCommands via mov r64,[rcx+0xB8]
        // on this build. Two candidate ABIs exist:
        //   sig A: GetUserCmd(int nSlot, int seq)  — splitscreen ABI
        //   sig B: GetUserCmd(int seq)             — classic ABI
        // Probe A first: if the real ABI is B, A's rdx=0 just fetches
        // entry 0 (in-bounds, harmless) and fails cn validation; whereas
        // probing B against a real A would put garbage in the index reg.
        // Cache whichever signature validates.
        static int getusercmd_sig = -1; // -1 unknown, 0 = A, 1 = B
        void** ivt = *reinterpret_cast<void***>(g_input);
        using FnA = CUserCmd*(*)(void*, int, int);
        using FnB = CUserCmd*(*)(void*, int);

        CUserCmd* cmd = nullptr;
        if (getusercmd_sig == 0) {
            cmd = reinterpret_cast<FnA>(ivt[8])(g_input, 0, seq_num);
        } else if (getusercmd_sig == 1) {
            cmd = reinterpret_cast<FnB>(ivt[8])(g_input, seq_num);
        } else {
            cmd = reinterpret_cast<FnA>(ivt[8])(g_input, 0, seq_num);
            if (cmd && region_readable(cmd, sizeof(CUserCmd)) &&
                cmd->command_number == seq_num) {
                getusercmd_sig = 0;
                log("GetUserCmd ABI resolved: (nSlot, seq) — cached");
            } else {
                cmd = reinterpret_cast<FnB>(ivt[8])(g_input, seq_num);
                if (cmd && region_readable(cmd, sizeof(CUserCmd)) &&
                    cmd->command_number == seq_num) {
                    getusercmd_sig = 1;
                    log("GetUserCmd ABI resolved: (seq) — cached");
                } else {
                    cmd = nullptr;
                }
            }
        }

        if (!cmd || !region_readable(cmd, sizeof(CUserCmd))) return;

        static int diag_left = 5;
        if (diag_left > 0) {
            --diag_left;
            log("iv[%d] seq=%d cmd@%p cn=%d tick=%d fw=%.1f side=%.1f yaw=%.1f btns=0x%X",
                PROBE_SLOTS[IDX], seq_num, cmd, cmd->command_number, cmd->tick_count,
                cmd->forwardmove, cmd->sidemove, cmd->viewangles.yaw, cmd->buttons);
        }

        if (cmd->command_number == seq_num) {
            // Reference view state for the slot-discovery sweep + layout
            // audit — only when the tape is NOT driving the cmd, so the
            // angles are the user's real view.
            recorder::Mode rmode = recorder::mode();
            if (rmode == recorder::Mode::Idle || rmode == recorder::Mode::Recording) {
                g_last_cmd_angles     = cmd->viewangles;
                g_last_cmd_angles_seq = seq_num;
                // NOTE: the automatic vtable sweep is DISABLED — blind-calling
                // unknown engine slots with fabricated args crashed the game.
                // Camera-follow now goes through the memory-scan path
                // (locate_view_angle_mem), read-only discovery + write-only
                // apply, zero function calls. See set_view_angles.
                if (rmode == recorder::Mode::Idle)
                    locate_view_angle_mem(cmd->viewangles);
            }
            if (rmode == recorder::Mode::Recording && (seq_num % 33) == 0)
                dump_cmd_layout(seq_num, cmd);

            recorder::on_createmove(cmd);
            // GetUserCmd's array and the CInput+0x108 array are the cmd ring
            // and its verified shadow — the engine consumes the +0x108 one
            // for the outgoing move, so every write the recorder/optimizer
            // just made to *cmd must be mirrored there too.
            mirror_cmd_to_send_array(seq_num, cmd);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // wrong-signature call — swallow so we don't take the game down
    }
}

static void* const PROBE_HOOKS[N_PROBES] = {
    reinterpret_cast<void*>(&hk_cm_probe<0>),
};

// IInput::ExtraMouseSample — iv[4], the sibling right after CreateMove.
// Signature: void ExtraMouseSample(float frametime, bool active). It runs
// EVERY rendered frame (not just per tick) to keep the mouse smooth between
// ticks, and it rewrites the view from the live mouse — which is exactly what
// clobbers our tape angle between ticks and freezes the playback camera. We
// call the original, then (during playback) re-stamp the tape angle onto the
// view globals AFTER the engine's own write but BEFORE the frame renders.
static void*  o_extra_mouse_sample = nullptr;
using ExtraMouse_t = void(*)(void*, float, bool);

static void __fastcall hk_extra_mouse_sample(void* self, float frametime, bool active) {
    __try {
        if (o_extra_mouse_sample)
            reinterpret_cast<ExtraMouse_t>(o_extra_mouse_sample)(self, frametime, active);
        if (recorder::mode() == recorder::Mode::Playing)
            hooks::reassert_view_angle();
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Sig-scan client.dll's IBaseClientDLL vtable for a slot whose function loads
// `g_pClientMode` via `mov r64, [rip+off]`. HudProcessInput and a few other
// hud methods do that: `GetClientModeNormal()->…`. Filter out the g_pInput
// pointer we already found so we don't grab the same one twice.
static void* find_g_pClientMode() {
    for (int slot = 5; slot <= 25; ++slot) {
        void* fn = g_client_vtable[slot];
        if (!fn || !region_readable(fn, 128)) continue;
        uint8_t* p = reinterpret_cast<uint8_t*>(fn);
        for (int i = 0; i < 96; ++i) {
            if (p[i] != 0x48 || p[i+1] != 0x8B) continue;
            if ((p[i+2] & 0xC7) != 0x05) continue;

            int32_t rel = *reinterpret_cast<int32_t*>(p + i + 3);
            void** ref = reinterpret_cast<void**>(p + i + 7 + rel);
            if (!region_readable(ref, sizeof(void*))) continue;
            void* candidate = *ref;
            if (!candidate || candidate == g_input) continue;
            if (!region_readable(candidate, 16)) continue;

            void** vt = *reinterpret_cast<void***>(candidate);
            if (!vt || !vtable_looks_valid(vt, 20)) continue;

            log("  slot %d @ %p → mov +%d → g_pClientMode candidate %p (vtable %p)",
                slot, fn, i, candidate, (void*)vt);
            return candidate;
        }
    }
    return nullptr;
}

// ─── Momentum savestate concommands ───────────────────────────────────
// ALL confirmed authoritative from `find mom_savestate` in the console
// 2026-07-23 (every one flagged clientcmd_can_execute, so ClientCmd runs
// them — the create toasts "Savestate N created!" prove dispatch works):
//   mom_savestate_create  — creates a saveloc                       (E)
//   mom_savestate_load     — teleports to the CURRENT saved location (Q)
//   mom_savestate_prev     — back through the list, teleporting      (1)
//   mom_savestate_next     — forward through the list, teleporting   (3)
// load is a PLAIN concommand that teleports on execution — the earlier
// +/-mom_savestate_load pair was the button-bind wrapper, and firing +/- in
// one frame cancelled the teleport. Using the plain command fixes Q.
static constexpr const char* CMD_SS_CREATE   = "mom_savestate_create";
static constexpr const char* CMD_SS_LOAD     = "mom_savestate_load";   // plain (instant)
static constexpr const char* CMD_SS_LOAD_DN  = "+mom_savestate_load";  // hold = freeze at lock
static constexpr const char* CMD_SS_LOAD_UP  = "-mom_savestate_load";  // release = go
static constexpr const char* CMD_SS_PREV     = "mom_savestate_prev";
static constexpr const char* CMD_SS_NEXT     = "mom_savestate_next";


static LRESULT CALLBACK hk_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    // Rebindable hotkeys — actions fire but DON'T consume the event, so the
    // key still reaches the game (weapon switch, jump, etc). Exceptions:
    // the menu-toggle key and any press eaten by the binds-UI capture.
    // Any input — keyboard OR any mouse button (L/R/M/X1/X2) — can be a hotkey.
    // Actions fire but DON'T consume the event (the key still reaches the game),
    // except the menu-toggle key and a press eaten by the bind-capture UI.
    int  vk = 0;
    bool repeat = false, is_down = false, is_mouse = false;
    switch (msg) {
        case WM_KEYDOWN:     vk = static_cast<int>(wp); repeat = (lp & (1LL << 30)) != 0; is_down = true; break;
        case WM_LBUTTONDOWN: vk = VK_LBUTTON;  is_down = is_mouse = true; break;
        case WM_RBUTTONDOWN: vk = VK_RBUTTON;  is_down = is_mouse = true; break;
        case WM_MBUTTONDOWN: vk = VK_MBUTTON;  is_down = is_mouse = true; break;
        case WM_XBUTTONDOWN: vk = (HIWORD(wp) == XBUTTON2) ? VK_XBUTTON2 : VK_XBUTTON1;
                             is_down = is_mouse = true; break;
    }
    if (is_down) {
        // A "press an input…" capture in the Settings tab eats the press —
        // works for keyboard and mouse buttons alike.
        if (menu::bind_feed_key(vk)) return 0;

        if (!repeat && vk == menu::bind_vk(menu::BindMenu)) {
            menu::g_open = !menu::g_open;
            return 0; // consume — dedicated menu key
        }

        // Mouse hotkeys fire only with the menu CLOSED (so clicks drive the UI
        // while it's open); keyboard hotkeys fire unless a text field is focused.
        if (is_mouse ? !menu::g_open : !menu::ui_wants_keyboard()) {
            if (vk == menu::bind_vk(menu::BindPause)) {
                log("[Hotkey] pause");
                recorder::pause();
            } else if (vk == menu::bind_vk(menu::BindPlay)) {
                log("[Hotkey] play_from_start");
                recorder::play_from_start();
            } else if (vk == menu::bind_vk(menu::BindRecord)) {
                // Blocked during playback, otherwise it silently wipes the
                // tape mid-replay.
                if (recorder::mode() != recorder::Mode::Playing)
                    recorder::start_record();
            } else if (vk == menu::bind_vk(menu::BindStop)) {
                recorder::stop(); // always safe
            } else if (!repeat && vk == menu::bind_vk(menu::BindCommit)) {
                // Commit segment: create a savestate so the game remembers the
                // spot for a later teleport, AND pin a tape checkpoint.
                if (recorder::mode() == recorder::Mode::Recording) {
                    if (menu::g_commit_freeze) {
                        recorder::request_commit_scrub();  // recorder fires the save (crouch blocked)
                        log("[Hotkey] commit (scrub) requested");
                    } else {
                        recorder::note_saveloc_fired();
                        hooks::execute_server_cmd(CMD_SS_CREATE);
                        recorder::commit();
                        log("[Hotkey] commit -> %s + tape marker", CMD_SS_CREATE);
                    }
                }
            } else if (!repeat && vk == menu::bind_vk(menu::BindRollback)) {
                // Load the CURRENT savestate. With clean-inputs on we use the
                // +command (hold = frozen at the lock, release = go), and the
                // A/D+crouch scrub is armed on the key RELEASE below. With it
                // off, the plain command teleports instantly. Recording pins the
                // rollback marker; Idle just teleports. Playback ignores the key.
                const char* loadcmd = menu::g_commit_freeze ? CMD_SS_LOAD_DN : CMD_SS_LOAD;
                recorder::Mode m = recorder::mode();
                if (m == recorder::Mode::Recording) {
                    hooks::execute_server_cmd(loadcmd);
                    recorder::rollback();
                    if (menu::g_commit_freeze) recorder::begin_load_scrub();  // scrub A/D+crouch while held
                    log("[Hotkey] rollback -> %s + tape marker", loadcmd);
                } else if (m == recorder::Mode::Idle) {
                    hooks::execute_server_cmd(loadcmd);
                    log("[Hotkey] %s (idle)", loadcmd);
                }
            } else if (!repeat && vk == menu::bind_vk(menu::BindPrevloc)) {
                // Move the RECORDER's selected segment back one — over the
                // segments YOU saved with E. Works Recording or Idle. Does
                // nothing if there's no earlier segment.
                recorder::rollback_prev();
                log("[Hotkey] prev segment");
            } else if (!repeat && vk == menu::bind_vk(menu::BindNextloc)) {
                // Move the RECORDER's selected segment forward one.
                recorder::rollback_next();
                log("[Hotkey] next segment");
            }
        }
    }

    // Load-key RELEASE (clean-inputs mode): the +command held you frozen at the
    // lock; releasing it (-command) unfreezes and starts movement. Fire the
    // release and arm the A/D+crouch scrub for the ticks right after.
    if (menu::g_commit_freeze &&
        (msg == WM_KEYUP || msg == WM_XBUTTONUP || msg == WM_MBUTTONUP)) {
        int uvk;
        if (msg == WM_KEYUP)          uvk = static_cast<int>(wp);
        else if (msg == WM_MBUTTONUP) uvk = VK_MBUTTON;
        else                          uvk = (HIWORD(wp) == XBUTTON2) ? VK_XBUTTON2 : VK_XBUTTON1;
        if (uvk == menu::bind_vk(menu::BindRollback)) {
            recorder::Mode m = recorder::mode();
            if (m == recorder::Mode::Recording || m == recorder::Mode::Idle) {
                hooks::execute_server_cmd(CMD_SS_LOAD_UP);   // release = go
                recorder::arm_load_scrub();
                log("[Hotkey] load released -> %s + scrub armed", CMD_SS_LOAD_UP);
            }
        }
    }

    // Right-click overdub: take over live at the current playback frame and
    // re-record the tail. Only while a tape is Playing, only when the toggle is
    // on, and only when the menu is closed (so UI clicks can't truncate the
    // tape). Lets you fix a bad segment by re-running from a clean spot instead
    // of redoing the whole tape.
    if (msg == WM_RBUTTONDOWN && menu::g_rmb_overdub && !menu::g_open &&
        recorder::mode() == recorder::Mode::Playing) {
        recorder::overdub_from_head();
        log("[Hotkey] RMB overdub — taking over from frame %zu", recorder::play_head());
    }

    if (menu::g_open) {
        ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp);
        switch (msg) {
        case WM_MOUSEMOVE:  case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:case WM_RBUTTONUP:   case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:  case WM_MOUSEWHEEL:  case WM_KEYDOWN:
        case WM_KEYUP:      case WM_CHAR:        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            return 1L;
        }
    }
    return CallWindowProcA(reinterpret_cast<WNDPROC>(o_wndproc), h, msg, wp, lp);
}

// Build our render-target view for the swap chain's current backbuffer.
// Called on first Present and again after every ResizeBuffers.
static void build_rtv_from_swapchain(IDXGISwapChain* swap) {
    if (!g_dev || g_rtv) return;
    ID3D11Texture2D* backbuf = nullptr;
    if (SUCCEEDED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                  reinterpret_cast<void**>(&backbuf))) && backbuf) {
        g_dev->CreateRenderTargetView(backbuf, nullptr, &g_rtv);
        backbuf->Release();
    }
}

// Shared render code — the Present hook lands here every frame.
static void present_common(IDXGISwapChain* swap) {
    ++g_endscene_calls;

    if (!g_imgui_up) {
        // First fire: capture the device/context/hwnd/RTV from the real
        // swap chain, then boot ImGui's Win32 + DX11 backends.
        if (FAILED(swap->GetDevice(__uuidof(ID3D11Device),
                                   reinterpret_cast<void**>(&g_dev))) || !g_dev) {
            return; // try again next frame
        }
        g_dev->GetImmediateContext(&g_ctx);
        if (!g_ctx) return;

        DXGI_SWAP_CHAIN_DESC scd{};
        swap->GetDesc(&scd);
        g_hwnd = scd.OutputWindow ? scd.OutputWindow : find_game_window();

        build_rtv_from_swapchain(swap);

        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        ImGui::StyleColorsDark();
        ImGui_ImplWin32_Init(g_hwnd);
        ImGui_ImplDX11_Init(g_dev, g_ctx);

        o_wndproc = reinterpret_cast<WndProc_t>(
            SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(hk_wndproc)));
        g_imgui_up = true;

        menu::g_open = true;
        menu::start_welcome();

        log("first Present — hwnd=%p, dev=%p, ctx=%p, rtv=%p, wndproc-orig=%p",
            (void*)g_hwnd, (void*)g_dev, (void*)g_ctx, (void*)g_rtv, (void*)o_wndproc);
        MessageBeep(MB_OK);
    }

    // Rebuild the RTV if ResizeBuffers dropped it since the last Present.
    if (!g_rtv) build_rtv_from_swapchain(swap);

    // Per-frame camera hold during playback: re-stamp the tape's view angle so
    // the engine's per-frame mouse resample can't clobber it between ticks.
    if (recorder::mode() == recorder::Mode::Playing)
        hooks::reassert_view_angle();

    // Toggle cursor visibility + neuter SetCursorPos on menu open/close.
    static bool prev_menu_open = false;
    if (menu::g_open != prev_menu_open) {
        ShowCursor(menu::g_open ? TRUE : FALSE);
        if (menu::g_open) patch_setcursorpos_noop();
        else              unpatch_setcursorpos();
        prev_menu_open = menu::g_open;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    menu::draw();

    ImGui::EndFrame();
    ImGui::Render();
    if (g_rtv) g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

static HRESULT WINAPI hk_present(IDXGISwapChain* swap, UINT sync_interval, UINT flags) {
    present_common(swap);
    return o_present_fn(swap, sync_interval, flags);
}

static HRESULT WINAPI hk_resize_buffers(IDXGISwapChain* swap, UINT buf_count,
                                        UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    // Drop our RTV so the resize can release the backbuffer, and let ImGui
    // invalidate its own device objects so nothing outlives the old buffers.
    // Both get rebuilt on the next Present.
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_imgui_up) ImGui_ImplDX11_InvalidateDeviceObjects();
    HRESULT hr = o_resize_buffers(swap, buf_count, w, h, fmt, flags);
    if (g_imgui_up) ImGui_ImplDX11_CreateDeviceObjects();
    return hr;
}

// ─── install / uninstall ──────────────────────────────────────────────
static void dump_relevant_modules() {
    HMODULE mods[512];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return;
    int n = static_cast<int>(needed / sizeof(HMODULE));
    log("relevant modules loaded:");
    for (int i = 0; i < n; ++i) {
        char path[MAX_PATH] = "";
        if (!GetModuleFileNameExA(GetCurrentProcess(), mods[i], path, sizeof(path))) continue;

        const char* name = strrchr(path, '\\');
        name = name ? name + 1 : path;

        char lower[MAX_PATH];
        for (int k = 0; k < (int)sizeof(lower) - 1 && name[k]; ++k)
            lower[k] = static_cast<char>(tolower(static_cast<unsigned char>(name[k])));
        lower[MAX_PATH - 1] = '\0';

        if (strstr(lower, "d3d")     || strstr(lower, "dxvk") ||
            strstr(lower, "vulkan")  || strstr(lower, "shaderapi") ||
            strstr(lower, "materialsystem") || strstr(lower, "vk_")) {
            log("  %-30s  @ %p  (%s)", name, (void*)mods[i], path);
        }
    }
}

bool hooks::install() {
    log("=== momentum_menu injected (Momentum Mod Playtest x64 / Strata Source, SDK 2013 MP path) ===");
    log("waiting for game dlls...");

    int waits = 0;
    while (!GetModuleHandleA("client.dll") ||
           !GetModuleHandleA("engine.dll") ||
           !GetModuleHandleA("shaderapidx11.dll")) {
        Sleep(200);
        if (++waits > 300) { die("timed out waiting for client/engine/shaderapidx11.dll (60s)."); return false; }
    }
    log("dlls loaded. settling 2.5s...");
    Sleep(2500);

    // IBaseClientDLL — Momentum exports VClient018 (SDK 2013 MP lineage);
    // sweep a wider band in case an update bumps it.
    log("resolving VClient*:");
    g_client = sweep("client.dll", "VClient", 15, 22);
    if (!g_client) { die("no VClient015-022 in client.dll — Momentum may have bumped the version past 22."); return false; }

    // Snapshot client vtable BEFORE any hooking so sig-scan sees virgin bytes
    g_client_vtable = *reinterpret_cast<void***>(g_client);

    // IInput — Valve dropped the VClientInput CreateInterface export in the
    // 2025 rebuild. Try both anyway, then sig-scan g_pInput out of client.dll.
    log("resolving IInput:");
    g_input = sweep("client.dll", "VClientInput", 1, 3);
    if (!g_input) {
        log("  VClientInput not exported — sig-scanning g_pInput from client.dll vtable...");
        g_input = find_input_via_sigscan(g_client_vtable);
    }
    if (!g_input) {
        die("could not locate g_pInput (neither VClientInput export nor sig-scan worked)."); return false;
    }
    log("  IInput @ %p", g_input);

    // IVEngineClient — needed for local view rotation during playback + console cmds.
    log("resolving IVEngineClient:");
    g_engine = sweep("engine.dll", "VEngineClient", 13, 16);
    log("  IVEngineClient @ %p", g_engine);

    // Hook IVEngineClient::ClientCmd (slot 7) to detect user chat commands
    // for SaveLoc-mode event capture. Store the original for our own outbound
    // calls to avoid recursion.
    if (g_engine) {
        dump_engine_vtable();   // read-only — identify real ClientCmd/ServerCmd
        void** engine_vt = *reinterpret_cast<void***>(g_engine);
        hooks::o_engine_client_cmd = reinterpret_cast<hooks::EngineClientCmd_t>(engine_vt[7]);
        swap_slot(engine_vt, 7, reinterpret_cast<void*>(::hk_engine_client_cmd));
        log("IVEngineClient::ClientCmd hooked (slot 7, orig=%p)",
            (void*)hooks::o_engine_client_cmd);
    }

    // IClientEntityList — needed for reading local player position (walk-to-start).
    log("resolving IClientEntityList:");
    g_ent_list = sweep("client.dll", "VClientEntityList", 3, 5);
    log("  IClientEntityList @ %p", g_ent_list);

    // Sig-scan m_pCommands/m_pVerifiedCommands offsets from IInput::GetUserCmd.
    dump_input_vtable();
    find_input_data_offsets();

    // IInput slot 5 hook DISABLED again — even without the CRC bypass memcpy,
    // just installing the hook + calling the original crashes Momentum on
    // map load. The shipped playtest binary is closed-source (Momentum went
    // closed in 2021) so we can't rely on GitHub header slot numbers to
    // know which slot is really WriteDelta vs. something with a different
    // signature. Real per-tick capture on Momentum needs proper binary RE
    // of CInput's vtable — deferred until we're ready for that work.
    log("IInput slot 5 hook DISABLED — signature mismatch crashes Momentum, needs binary RE.");

    // Diagnostic: dump each client-vtable slot so we can identify CreateMove
    // if the slot number shifted between SDK versions. CreateMove references
    // g_pInput (via `input->CreateMove(...)`) so we can spot it.
    log("client vtable slot dump (looking for CreateMove):");
    for (int slot = 15; slot <= 30; ++slot) {
        void* fn = g_client_vtable[slot];
        if (!fn) { log("  vt[%2d] = null", slot); continue; }
        uint8_t* p = reinterpret_cast<uint8_t*>(fn);
        if (!region_readable(p, 128)) { log("  vt[%2d] = unreadable", slot); continue; }
        char hex[64] = "";
        for (int i = 0; i < 16; ++i) {
            char b[4]; std::snprintf(b, sizeof(b), "%02X ", p[i]);
            strcat(hex, b);
        }
        bool refs_input = false;
        for (int i = 0; i < 96 && !refs_input; ++i) {
            if (p[i] == 0x48 && p[i+1] == 0x8B && (p[i+2] & 0xC7) == 0x05) {
                int32_t rel = *reinterpret_cast<int32_t*>(p + i + 3);
                void* refaddr = p + i + 7 + rel;
                if (region_readable(refaddr, 8) && *reinterpret_cast<void**>(refaddr) == g_input)
                    refs_input = true;
            }
        }
        log("  vt[%2d] @ %p  refs_input=%d  bytes: %s",
            slot, fn, refs_input ? 1 : 0, hex);
    }

    // Hook IBaseClientDLL::CreateMove — pre-read original to close the
    // install-time race window.
    o_createmove = reinterpret_cast<CreateMove_t>(g_client_vtable[VT_CLIENT_CM]);
    if (!o_createmove) { die("IBaseClientDLL::CreateMove vtable slot is null (bad slot?)"); return false; }
    swap_slot(g_client_vtable, VT_CLIENT_CM, reinterpret_cast<void*>(hk_createmove));
    log("IBaseClientDLL::CreateMove hooked (slot %d, orig=%p)", VT_CLIENT_CM, (void*)o_createmove);

    // Per-tick CreateMove capture — probe IInput instead of IClientMode.
    //
    // The client-DLL vtable's slot 21 (IBaseClientDLL::CreateMove) is
    // literally the thunk `input->vt[11](args)` on this build (see the
    // vtable dump above: `mov rcx, [g_pInput]; mov rax, [rcx]; jmp
    // [rax+0x58]`, and 0x58/8 = slot 11). If Momentum's engine ever
    // bypasses the client thunk and calls input->CreateMove directly,
    // hooking the client-side slot 21 misses the call entirely — which
    // matches what we saw: vt[21] fired ~21 times per session while
    // Present fired thousands, so tick-rate calls skip vt[21].
    //
    // Historical baggage: an earlier iteration sig-scanned for
    // g_pClientMode and probed its vtable. That scan turned out to grab
    // g_pEngineClient by mistake (same instance pointer on this build),
    // so the probe was hooking a random IVEngineClient slot and never
    // fired. The IInput slot 11 hook below replaces that entire
    // machinery — we co-opt g_cm_vtable / cm_probe_originals to point at
    // g_pInput's vtable so the diagnostics UI keeps working unchanged.
    if (g_input) {
        g_cm_vtable = *reinterpret_cast<void***>(g_input);
        for (int i = 0; i < N_PROBES; ++i) {
            int slot = PROBE_SLOTS[i];
            cm_probe_originals[i] = swap_slot(g_cm_vtable, slot, PROBE_HOOKS[i]);
            log("  probe hook installed IInput vt[%2d] (orig=%p)",
                slot, cm_probe_originals[i]);
        }
        // iv[4] = ExtraMouseSample — per-frame view resample. Hook it so the
        // playback camera can be re-stamped every frame (see the hook).
        o_extra_mouse_sample = swap_slot(g_cm_vtable, 4,
            reinterpret_cast<void*>(&hk_extra_mouse_sample));
        log("  ExtraMouseSample hook installed IInput vt[ 4] (orig=%p)",
            o_extra_mouse_sample);
    } else {
        log("g_input null — per-tick capture path disabled");
    }

    dump_relevant_modules();

    if (!grab_swapchain_vtable()) { die("could not create dummy IDXGISwapChain (D3D11)"); return false; }

    log("hk_present         address = %p", (void*)&hk_present);
    log("hk_resize_buffers  address = %p", (void*)&hk_resize_buffers);

    // Pull the real code addresses of Present + ResizeBuffers from the
    // dummy vtable. These live in dxgi.dll and are shared by every swap
    // chain in the process — hooking them once catches every future
    // Present regardless of when the game creates its real swap chain.
    void* present_fn        = g_swapchain_vt[VT_SWAPCHAIN_PRESENT];
    void* resize_buffers_fn = g_swapchain_vt[VT_SWAPCHAIN_RESIZE_BUFFERS];
    if (!present_fn || !resize_buffers_fn) {
        die("could not read Present/ResizeBuffers from IDXGISwapChain vtable"); return false;
    }

    // Log the first 32 bytes of each target so we can verify the disassembler
    // handled the prologue if a hook fails.
    auto dump_bytes = [](const char* label, void* fn) {
        uint8_t* p = reinterpret_cast<uint8_t*>(fn);
        char hex[128] = "";
        for (int i = 0; i < 24; ++i) {
            char b[4]; std::snprintf(b, sizeof(b), "%02X ", p[i]);
            strcat(hex, b);
        }
        log("%s @ %p bytes: %s", label, fn, hex);
    };
    dump_bytes("Present",       present_fn);
    dump_bytes("ResizeBuffers", resize_buffers_fn);

    // Steam overlay (and a few other tools like RTSS / Discord) inline-hook
    // DXGI Present/ResizeBuffers with jump thunks — usually a chain like
    // `E9 rel32` (relative jump) into a stub that then does `FF 25 disp32`
    // (indirect jump through a data pointer). Our minimal length
    // disassembler can't relocate either, so if we see one we chase the
    // thunks to the destination and hook THERE — that lands inside the
    // other tool's hook body, which simply chains us before their existing
    // "call original" trampoline. (Cap chases at 8 hops to defend against
    // a hostile loop.)
    auto unwrap_thunks = [](void* fn) -> void* {
        uint8_t* p = reinterpret_cast<uint8_t*>(fn);
        for (int i = 0; i < 8 && p; ++i) {
            if (p[0] == 0xE9) {                          // jmp rel32
                int32_t rel = *reinterpret_cast<int32_t*>(p + 1);
                p = p + 5 + rel;
            } else if (p[0] == 0xFF && p[1] == 0x25) {   // jmp qword ptr [rip+disp32]
                int32_t disp = *reinterpret_cast<int32_t*>(p + 2);
                uint8_t** slot = reinterpret_cast<uint8_t**>(p + 6 + disp);
                p = *slot;
            } else {
                break;
            }
        }
        return p;
    };
    void* present_target        = unwrap_thunks(present_fn);
    void* resize_buffers_target = unwrap_thunks(resize_buffers_fn);
    if (present_target != present_fn) {
        log("Present prologue is a thunk chain — chased to %p", present_target);
        dump_bytes("Present (chased)",       present_target);
    }
    if (resize_buffers_target != resize_buffers_fn) {
        log("ResizeBuffers prologue is a thunk chain — chased to %p", resize_buffers_target);
        dump_bytes("ResizeBuffers (chased)", resize_buffers_target);
    }

    // Inline function hooks — hit the code itself, so every swap chain is
    // covered (including any the game creates AFTER we injected).
    if (!g_hook_present.install(present_target, reinterpret_cast<void*>(hk_present))) {
        die("inline hook on IDXGISwapChain::Present failed (unknown prologue bytes — see log)"); return false;
    }
    o_present_fn = g_hook_present.get_original<Present_t>();
    log("Present inline-hooked (saved %zu bytes)", g_hook_present.saved_sz);

    if (!g_hook_resize_buffers.install(resize_buffers_target, reinterpret_cast<void*>(hk_resize_buffers))) {
        log("ResizeBuffers inline hook failed — RTV will leak backbuffer on window resize, continuing.");
    } else {
        o_resize_buffers = g_hook_resize_buffers.get_original<ResizeBuffers_t>();
        log("ResizeBuffers inline-hooked (saved %zu bytes)", g_hook_resize_buffers.saved_sz);
    }

    // Capture SetCursorPos's first 6 bytes for the dynamic patch/unpatch flow.
    if (HMODULE user32 = GetModuleHandleA("user32.dll")) {
        g_scp_addr = reinterpret_cast<uint8_t*>(GetProcAddress(user32, "SetCursorPos"));
        if (g_scp_addr) {
            memcpy(g_scp_saved, g_scp_addr, 6);
            log("SetCursorPos @ %p — saved 6 bytes for patch-on-open", (void*)g_scp_addr);
        } else {
            log("SetCursorPos not exported by user32 — mouse may snap");
        }
    }

    menu::init_paths();
    menu::g_open = true;
    g_ready = true;

    log("=== all hooks installed. waiting for first Present... ===");
    MessageBeep(0xFFFFFFFF);
    return true;
}

void hooks::uninstall() {
    log("uninstall requested");
    if (o_createmove && g_client_vtable)
        swap_slot(g_client_vtable, VT_CLIENT_CM, reinterpret_cast<void*>(o_createmove));
    if (g_cm_vtable) {
        for (int i = 0; i < N_PROBES; ++i) {
            if (cm_probe_originals[i])
                swap_slot(g_cm_vtable, PROBE_SLOTS[i], cm_probe_originals[i]);
        }
        if (o_extra_mouse_sample)
            swap_slot(g_cm_vtable, 4, o_extra_mouse_sample);
    }

    g_hook_present.uninstall();
    g_hook_resize_buffers.uninstall();
    g_hook_cm_createmove.uninstall();

    // Restore IVEngineClient::ClientCmd (slot 7). CRITICAL — if we skip this
    // and the DLL is unloaded, the vtable still points at hk_engine_client_cmd
    // whose memory has been freed, so the next chat command crashes the game.
    if (o_engine_client_cmd && g_engine) {
        void** engine_vt = *reinterpret_cast<void***>(g_engine);
        swap_slot(engine_vt, 7, reinterpret_cast<void*>(o_engine_client_cmd));
        log("IVEngineClient::ClientCmd slot 7 restored to %p", (void*)o_engine_client_cmd);
        o_engine_client_cmd = nullptr;
    }

    if (o_write_delta && g_input) {
        void** input_vt = *reinterpret_cast<void***>(g_input);
        swap_slot(input_vt, 5, reinterpret_cast<void*>(o_write_delta));
    }
    unpatch_setcursorpos();

    if (g_hwnd && o_wndproc)
        SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(o_wndproc));

    if (g_imgui_up) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_imgui_up = false;
    }

    // Release the DX11 refs we AddRef'd on first Present. Order matters:
    // RTV → immediate context → device (device owns the others).
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}

namespace hooks {
    static void* resolve_local_player();   // defined lower with the netvar helpers

    int  endscene_calls()      { return g_endscene_calls; }
    int  createmove_calls()    { return g_createmove_calls; }
    int  cm_createmove_calls() { return g_cm_createmove_calls; }
    bool ready()               { return g_ready; }

    int  probe_count()         { return N_PROBES; }
    int  probe_slot(int idx)   { return (idx >= 0 && idx < N_PROBES) ? PROBE_SLOTS[idx] : -1; }
    int  probe_hits(int idx)   { return (idx >= 0 && idx < N_PROBES) ? cm_probe_counts[idx] : 0; }
    int  write_delta_calls()   { return g_write_delta_calls; }

    // View rotation. The old SetViewAngles vtable slot (2013-MP slot 20) is
    // dead on Strata, and the runtime vtable sweep that tried to find the
    // new one crashed the game (blind-calling non-getter slots). This now
    // writes the view angles straight into cl.viewangles — the engine global
    // the scanner located (locate_view_angle_mem) that the first-person
    // renderer reads. Pure store, no function call, can't crash. If the
    // global isn't located yet, silent no-op (movement still replays via the
    // cmd; only the on-screen camera waits on the scan).
    // Player-entity view-angle field offsets. These are STRUCT offsets — the
    // same every session for a given game build — so we hardcode the ones the
    // scan found working (player+0x34F4 is the first-person render source,
    // +0x38D0 the networked eye angle). Hardcoding makes camera-follow reliable
    // across game restarts instead of depending on a flaky per-session re-scan
    // (a restart that only re-locked 0x38D0 left the camera frozen). The scan
    // still runs and appends anything else it confirms.
    static const intptr_t KNOWN_ENT_VIEW_OFFS[] = { 0x34F4, 0x38D0 };

    static void write_all_view_angles(float pitch, float yaw) {
        for (float* p : g_view_angle_ptrs) {
            if (!region_readable(p, 12)) continue;
            p[0] = pitch;
            p[1] = yaw;
        }
        void* ent = resolve_local_player();
        if (ent) {
            uint8_t* eb = reinterpret_cast<uint8_t*>(ent);
            for (intptr_t off : KNOWN_ENT_VIEW_OFFS) {
                float* p = reinterpret_cast<float*>(eb + off);
                if (!region_readable(p, 12)) continue;
                p[0] = pitch;
                p[1] = yaw;
            }
            for (intptr_t off : g_ent_view_offs) {
                float* p = reinterpret_cast<float*>(eb + off);
                if (!region_readable(p, 12)) continue;
                p[0] = pitch;
                p[1] = yaw;
            }
        }
    }

    void set_view_angles(const QAngle3& angles) {
        // Cache for the per-frame re-assert (see reassert_view_angle). The
        // tick-rate write alone loses to the engine's per-frame mouse resample.
        g_last_set_va = angles;
        g_have_set_va = true;
        __try { write_all_view_angles(angles.pitch, angles.yaw); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Called every FRAME from the Present + ExtraMouseSample hooks while a tape
    // is playing. set_view_angles writes at tick rate (66/s), but the engine
    // re-samples the mouse and rewrites the view every rendered frame — so
    // between ticks our value gets clobbered and the camera never visibly
    // turns. Re-stamping the cached tape angle each frame holds the camera on
    // the tape. Movement is unaffected (it's driven by the cmd).
    void reassert_view_angle() {
        if (!g_have_set_va) return;
        __try { write_all_view_angles(g_last_set_va.pitch, g_last_set_va.yaw); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // ClientCmd — slot 7 on IVEngineClient (SDK 2013 MP). Restricted commands
    // (like setpos) need sv_cheats 1 on the server. We store the original so
    // our own outbound calls bypass the hook we install below (avoids infinite
    // recursion + prevents our own /teleport calls from being logged as user
    // events during recording).
    void execute_client_cmd(const char* cmd) {
        if (!g_engine || !cmd) return;
        __try {
            EngineClientCmd_t fn = o_engine_client_cmd
                ? o_engine_client_cmd
                : reinterpret_cast<EngineClientCmd_t>(
                    (*reinterpret_cast<void***>(g_engine))[7]);
            fn(g_engine, cmd);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // ServerCmd — slot 6 on IVEngineClient (confirmed from the engine vtable
    // dump 2026-07-23: ev[6] is a clean adjustor thunk sitting right before
    // ClientCmd at ev[7], matching the SDK-2013 order ServerCmd(6)/ClientCmd(7)).
    // Signature: void ServerCmd(const char* sz, bool bReliable = true). The
    // mom_savestate_* commands are SERVER-registered, so this — not ClientCmd —
    // is what makes them fire (mirrors how the console forwards them).
    void execute_server_cmd(const char* cmd) {
        if (!g_engine || !cmd) return;
        __try {
            using ServerCmd_t = void(*)(void*, const char*, bool);
            auto fn = reinterpret_cast<ServerCmd_t>(
                (*reinterpret_cast<void***>(g_engine))[6]);
            fn(g_engine, cmd, true);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Two-pass origin finder. First pass: collect all Vector3 candidates.
    // Second pass (subsequent calls): pick the one whose values actually
    // change over time — that has to be the live m_vecOrigin.
    struct OriginCandidate {
        intptr_t offset;
        float x, y, z;
    };
    static std::vector<OriginCandidate> g_scan_candidates;
    static bool g_scan_first_pass_done = false;

    static bool scan_origin_offset(void* entity) {
        if (g_origin_offset >= 0) return true;
        if (!entity) return false;
        uint8_t* base = reinterpret_cast<uint8_t*>(entity);

        auto ok = [](float v) { return v == v && std::abs(v) < 1e6f; };

        if (!g_scan_first_pass_done) {
            for (intptr_t off = 0x130; off < 0x400; off += 4) {
                if (!region_readable(base + off, 12)) continue;
                float x = *reinterpret_cast<float*>(base + off);
                float y = *reinterpret_cast<float*>(base + off + 4);
                float z = *reinterpret_cast<float*>(base + off + 8);
                if (!ok(x) || !ok(y) || !ok(z)) continue;
                // Skip near-zero Vectors (uninitialized fields, mins/maxs at origin)
                if (std::abs(x) < 0.5f && std::abs(y) < 0.5f && std::abs(z) < 0.5f) continue;
                // Skip obvious bounding volumes like (-16, -16, 0) or (16, 16, 72)
                if (std::abs(x) < 100.f && std::abs(y) < 100.f && std::abs(z) < 100.f) continue;
                g_scan_candidates.push_back({ off, x, y, z });
            }
            g_scan_first_pass_done = true;
            log("origin scan: %zu candidate offsets", g_scan_candidates.size());
            return false;
        }

        // Second+ pass: pick the candidate with the LARGEST 3D delta once it
        // exceeds a walking-speed threshold. Playing walking speed is ~250
        // units/sec / 66 tick = ~3.8 units per tick, so anything ≥3 is a
        // real position update. Also require both x and y to be non-tiny —
        // typical map coordinates aren't near zero on either axis.
        intptr_t best_off = -1;
        float    best_delta = 3.0f;
        float    best_x=0, best_y=0, best_z=0;
        for (const auto& c : g_scan_candidates) {
            float x = *reinterpret_cast<float*>(base + c.offset);
            float y = *reinterpret_cast<float*>(base + c.offset + 4);
            float z = *reinterpret_cast<float*>(base + c.offset + 8);
            if (std::abs(x) < 5.f || std::abs(y) < 5.f) continue;
            float dx = x - c.x, dy = y - c.y, dz = z - c.z;
            float delta = std::sqrt(dx*dx + dy*dy + dz*dz);
            if (delta > best_delta) {
                best_delta = delta;
                best_off   = c.offset;
                best_x = x; best_y = y; best_z = z;
            }
        }
        if (best_off >= 0) {
            log("origin CONFIRMED @ 0x%llX  (delta=%.2f, pos=%.1f,%.1f,%.1f)",
                (long long)best_off, best_delta, best_x, best_y, best_z);
            g_origin_offset = best_off;
            g_scan_candidates.clear();
            return true;
        }
        return false;
    }

    // ─── netvar walker ──────────────────────────────────────────────
    // Walks the game's own network table so we get the *authoritative*
    // offset of m_vecOrigin regardless of build layout shifts.
    // Let the compiler handle alignment via natural padding — MSVC x64 will
    // insert 3 bytes after m_bInsideArray so the following pointer is 8-aligned.
    struct RecvProp_t {
        char*        m_pVarName;
        int          m_RecvType;
        int          m_Flags;
        int          m_StringBufferSize;
        bool         m_bInsideArray;
        // 3 bytes auto-pad
        void*        m_pExtraData;
        void*        m_pArrayProp;
        void*        m_ArrayLengthProxy;
        void*        m_ProxyFn;
        void*        m_DataTableProxyFn;
        void*        m_pDataTable;
        int          m_Offset;
        int          m_ElementStride;
        int          m_nElements;
        // 4 bytes auto-pad to align next pointer
        char*        m_pParentArrayPropName;
    };
    struct RecvTable_t {
        RecvProp_t*  m_pProps;
        int          m_nProps;
        char         _pad0[4];
        void*        m_pDecoder;
        char*        m_pNetTableName;
    };
    struct ClientClass_t {
        void*        m_pCreateFn;
        void*        m_pCreateEventFn;
        char*        m_pNetworkName;
        RecvTable_t* m_pRecvTable;
        ClientClass_t* m_pNext;
        int          m_ClassID;
    };

    static int walk_recv_table(RecvTable_t* table, const char* prop_name, int acc = 0) {
        if (!table) return -1;
        for (int i = 0; i < table->m_nProps; ++i) {
            RecvProp_t& p = table->m_pProps[i];
            if (p.m_pVarName && std::strcmp(p.m_pVarName, prop_name) == 0)
                return acc + p.m_Offset;
            if (p.m_pDataTable) {
                int r = walk_recv_table(reinterpret_cast<RecvTable_t*>(p.m_pDataTable),
                                        prop_name, acc + p.m_Offset);
                if (r >= 0) return r;
            }
        }
        return -1;
    }

    // GetAllClasses on IBaseClientDLL — sig-scan for it: iterate slots, whichever
    // returns a linked list where m_pNetworkName is a plausible C-string.
    static ClientClass_t* try_get_all_classes(int slot) {
        if (!g_client || !g_client_vtable) return nullptr;
        __try {
            using Fn = ClientClass_t*(*)(void*);
            auto fn = reinterpret_cast<Fn>(g_client_vtable[slot]);
            ClientClass_t* head = fn(g_client);
            if (!head || !region_readable(head, sizeof(ClientClass_t))) return nullptr;
            if (!head->m_pNetworkName || !region_readable(head->m_pNetworkName, 4)) return nullptr;
            // Check first char is alpha (class names start with 'C' or 'D')
            char c = head->m_pNetworkName[0];
            if (c < 'A' || c > 'Z') return nullptr;
            return head;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    // Forward-declared; defined further down alongside the offset globals.
    extern int g_cache_age_ticks;
    void bump_cache_age();
    void maybe_reset_caches();

    static int find_netvar_offset(const char* prefer_class_substr, const char* prop_name) {
        static ClientClass_t* head = nullptr;
        // Force re-lookup periodically (paired with maybe_reset_caches).
        static int last_reset_stamp = g_cache_age_ticks;
        if (g_cache_age_ticks == 0 && last_reset_stamp != 0) {
            head = nullptr;
            last_reset_stamp = 0;
        }
        if (!head) {
            for (int slot : { 8, 9, 10, 11, 7, 6, 12 }) {
                head = try_get_all_classes(slot);
                if (head) {
                    log("GetAllClasses at client vt[%d], head=%p (%s)",
                        slot, (void*)head,
                        head->m_pNetworkName ? head->m_pNetworkName : "?");
                    break;
                }
            }
            if (!head) { log("netvar: GetAllClasses not found in candidate slots"); return -1; }
        }

        // Multi-pass search — each pass narrows less than the previous.
        // The critical bug this fixes: an old pass-0 match on "Player"
        // grabbed CTEPlayerDecal (a temp-entity for footprint decals) whose
        // m_vecOrigin sits at 0x24 — nowhere near the real player's origin.
        // The result was position reads returning uninitialized garbage.
        //
        //   pass 0: name matches substr AND doesn't start with "CTE" or
        //           contain "Decal"/"Effect"/"Print" — real player-like
        //           entities (CCSPlayer, CBasePlayer, CHL2MP_Player, …).
        //   pass 1: name matches substr, no exclusions — fallback if 0 fails.
        //   pass 2: no filter — global fallback.
        auto is_temp_or_effect = [](const char* n) {
            if (!n) return true;
            if (n[0]=='C' && n[1]=='T' && n[2]=='E') return true; // CTE* = temp entity
            return std::strstr(n, "Decal")  || std::strstr(n, "Effect") ||
                   std::strstr(n, "Print")  || std::strstr(n, "Marker") ||
                   std::strstr(n, "Trace")  || std::strstr(n, "Sprite");
        };

        int max_pass = prefer_class_substr ? 3 : 1;
        for (int pass = 0; pass < max_pass; ++pass) {
            for (ClientClass_t* c = head; c; c = c->m_pNext) {
                if (!c->m_pNetworkName || !c->m_pRecvTable) continue;
                bool need_substr = (pass == 0 || pass == 1) && prefer_class_substr;
                if (need_substr &&
                    !std::strstr(c->m_pNetworkName, prefer_class_substr)) continue;
                if (pass == 0 && is_temp_or_effect(c->m_pNetworkName)) continue;
                int off = walk_recv_table(c->m_pRecvTable, prop_name);
                if (off >= 0) {
                    log("netvar found: class=%s prop=%s @ 0x%X (pass %d)",
                        c->m_pNetworkName, prop_name, off, pass);
                    return off;
                }
            }
        }
        log("netvar: %s not found", prop_name);
        return -1;
    }

    // Read the entity handle a spectator is following via m_hObserverTarget.
    // Returns nullptr if not spectating anyone.
    static intptr_t g_observer_target_off = -2; // -2 = unresolved, -1 = not found, >=0 = valid
    static intptr_t g_velocity_off        = -2;
    static intptr_t g_eyeangles_off       = -2;

    int  g_cache_age_ticks = 0;
    void bump_cache_age()  { ++g_cache_age_ticks; }
    void maybe_reset_caches() {
        constexpr int RESET_EVERY = 66 * 60 * 5;
        if (g_cache_age_ticks < RESET_EVERY) return;
        g_cache_age_ticks     = 0;
        g_origin_offset       = -1;
        g_observer_target_off = -2;
        g_velocity_off        = -2;
        g_eyeangles_off       = -2;
        log("hooks: cache-refresh triggered (5 min elapsed)");
    }

    static void* get_spectated_entity() {
        if (!g_engine || !g_ent_list || !g_client) return nullptr;
        __try {
            using GetLocal_t = int(*)(void*);
            auto get_local = reinterpret_cast<GetLocal_t>((*reinterpret_cast<void***>(g_engine))[12]);
            int idx = get_local(g_engine);
            if (idx <= 0) return nullptr;

            using GetEnt_t = void*(*)(void*, int);
            auto get_ent = reinterpret_cast<GetEnt_t>((*reinterpret_cast<void***>(g_ent_list))[3]);
            void* local = get_ent(g_ent_list, idx);
            if (!local) return nullptr;

            if (g_observer_target_off == -2)
                g_observer_target_off = find_netvar_offset("Player", "m_hObserverTarget");
            if (g_observer_target_off < 0) return nullptr;

            uint32_t handle = *reinterpret_cast<uint32_t*>(
                reinterpret_cast<uint8_t*>(local) + g_observer_target_off);
            if (handle == 0xFFFFFFFFu || handle == 0) return nullptr;
            int target_idx = static_cast<int>(handle & 0xFFF);
            if (target_idx == idx) return nullptr; // spectating self / freecam
            return get_ent(g_ent_list, target_idx);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    bool read_spectated_inputs(SpecInputs& out) {
        out = {};
        void* target = get_spectated_entity();
        static int diag = 10;
        if (diag > 0) { --diag; log("spec: target entity = %p", target); }
        if (!target) return false;
        __try {
            if (g_velocity_off == -2)
                g_velocity_off = find_netvar_offset("Player", "m_vecVelocity[0]");
            if (g_eyeangles_off == -2)
                g_eyeangles_off = find_netvar_offset("Player", "m_angEyeAngles[0]");
            if (g_velocity_off < 0 || g_eyeangles_off < 0) {
                if (diag > 0) log("spec: velocity_off=%lld eye_off=%lld — one missing",
                    (long long)g_velocity_off, (long long)g_eyeangles_off);
                return false;
            }

            float* vel = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(target) + g_velocity_off);
            float* eye = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(target) + g_eyeangles_off);
            float vx = vel[0], vy = vel[1], vz = vel[2];
            float pitch = eye[0], yaw = eye[1];

            if (diag > 0) {
                log("spec: vel=(%.1f,%.1f,%.1f) eye=(pitch=%.1f, yaw=%.1f)",
                    vx, vy, vz, pitch, yaw);
            }

            // Reject NaN
            auto nan = [](float v) { return v != v; };
            if (nan(vx) || nan(vy) || nan(vz) || nan(pitch) || nan(yaw)) return false;

            // Project world velocity onto view directions (yaw only for horizontal).
            const float d2r = 0.017453292519943295f;
            float cy = std::cos(yaw * d2r);
            float sy = std::sin(yaw * d2r);
            float fw = vx * cy + vy * sy;
            float sd = vx * sy - vy * cy;
            // Clamp to cmd's valid range so the server doesn't reject a spike
            // (surf ramps and jumps hit velocity ~1000+, way above walk max).
            auto clamp = [](float v) {
                if (v >  450.f) return  450.f;
                if (v < -450.f) return -450.f;
                return v;
            };
            out.forwardmove = clamp(fw);
            out.sidemove    = clamp(sd);
            out.upmove      = clamp(vz);
            out.viewangles  = { pitch, yaw, 0.f };
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Local-player entity resolution. GetLocalPlayer was slot 12 on SDK 2013
    // MP's IVEngineClient, but Strata shifted the vtable — the old direct
    // call returned garbage and every position/velocity/flags getter bailed
    // silently, which is why drift telemetry read 0.0 on Momentum. Slot 12's
    // answer is now VALIDATED (plausible index + resolvable entity) before
    // being trusted; otherwise fall back to a low-index sweep — offline on a
    // listen server the local player is always one of the first entities.
    static void* resolve_local_player() {
        if (!g_engine || !g_ent_list) return nullptr;
        using GetLocal_t = int(*)(void*);
        using GetEnt_t   = void*(*)(void*, int);
        auto get_ent = reinterpret_cast<GetEnt_t>(
            (*reinterpret_cast<void***>(g_ent_list))[3]);

        static bool s_logged = false;
        auto get_local = reinterpret_cast<GetLocal_t>(
            (*reinterpret_cast<void***>(g_engine))[12]);
        int idx = get_local(g_engine);
        if (idx >= 1 && idx <= 128) {
            void* e = get_ent(g_ent_list, idx);
            if (e) {
                if (!s_logged) { s_logged = true;
                    log("local player: GetLocalPlayer(slot12) idx=%d ent=%p", idx, e); }
                return e;
            }
        }
        for (int i = 1; i <= 4; ++i) {
            void* e = get_ent(g_ent_list, i);
            if (e) {
                if (!s_logged) { s_logged = true;
                    log("local player: low-index fallback idx=%d ent=%p (slot12 gave %d)", i, e, idx); }
                return e;
            }
        }
        return nullptr;
    }

    // Non-static bridge so the file-scope viewangle memory scanner (defined
    // above the namespace) can reach the local-player entity safely.
    void* resolve_local_player_bridge() { return resolve_local_player(); }

    bool get_local_player_pos(Vec3& out) {
        out = { 0, 0, 0 };
        if (!g_engine || !g_ent_list) return false;
        __try {
            void* entity = resolve_local_player();
            if (!entity) return false;

            if (g_origin_offset < 0) {
                // Try netvar walker first — the reliable path.
                int off = find_netvar_offset("Player", "m_vecOrigin");
                if (off >= 0) {
                    g_origin_offset = off;
                    log("origin offset locked via netvar: 0x%X", off);
                } else if (!scan_origin_offset(entity)) {
                    return false;
                }
            }

            float* origin = reinterpret_cast<float*>(
                reinterpret_cast<uint8_t*>(entity) + g_origin_offset);
            out.x = origin[0];
            out.y = origin[1];
            out.z = origin[2];
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Cached m_vecVelocity offset — one-shot netvar lookup, same pattern as
    // origin. Used by the strafe optimizer to steer sidemove toward the
    // side that maximizes air-accel gain.
    static intptr_t g_vel_offset = -1;

    bool get_local_player_vel(Vec3& out) {
        out = { 0, 0, 0 };
        if (!g_engine || !g_ent_list) return false;
        __try {
            void* entity = resolve_local_player();
            if (!entity) return false;

            if (g_vel_offset < 0) {
                // Netvar name on CCSPlayer is m_vecVelocity[0] (array-indexed),
                // NOT m_vecVelocity — the shorter name only matches temp
                // entities like CTEPhysicsProp at a completely wrong offset.
                int off = find_netvar_offset("Player", "m_vecVelocity[0]");
                if (off < 0) return false;
                g_vel_offset = off;
                log("velocity offset locked via netvar: 0x%X", off);
            }

            float* v = reinterpret_cast<float*>(
                reinterpret_cast<uint8_t*>(entity) + g_vel_offset);
            out.x = v[0];
            out.y = v[1];
            out.z = v[2];
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Cached m_fFlags offset — one-shot netvar lookup, same pattern as
    // origin / velocity above.
    static intptr_t g_flags_offset = -1;

    bool get_local_player_flags(int& out) {
        out = 0;
        if (!g_engine || !g_ent_list) return false;
        __try {
            void* entity = resolve_local_player();
            if (!entity) return false;

            if (g_flags_offset < 0) {
                int off = find_netvar_offset("Player", "m_fFlags");
                if (off < 0) return false;
                g_flags_offset = off;
                log("m_fFlags offset locked via netvar: 0x%X", off);
            }

            out = *reinterpret_cast<int*>(
                reinterpret_cast<uint8_t*>(entity) + g_flags_offset);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

}

// Definition of the ClientCmd trampoline pointer + user-typed chat interceptor.
hooks::EngineClientCmd_t hooks::o_engine_client_cmd = nullptr;

void __fastcall hk_engine_client_cmd(void* self, const char* cmd) {
    __try {
        if (cmd) {
            // Trace every command that hits us so we can see whether user
            // binds actually reach this hook. First 8 calls per second only,
            // to keep the log from exploding when the game spams input cmds.
            static int trace_budget = 0;
            static ULONGLONG trace_window = 0;
            ULONGLONG now = GetTickCount64();
            if (now - trace_window > 1000) { trace_window = now; trace_budget = 0; }
            if (trace_budget < 8) { hooks::log("ClientCmd: %s", cmd); ++trace_budget; }

            // Detect Momentum savestate concommands regardless of mode so
            // user-bound presses (MOUSE4/MOUSE5 defaults, custom binds)
            // still populate the tape's event stream.
            //   savestate_create        → Saveloc
            //   savestate_load          → Teleport (to current)
            //   savestate_prev / _next  → also treated as Teleport family;
            //                             the recorder distinguishes those
            //                             via its own hotkey path anyway.
            bool save = std::strstr(cmd, "savestate_create") != nullptr;
            bool tele = !save && std::strstr(cmd, "savestate_") != nullptr;
            if (save || tele) {
                auto m = recorder::mode();
                const char* mstr =
                    m == recorder::Mode::Recording ? "REC" :
                    m == recorder::Mode::Playing   ? "PLAY" :
                    m == recorder::Mode::Idle      ? "IDLE" : "OTHER";
                hooks::log("  -> match %s (mode=%s) cmd='%s'", save ? "SAVELOC" : "TELEPORT", mstr, cmd);
                // Bump the counter for EVERY savestate_create we see, regardless
                // of mode — it tracks the session-wide savestate slot number
                // and needs to stay in sync even outside recording.
                if (save) recorder::note_saveloc_fired();
                if (m == recorder::Mode::Recording) {
                    recorder::record_event(
                        save ? recorder::EventType::Saveloc : recorder::EventType::Teleport,
                        cmd);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (hooks::o_engine_client_cmd) hooks::o_engine_client_cmd(self, cmd);
}
