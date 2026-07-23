#include <Windows.h>
#include "hooks.h"

static HMODULE g_self = nullptr;

static DWORD WINAPI init_thread(LPVOID) {
    if (!hooks::install()) {
        FreeLibraryAndExitThread(g_self, 0);
    }
    return 0;
}

static DWORD WINAPI unload_thread(LPVOID) {
    hooks::uninstall();
    Sleep(100); // let any in-flight hooked calls settle
    FreeLibraryAndExitThread(g_self, 0);
    return 0;
}

namespace hooks { void request_unload(); }

// exposed to menu button
namespace hooks {
    void request_unload() {
        HANDLE t = CreateThread(nullptr, 0, unload_thread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
}

BOOL WINAPI DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = mod;
        DisableThreadLibraryCalls(mod);
        if (HANDLE t = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr))
            CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH) {
        hooks::uninstall();
    }
    return TRUE;
}
