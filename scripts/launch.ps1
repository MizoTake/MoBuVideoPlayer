[CmdletBinding()]
param(
    [string]$MotionBuilderRoot = $env:MOBU_ROOT,
    [string]$PluginDirectory = '',
    [string]$StartupScript = '',
    [switch]$SmokeTest,
    [switch]$StorySmokeTest,
    [switch]$NewWindow,
    [switch]$KeepOpen
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$existingHosts = @(Get-Process motionbuilder -ErrorAction SilentlyContinue)
if ($KeepOpen -and -not $StorySmokeTest) { throw '-KeepOpen requires -StorySmokeTest.' }
if ($existingHosts.Count -and -not $NewWindow) {
    $hostIds = ($existingHosts | ForEach-Object Id) -join ', '
    if ($SmokeTest -or $StorySmokeTest -or $StartupScript) {
        throw "Existing MotionBuilder found (PID: $hostIds). No process was started. Run the script in the existing Python Editor; smoke scripts that reset a scene require an isolated scene."
    }
    Write-Output "Using existing MotionBuilder window (PID: $hostIds). No process was started."
    return
}
if (-not $MotionBuilderRoot) {
    $MotionBuilderRoot = Join-Path $env:ProgramFiles 'Autodesk/MotionBuilder 2026'
}
if (-not $PluginDirectory) {
    $PluginDirectory = Join-Path $projectRoot 'plugins'
    if (-not (Test-Path -LiteralPath (Join-Path $PluginDirectory 'MoBuVideoPlayer.dll'))) {
        $PluginDirectory = Join-Path $projectRoot 'build/package/plugins'
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $PluginDirectory 'MoBuVideoPlayer.dll'))) {
    throw 'Build the plug-in first, or specify -PluginDirectory containing MoBuVideoPlayer.dll.'
}
$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = Join-Path $MotionBuilderRoot 'bin/x64/motionbuilder.exe'
$startInfo.WorkingDirectory = $projectRoot
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$pluginPaths = [System.IO.Path]::GetFullPath($PluginDirectory)
if ($env:MOTIONBUILDER_PLUGIN_PATH) { $pluginPaths += ';' + $env:MOTIONBUILDER_PLUGIN_PATH }
$startInfo.Environment['MOTIONBUILDER_PLUGIN_PATH'] = $pluginPaths
if ($SmokeTest -and $StorySmokeTest) { throw 'Choose one host smoke test per dedicated process.' }
if ($SmokeTest -or $StorySmokeTest) {
    $StartupScript = Join-Path $projectRoot 'tests/mobu_smoke.py'
    $startInfo.Environment['MOBU_VIDEO_TEST_OUTPUT'] = Join-Path $projectRoot 'artifacts/host-smoke'
    $startInfo.Environment['MOBU_VIDEO_TEST_FIXTURES'] = Join-Path $projectRoot 'build/fixtures'
    $startInfo.Environment['MOBU_VIDEO_TEST_PLUGIN'] = Join-Path $PluginDirectory 'MoBuVideoPlayer.dll'
    $runId = [Guid]::NewGuid().ToString()
    $startInfo.Environment['MOBU_VIDEO_TEST_RUN_ID'] = $runId
    $startInfo.ArgumentList.Add('-suspendMessages')
    if ($StorySmokeTest) {
        $StartupScript = Join-Path $projectRoot 'tests/mobu_story_smoke.py'
        $startInfo.Environment['MOBU_STORY_TEST_OUTPUT'] = Join-Path $projectRoot 'artifacts/story-host-smoke'
        $startInfo.Environment['MOBU_AUDIO_TEST_FIXTURES'] = Join-Path $projectRoot 'build/audio-fixtures'
        $startInfo.Environment['MOBU_AUDIO_LOOPBACK_PROBE'] = Join-Path $projectRoot 'build/audio_loopback_probe.exe'
        $startInfo.Environment['MOBU_STORY_TEST_KEEP_OPEN'] = if ($KeepOpen) { '1' } else { '0' }
    }
}
if ($StartupScript) { $startInfo.ArgumentList.Add([System.IO.Path]::GetFullPath($StartupScript)) }
$process = [System.Diagnostics.Process]::Start($startInfo)
Write-Output "Started MotionBuilder with Native Video. Process ID: $($process.Id)"
if ($KeepOpen) {
    Write-Output 'The explicitly requested Story validation window remains open. Inspect artifacts/story-host-smoke/story-results.json for completion.'
    return
}
if ($SmokeTest -or $StorySmokeTest) {
    if (-not $process.WaitForExit(180000)) {
        $process.Kill()
        throw 'The dedicated MotionBuilder smoke-test process timed out.'
    }
    $reportFile = Join-Path $projectRoot 'artifacts/host-smoke/host-results.json'
    if ($StorySmokeTest) { $reportFile = Join-Path $projectRoot 'artifacts/story-host-smoke/story-results.json' }
    $report = if (Test-Path -LiteralPath $reportFile) { Get-Content -LiteralPath $reportFile -Raw | ConvertFrom-Json } else { $null }
    if (-not $report -or $report.run_id -ne $runId -or $report.status -ne 'passed') {
        throw "MotionBuilder smoke test did not pass. Inspect $reportFile (process exit $($process.ExitCode))."
    }
    Write-Output "PASS $($report.checks.Count) MotionBuilder host checks. Evidence: $reportFile"
}
