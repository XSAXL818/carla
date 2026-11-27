# Launch the CARLA test platform in the editor "uncooked" path, tuned for a
# machine that cannot afford UE5's default renderer.
#
# The editor binary is the only way to run uncooked content, so it stays heavy;
# this script claws back what can be clawed back: Lumen and virtual shadow maps
# off, screen percentage down, 30 fps cap, backend and stale processes handled.
#
# With no -Map the run uses the project's GameDefaultMap, which is the
# test-platform entry level (Content/TestPlatform/Maps/L_TestPlatformEntry), so
# the config panel appears. Any other map runs without it: the panel is gated to
# the entry maps listed in DefaultGame.ini.
#
#   pwsh -File run-editor-lowquality.ps1              # 1280x720, 70%, entry level
#   pwsh -File run-editor-lowquality.ps1 -ResX 1600 -ResY 900 -Screen 50
#   pwsh -File run-editor-lowquality.ps1 -Map /Game/Carla/Maps/Town05_Opt   # browse a town, no panel
#   pwsh -File run-editor-lowquality.ps1 -Backend 127.0.0.1:8791   # non-default backend endpoint
param(
    [int]$ResX = 1280,
    [int]$ResY = 720,
    [int]$Screen = 70,
    [switch]$HighQuality,
    [switch]$ShowFps,
    [string]$Backend = 'http://127.0.0.1:8765',
    # empty = the project default (the entry level); a package path or map name
    # here overrides it, and the config panel stays away unless the map is
    # whitelisted.
    [string]$Map = ''
)

# Normalisation follows FTestPlatformApi::RootUrl(): trim, add http:// when the
# scheme is missing, drop the trailing slash. It is deliberately stricter on two
# points: RootUrl() keeps the default base when nothing usable is left (e.g.
# "http://"), and it hands a value that is not a URL at all (e.g. "not a url")
# to the HTTP layer verbatim; here both fail loudly instead - the URI check below
# rejects anything that is not an absolute URL with a host. The endpoint was
# typed by an operator, so a typo must stop the launch rather than silently run
# against a backend that was not asked for; and whatever is probed, exported to
# the child process and passed as -TestPlatformBackend= is this one string.
function ConvertTo-BackendUrl {
    param([string]$Url)
    $text = "$Url".Trim()
    if (-not $text.Contains('://')) { $text = "http://$text" }
    $text = $text.TrimEnd('/')
    if ([string]::IsNullOrWhiteSpace($text) -or $text.EndsWith(':')) {
        throw "invalid -Backend value: '$Url'"
    }
    return $text
}

$backendUrl = ConvertTo-BackendUrl $Backend
$backendUri = $null
if (-not [System.Uri]::TryCreate($backendUrl, [System.UriKind]::Absolute, [ref]$backendUri) -or
    -not $backendUri.IsAbsoluteUri -or $backendUri.Host -eq '') {
    throw "invalid -Backend value: '$Backend'"
}
$backendHost = $backendUri.Host
$backendPort = $backendUri.Port

$ErrorActionPreference = 'Continue'
$engine = 'G:\Epic\EpicApps\UE_5.8_Carla\Engine\Binaries\Win64\UnrealEditor.exe'
$project = 'C:\GameProject\CarlaUE5\carla\Unreal\CarlaUnreal\CarlaUnreal.uproject'
$backendDir = 'C:\GameProject\CarlaUE5\carla\Unreal\CarlaUnreal\Source\TestPlatform\Platform\backend'
$python = 'C:\SoftWare\MiniConda\envs\chat_carla\python.exe'

foreach ($path in @($engine, $project, $backendDir, $python)) {
    if (-not (Test-Path $path)) { throw "missing: $path" }
}

# one server at a time: these hold CARLA's :2000 and the backend endpoint, and lock the pak files
Get-Process UnrealEditor, LiveCodingConsole, CarlaUnreal -ErrorAction SilentlyContinue |
    ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 3

# the FastAPI backend must be listening before the panel polls it
$healthy = $false
try {
    $null = Invoke-WebRequest "$backendUrl/health" -TimeoutSec 3
    $healthy = $true
} catch { }
if (-not $healthy) {
    # run.py reads TESTPLATFORM_HOST/PORT at import time; the child inherits them
    $previousHost = $env:TESTPLATFORM_HOST
    $previousPort = $env:TESTPLATFORM_PORT
    $env:TESTPLATFORM_HOST = $backendHost
    $env:TESTPLATFORM_PORT = "$backendPort"
    try {
        Start-Process -FilePath $python -ArgumentList 'run.py' -WorkingDirectory $backendDir -WindowStyle Hidden
    } finally {
        $env:TESTPLATFORM_HOST = $previousHost
        $env:TESTPLATFORM_PORT = $previousPort
    }
    Start-Sleep -Seconds 6
}

# a session left over from a killed server stays in "error" and greets the panel
# with a dead card, so clear it before the UI loads
try {
    $current = (Invoke-WebRequest "$backendUrl/sessions/current" -TimeoutSec 5).Content |
        ConvertFrom-Json
    if ($current.state -eq 'error') {
        $null = Invoke-WebRequest "$backendUrl/sessions/$($current.id)/destroy" `
            -Method Post -TimeoutSec 20 -SkipHttpErrorCheck
        Write-Host "cleared the stale error session $($current.id)" -ForegroundColor DarkYellow
    }
} catch { }

$cvars = @(
    'sg.ViewDistanceQuality 0', 'sg.ShadowQuality 0', 'sg.PostProcessQuality 0',
    'sg.TextureQuality 0', 'sg.EffectsQuality 0', 'sg.FoliageQuality 0',
    'sg.ShadingQuality 0', 'sg.AntiAliasingQuality 0',
    'r.DynamicGlobalIlluminationMethod 0',   # no Lumen GI
    'r.ReflectionMethod 0',                  # no Lumen reflections
    'r.Shadow.Virtual.Enable 0',             # no virtual shadow maps
    'r.AntiAliasingMethod 0',
    'r.SSR.Quality 0', 'r.AmbientOcclusionLevels 0',
    'r.BloomQuality 0', 'r.MotionBlurQuality 0', 'r.DepthOfFieldQuality 0',
    't.MaxFPS 30',
    'stat fps',
    'stat unit'
    "r.ScreenPercentage $Screen"
)
if ($HighQuality) { $cvars = @("r.ScreenPercentage $Screen") }

$arguments = @(
    $project, '-game', '-windowed', "-ResX=$ResX", "-ResY=$ResY", '-nosound', '-nologtimes',
    "-TestPlatformBackend=$backendUrl"
) + @($Map | Where-Object { $_ -ne '' }) + @(
    "-ExecCmds=$($cvars -join ',')"
)

Write-Host "launching CARLA (editor/game path) ${ResX}x${ResY} screen=$Screen map=$(if ($Map -eq '') { 'project default (entry level)' } else { $Map })" -ForegroundColor Cyan
$editor = Start-Process -FilePath $engine -ArgumentList $arguments -PassThru

$logs = Join-Path (Split-Path $project) 'Saved\Logs'
$deadline = (Get-Date).AddMinutes(8)
$ready = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 5
    $log = Get-ChildItem $logs -Filter 'CarlaUnreal*.log' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($log -and (Select-String -Path $log.FullName -Pattern 'test platform UI added' -Quiet)) {
        $ready = $true
        break
    }
    if ($editor.HasExited) { break }
}

if ($ready) {
    $session = $null
    try {
        $session = (Invoke-WebRequest "$backendUrl/sessions/current" -TimeoutSec 10).Content |
            ConvertFrom-Json
    } catch { }
    Write-Host "UI is up (pid $($editor.Id)); session state: $($session.state) map: $($session.map)" -ForegroundColor Green
    Write-Host "press Ctrl+C in this window to leave it running, or close the game window to stop." -ForegroundColor DarkGray
} else {
    Write-Host "the UI marker never appeared - check $logs" -ForegroundColor Red
}


