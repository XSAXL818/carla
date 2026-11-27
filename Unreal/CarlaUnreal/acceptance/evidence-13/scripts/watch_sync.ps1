# Watch /sessions/current while synchronous mode is being stolen, and capture the
# panel the moment the backend reports it lost the clock.
# Usage: .\watch_sync.ps1 -Seconds 40 [-EvidenceDir acceptance\evidence-13]
param(
    [double]$Seconds = 40.0,
    [string]$EvidenceDir = "acceptance\evidence-13",
    [string]$OutPrefix = "05-sync-lost"
)

$capture = Join-Path $PSScriptRoot "capture_panel.ps1"
$started = Get-Date
$seen = @{}
$rows = @()
$shot = 0
while (((Get-Date) - $started).TotalSeconds -lt $Seconds) {
    try {
        $s = Invoke-RestMethod "http://127.0.0.1:8765/sessions/current" -TimeoutSec 2
    } catch {
        Write-Output ("t={0,5:N1} /sessions/current unreachable: {1}" -f ((Get-Date) - $started).TotalSeconds, $_.Exception.Message)
        Start-Sleep -Milliseconds 200
        continue
    }
    $t = [math]::Round(((Get-Date) - $started).TotalSeconds, 1)
    $key = "{0}|{1}|{2}" -f $s.state, $s.sync_mode, $s.sync_lost
    $rows += [pscustomobject]@{
        t = $t; state = $s.state; sync_mode = $s.sync_mode; sync_lost = $s.sync_lost
        fixed_delta = $s.fixed_delta; tick = $s.tick; sim_time = $s.sim_time
        events = $s.events; reasserts = $s.sync_reasserts
    }
    if (-not $seen.ContainsKey($key)) {
        $seen[$key] = $t
        Write-Output ("t={0,5:N1} state={1} sync_mode={2} sync_lost={3} delta={4} reasserts={5} events={6}" -f `
            $t, $s.state, $s.sync_mode, $s.sync_lost, $s.fixed_delta, $s.sync_reasserts, $s.events)
    }
    if ($s.sync_lost -eq $true) {
        $shot++
        $out = Join-Path $EvidenceDir ("{0}-{1}-t{2}.png" -f $OutPrefix, $shot, $t)
        try { & $capture -Out $out } catch { Write-Output "capture failed: $($_.Exception.Message)" }
        Write-Output ("t={0,5:N1} CAPTURED sync_lost -> {1}" -f $t, $out)
        if ($shot -ge 3) { break }
    }
    Start-Sleep -Milliseconds 200
}
$rows | ConvertTo-Csv | Set-Content (Join-Path $EvidenceDir "$OutPrefix-polls.csv") -Encoding UTF8
Write-Output ("watcher done, {0} polls, {1} captures" -f $rows.Count, $shot)
