param(
    [string]$DllBasename = "momentum_menu.dll",
    [string]$ProcName    = "momentum"
)

$typeDef = @"
using System;
using System.Runtime.InteropServices;
public static class Unloader {
    [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr GetModuleHandleA(string name);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr GetProcAddress(IntPtr hMod, string name);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr CreateRemoteThread(IntPtr hProc, IntPtr sec, UIntPtr stack, IntPtr start, IntPtr param, uint flags, IntPtr threadId);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern int WaitForSingleObject(IntPtr h, int ms);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool GetExitCodeThread(IntPtr h, out uint code);
    [DllImport("psapi.dll",   SetLastError=true)] public static extern bool EnumProcessModulesEx(IntPtr hProc, [In,Out] IntPtr[] mods, uint cb, out uint needed, uint filter);
    [DllImport("psapi.dll",   SetLastError=true, CharSet=CharSet.Ansi)] public static extern uint GetModuleFileNameExA(IntPtr hProc, IntPtr hMod, System.Text.StringBuilder name, uint size);

    public static IntPtr Find(IntPtr hProc, string basenameLower) {
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

    public static int UnloadAll(int pid, string basenameLower) {
        IntPtr h = OpenProcess(0x1F0FFFu, false, pid);
        if (h == IntPtr.Zero) return 0;
        IntPtr freeLib = GetProcAddress(GetModuleHandleA("kernel32.dll"), "FreeLibrary");
        int count = 0;
        for (int i = 0; i < 32; ++i) {
            IntPtr mod = Find(h, basenameLower);
            if (mod == IntPtr.Zero) break;
            IntPtr t = CreateRemoteThread(h, IntPtr.Zero, UIntPtr.Zero, freeLib, mod, 0u, IntPtr.Zero);
            if (t == IntPtr.Zero) break;
            WaitForSingleObject(t, 5000);
            uint code = 0; GetExitCodeThread(t, out code); CloseHandle(t);
            if (code == 0) break;
            count++;
        }
        CloseHandle(h);
        return count;
    }
}
"@

Add-Type -TypeDefinition $typeDef -Language CSharp

$procs = Get-Process $ProcName -ErrorAction SilentlyContinue
if (-not $procs) { Write-Host "Process not running." -ForegroundColor Yellow; exit 0 }
$proc = $procs | Select-Object -First 1
$freed = [Unloader]::UnloadAll($proc.Id, $DllBasename.ToLower())
Write-Host "  freed $freed handle(s) of $DllBasename from PID $($proc.Id)" -ForegroundColor Yellow
Start-Sleep -Milliseconds 300
