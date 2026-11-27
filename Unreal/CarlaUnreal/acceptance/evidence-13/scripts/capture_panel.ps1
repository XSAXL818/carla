# Capture the client area of the editor's game window (PrintWindow, client only).
# All Win32 work happens in C#: PowerShell turns a returned IntPtr.Zero into
# $null, which makes a pure-PowerShell window enumeration loop forever.
# The window handle comes from the UnrealEditor process' MainWindowHandle: with
# -game the game window is the process' main window.
# Usage: powershell -File .\capture_panel.ps1 -Out <path.png> [-ProcessName UnrealEditor]
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$ProcessName = "UnrealEditor"
)

Add-Type -AssemblyName System.Drawing

if (-not ("PanelCap" -as [type])) {
Add-Type -ReferencedAssemblies System.Drawing @"
using System;
using System.Text;
using System.Diagnostics;
using System.Runtime.InteropServices;

public class PanelCap
{
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT r);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr hwnd, StringBuilder s, int n);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }

    static string Title(IntPtr h)
    {
        var sb = new StringBuilder(512);
        GetWindowText(h, sb, sb.Capacity);
        return sb.ToString();
    }

    public static string Capture(string processName, string outPath)
    {
        Process[] procs = Process.GetProcessesByName(processName);
        if (procs.Length == 0) throw new Exception("no process named '" + processName + "'");
        IntPtr h = IntPtr.Zero;
        foreach (Process p in procs)
        {
            p.Refresh();
            if (p.MainWindowHandle != IntPtr.Zero) { h = p.MainWindowHandle; break; }
        }
        if (h == IntPtr.Zero) throw new Exception("process '" + processName + "' has no main window");
        RECT r;
        GetClientRect(h, out r);
        int w = r.Right - r.Left;
        int hh = r.Bottom - r.Top;
        if (w <= 0 || hh <= 0) throw new Exception("empty client rect " + w + "x" + hh);
        using (var bmp = new System.Drawing.Bitmap(w, hh))
        using (var g = System.Drawing.Graphics.FromImage(bmp))
        {
            IntPtr hdc = g.GetHdc();
            bool ok = PrintWindow(h, hdc, 3); // PW_CLIENTONLY | PW_RENDERFULLCONTENT
            g.ReleaseHdc(hdc);
            bmp.Save(outPath, System.Drawing.Imaging.ImageFormat.Png);
            return string.Format("captured '{0}' {1}x{2} PrintWindow={3} -> {4}", Title(h), w, hh, ok, outPath);
        }
    }
}
"@
}

$dir = Split-Path -Parent $Out
if (-not $dir) { $dir = "." }
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
$full = (Resolve-Path -LiteralPath $dir).Path + "\" + (Split-Path -Leaf $Out)
Write-Output ([PanelCap]::Capture($ProcessName, $full))
