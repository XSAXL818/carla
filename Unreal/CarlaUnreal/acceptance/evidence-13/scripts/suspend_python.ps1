# Suspend / resume the backend's python process (NtSuspendProcess), which keeps
# its TCP connection to the simulator open: the "live client is not ticking"
# case, not the "client is gone" case.
# Usage: .\suspend_python.ps1 -Seconds 70 [-SecondsToWaitAfter 1]
param(
    [Parameter(Mandatory = $true)][double]$Seconds,
    [string]$CommandLineMatch = "run.py"
)

if (-not ("NtProc" -as [type])) {
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class NtProc {
    [DllImport("ntdll.dll")] public static extern int NtSuspendProcess(IntPtr h);
    [DllImport("ntdll.dll")] public static extern int NtResumeProcess(IntPtr h);
    [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
}
"@
}

$procs = Get-CimInstance Win32_Process -Filter "Name='python.exe'" |
    Where-Object { $_.CommandLine -and $_.CommandLine.Contains($CommandLineMatch) }
if (-not $procs) { throw "no python.exe whose command line contains '$CommandLineMatch'" }
$targets = @($procs)
foreach ($p in $targets) { Write-Output ("target pid={0} cmd={1}" -f $p.ProcessId, $p.CommandLine) }

$PROCESS_SUSPEND_RESUME = 0x0800
$handles = @()
foreach ($p in $targets) {
    $h = [NtProc]::OpenProcess($PROCESS_SUSPEND_RESUME, $false, $p.ProcessId)
    if ($h -eq [IntPtr]::Zero) { throw "OpenProcess failed for pid $($p.ProcessId)" }
    $handles += $h
    $rc = [NtProc]::NtSuspendProcess($h)
    Write-Output ("suspended pid={0} rc={1} at {2}" -f $p.ProcessId, $rc, (Get-Date -Format o))
}
Start-Sleep -Seconds $Seconds
foreach ($h in $handles) {
    $rc = [NtProc]::NtResumeProcess($h)
    Write-Output ("resumed rc={0} at {1}" -f $rc, (Get-Date -Format o))
    [void][NtProc]::CloseHandle($h)
}
