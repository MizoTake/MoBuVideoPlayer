[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskOutput = Join-Path $taskRoot 'artifacts/reuse-host'
$taskReportPath = Join-Path $taskOutput 'reuse-results.json'
$taskProbe = Join-Path $taskRoot 'build/audio_loopback_probe.exe'

function Send-ReuseCommand([string]$Action, [string]$Name = '') {
    $commandId = [Guid]::NewGuid().ToString()
    $temporary = Join-Path $taskOutput "command-$commandId.tmp"
    $payload = @{ id = $commandId; action = $Action }
    if ($Name) { $payload.name = $Name }
    $payload | ConvertTo-Json |
        Set-Content -LiteralPath $temporary -Encoding utf8NoBOM
    Move-Item -LiteralPath $temporary -Destination (Join-Path $taskOutput 'command.json') -Force
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        Start-Sleep -Milliseconds 100
        $result = Get-Content -LiteralPath $taskReportPath -Raw | ConvertFrom-Json
        if ($result.status -eq 'failed') {
            throw "The existing-host checks failed: $($result.error)"
        }
        if ($result.last_command_id -eq $commandId) {
            if ($result.last_command.status -ne 'complete') { throw "The host rejected action $Action." }
            return $result
        }
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "The existing host did not acknowledge action $Action. No MotionBuilder process was started."
}

if (-not (Test-Path -LiteralPath $taskReportPath)) {
    throw 'Run mobu_reuse_checks.py in the existing MotionBuilder Python Editor first.'
}
$initial = Get-Content -LiteralPath $taskReportPath -Raw | ConvertFrom-Json
if ($initial.status -ne 'ready') { throw "The existing-host checks are not ready: $($initial.status)" }
$taskProcessesBefore = @(Get-Process motionbuilder | ForEach-Object Id)
$captures = @()
$capture = $null
$hostFinished = $false
$driverFailure = $null
$cleanupErrors = @()
$final = $null
try {
    if (-not (Test-Path -LiteralPath $taskProbe -PathType Leaf)) {
        throw 'Build the existing Windows loopback probe before running reuse checks.'
    }
    foreach ($signal in @(@{ name = 'stereo'; frequency = 440 }, @{ name = 'pcm24'; frequency = 431 })) {
        $captureId = [Guid]::NewGuid().ToString()
        $capturePath = Join-Path $taskOutput "$($signal.name)-$captureId.json"
        $arguments = @('"' + $capturePath + '"', '3', [string]$signal.frequency)
        $capture = Start-Process -FilePath $taskProbe -ArgumentList $arguments -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput ($capturePath + '.stdout') -RedirectStandardError ($capturePath + '.stderr')
        $readyDeadline = [DateTime]::UtcNow.AddSeconds(5)
        while (-not (Test-Path -LiteralPath ($capturePath + '.ready'))) {
            if ($capture.HasExited -or [DateTime]::UtcNow -ge $readyDeadline) {
                throw 'The OS loopback probe could not start.'
            }
            Start-Sleep -Milliseconds 25
        }
        Send-ReuseCommand 'play' $signal.name | Out-Null
        if (-not $capture.WaitForExit(8000)) {
            throw 'The owned audio probe timed out.'
        }
        Send-ReuseCommand 'mute' | Out-Null
        if ($capture.ExitCode) { throw (Get-Content -LiteralPath ($capturePath + '.stderr') -Raw) }
        $sample = Get-Content -LiteralPath $capturePath -Raw | ConvertFrom-Json
        if ($sample.toneAmplitude -le 0.003) { throw "Expected $($signal.frequency) Hz was not heard from standard Story." }
        $captures += @{ name = $signal.name; frequency = $signal.frequency; result = $sample }
        Write-Output "PASS existing host output: $($signal.name), $($signal.frequency) Hz, amplitude=$($sample.toneAmplitude)"
        $capture.Dispose()
        $capture = $null
    }
    Send-ReuseCommand 'save' | Out-Null
    Send-ReuseCommand 'append' | Out-Null
    $final = Send-ReuseCommand 'finish'
    $hostFinished = $true
    $taskProcessesAfter = @(Get-Process motionbuilder | ForEach-Object Id)
    if (@(Compare-Object $taskProcessesBefore $taskProcessesAfter).Count) {
        throw 'The MotionBuilder process inventory changed during the reuse checks.'
    }
} catch {
    $driverFailure = $_
} finally {
    if ($capture) {
        try {
            if (-not $capture.HasExited) {
                $capture.Kill()
                if (-not $capture.WaitForExit(5000)) { throw 'The owned loopback probe did not exit during cleanup.' }
            }
            $capture.Dispose()
        } catch { $cleanupErrors += "Loopback cleanup: $($_.Exception.Message)" }
    }
    if (-not $hostFinished) {
        try {
            $current = Get-Content -LiteralPath $taskReportPath -Raw | ConvertFrom-Json
            if ($current.status -eq 'passed' -or ($current.status -eq 'failed' -and $current.scene_preserved -and -not $current.restore_error)) {
                $final = $current
            } else {
                try { Send-ReuseCommand 'mute' | Out-Null } catch { $cleanupErrors += "Mute acknowledgement: $($_.Exception.Message)" }
                try { $final = Send-ReuseCommand 'finish' } catch {
                    $current = Get-Content -LiteralPath $taskReportPath -Raw | ConvertFrom-Json
                    $final = $current
                    if (-not $current.scene_preserved -or $current.restore_error) {
                        $cleanupErrors += "Scene restoration: $($_.Exception.Message)"
                    }
                }
            }
        } catch { $cleanupErrors += "Cannot verify scene restoration: $($_.Exception.Message)" }
    }
    $taskProcessesAfter = @(Get-Process motionbuilder -ErrorAction SilentlyContinue | ForEach-Object Id)
    $driverReport = @{
        status = if ($driverFailure -or $cleanupErrors.Count) { 'failed' } else { 'passed' }
        captures = $captures
        host_pids = $taskProcessesAfter
        scene_preserved = [bool]($final -and $final.scene_preserved)
        cleanup_errors = $cleanupErrors
    }
    if ($driverFailure) { $driverReport.error = $driverFailure.Exception.Message }
    $driverReport | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath (Join-Path $taskOutput 'driver-results.json') -Encoding utf8NoBOM
}
if ($cleanupErrors.Count) { Write-Warning ($cleanupErrors -join [Environment]::NewLine) }
if ($driverFailure) { throw $driverFailure }
if ($cleanupErrors.Count) {
    throw 'Existing-host validation cleanup was not fully acknowledged. Inspect driver-results.json before continuing.'
}
Write-Output "PASS $($final.checks.Count) existing-host checks; original scene retained."
