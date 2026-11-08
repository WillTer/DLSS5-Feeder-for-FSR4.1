param([string]$ProcName = 'SpellForce')
Add-Type @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class WinEnum {
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern int GetWindowText(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll")] static extern int GetClassName(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, StringBuilder l);
    delegate bool EnumProc(IntPtr h, IntPtr lp);
    static uint want;
    public static List<string> Result = new List<string>();
    static bool Child(IntPtr h, IntPtr lp) {
        var sb = new StringBuilder(512);
        SendMessage(h, 0x000D, (IntPtr)511, sb);   // WM_GETTEXT
        var cn = new StringBuilder(256); GetClassName(h, cn, 256);
        if (sb.Length > 0) Result.Add("    [" + cn + "] " + sb);
        return true;
    }
    static bool Top(IntPtr h, IntPtr lp) {
        uint pid; GetWindowThreadProcessId(h, out pid);
        if (pid != want || !IsWindowVisible(h)) return true;
        var sb = new StringBuilder(512); GetWindowText(h, sb, 512);
        var cn = new StringBuilder(256); GetClassName(h, cn, 256);
        Result.Add("WINDOW class=" + cn + " title='" + sb + "'");
        EnumChildWindows(h, Child, IntPtr.Zero);
        return true;
    }
    public static List<string> For(uint processId) {
        want = processId; Result = new List<string>();
        EnumWindows(Top, IntPtr.Zero);
        return Result;
    }
}
"@
$p = Get-Process -Name $ProcName -ErrorAction Stop
[WinEnum]::For([uint32]$p.Id) | ForEach-Object { $_ }
