param(
    [string]$Dll = "",
    [string]$ProcName = "momentum"
)

# Default to the DLL sitting right next to this script, so the folder is
# self-contained and portable (no dependency on build\Release\).
if (-not $Dll) { $Dll = Join-Path $PSScriptRoot "momentum_menu.dll" }

$typeDef = @"
using System;
using System.Runtime.InteropServices;
public static class Injector {
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr GetModuleHandleA(string name);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr GetProcAddress(IntPtr hMod, string name);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr VirtualAllocEx(IntPtr hProc, IntPtr addr, UIntPtr size, uint alloc, uint protect);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool WriteProcessMemory(IntPtr hProc, IntPtr addr, byte[] data, UIntPtr size, out UIntPtr written);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr CreateRemoteThread(IntPtr hProc, IntPtr sec, UIntPtr stack, IntPtr start, IntPtr param, uint flags, IntPtr threadId);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern int WaitForSingleObject(IntPtr h, int ms);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool GetExitCodeThread(IntPtr h, out uint code);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool VirtualFreeEx(IntPtr hProc, IntPtr addr, UIntPtr size, uint type);

    // psapi
    [DllImport("psapi.dll", SetLastError=true)]
    public static extern bool EnumProcessModulesEx(IntPtr hProc, [In,Out] IntPtr[] mods, uint cb, out uint needed, uint filter);
    [DllImport("psapi.dll", SetLastError=true, CharSet=CharSet.Ansi)]
    public static extern uint GetModuleFileNameExA(IntPtr hProc, IntPtr hMod, System.Text.StringBuilder name, uint size);

    // Ask the target process to run `func(param)` synchronously and return the exit code.
    public static uint RemoteCall(IntPtr hProc, IntPtr func, IntPtr param) {
        IntPtr t = CreateRemoteThread(hProc, IntPtr.Zero, UIntPtr.Zero, func, param, 0u, IntPtr.Zero);
        if (t == IntPtr.Zero) return 0;
        WaitForSingleObject(t, 5000);
        uint code = 0;
        GetExitCodeThread(t, out code);
        CloseHandle(t);
        return code;
    }

    public static IntPtr FindLoadedDll(IntPtr hProc, string basenameLower) {
        IntPtr[] mods = new IntPtr[1024];
        uint needed;
        if (!EnumProcessModulesEx(hProc, mods, (uint)(mods.Length * IntPtr.Size), out needed, 3u)) return IntPtr.Zero;
        int n = (int)(needed / IntPtr.Size);
        if (n > mods.Length) n = mods.Length;
        for (int i = 0; i < n; ++i) {
            var sb = new System.Text.StringBuilder(1024);
            if (GetModuleFileNameExA(hProc, mods[i], sb, 1024) == 0) continue;
            string full = sb.ToString().ToLower();
            int slash = full.LastIndexOfAny(new[] { '\\', '/' });
            string basename = slash >= 0 ? full.Substring(slash + 1) : full;
            if (basename == basenameLower) return mods[i];
        }
        return IntPtr.Zero;
    }

    public static int ForceUnload(int pid, string dllBasenameLower) {
        IntPtr h = OpenProcess(0x1F0FFFu, false, pid);
        if (h == IntPtr.Zero) return -1;
        IntPtr kernel32 = GetModuleHandleA("kernel32.dll");
        IntPtr freeLib  = GetProcAddress(kernel32, "FreeLibrary");
        int count = 0;
        for (int i = 0; i < 32; ++i) {
            IntPtr mod = FindLoadedDll(h, dllBasenameLower);
            if (mod == IntPtr.Zero) break;
            uint code = RemoteCall(h, freeLib, mod);
            if (code == 0) break;
            count++;
        }
        CloseHandle(h);
        return count;
    }

    public static int Inject(int pid, string dll) {
        IntPtr h = OpenProcess(0x1F0FFFu, false, pid);
        if (h == IntPtr.Zero) return -1;
        byte[] bytes = System.Text.Encoding.ASCII.GetBytes(dll + "\0");
        UIntPtr sz = new UIntPtr((uint)bytes.Length);
        IntPtr mem = VirtualAllocEx(h, IntPtr.Zero, sz, 0x3000u, 0x04u);
        if (mem == IntPtr.Zero) { CloseHandle(h); return -2; }
        UIntPtr written;
        if (!WriteProcessMemory(h, mem, bytes, sz, out written)) { VirtualFreeEx(h, mem, UIntPtr.Zero, 0x8000u); CloseHandle(h); return -3; }
        IntPtr loadLib = GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
        if (loadLib == IntPtr.Zero) { VirtualFreeEx(h, mem, UIntPtr.Zero, 0x8000u); CloseHandle(h); return -4; }
        uint code = RemoteCall(h, loadLib, mem);
        VirtualFreeEx(h, mem, UIntPtr.Zero, 0x8000u);
        CloseHandle(h);
        return (code != 0) ? 0 : -5;
    }
}
"@

Add-Type -TypeDefinition $typeDef -Language CSharp

if (-not (Test-Path $Dll)) {
    Write-Host "DLL not found at: $Dll" -ForegroundColor Red
    exit 1
}

$procs = Get-Process $ProcName -ErrorAction SilentlyContinue
if (-not $procs) {
    Write-Host "Process '$ProcName.exe' is not running." -ForegroundColor Red
    exit 1
}

$proc = $procs | Select-Object -First 1
$dllBase = ([System.IO.Path]::GetFileName($Dll)).ToLower()

Write-Host "Force-unloading any prior copies of $dllBase from PID=$($proc.Id) ..." -ForegroundColor Yellow
$freed = [Injector]::ForceUnload($proc.Id, $dllBase)
Write-Host "  FreeLibrary calls that returned non-zero: $freed" -ForegroundColor Yellow
Start-Sleep -Milliseconds 300

Write-Host "Injecting $Dll ..." -ForegroundColor Cyan
$result = [Injector]::Inject($proc.Id, $Dll)
if ($result -eq 0) {
    Write-Host "OK - DLL loaded." -ForegroundColor Green
    exit 0
} else {
    Write-Host "Injection failed with code $result (LastError: $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error()))" -ForegroundColor Red
    exit 1
}
