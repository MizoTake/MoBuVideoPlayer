#requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('Core', 'PackageInput', 'VerifyRelease')]
    [string]$Mode = 'Core',
    [string]$PluginPath = '',
    [string]$MetadataPath = '',
    [string]$BuildDirectory = '',
    [string]$OutputDirectory = '',
    [string]$Tag = '',
    [switch]$PrepareHostedTools
)

$ErrorActionPreference = 'Stop'
foreach ($pathParameter in @('PluginPath', 'MetadataPath', 'BuildDirectory', 'OutputDirectory')) {
    $pathValue = Get-Variable -Name $pathParameter -ValueOnly
    if ([string]::IsNullOrEmpty($pathValue)) { continue }
    $pathProvider = $null
    $pathDrive = $null
    $resolvedPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath(
        $pathValue, [ref]$pathProvider, [ref]$pathDrive)
    if ($pathProvider.Name -ne 'FileSystem') { throw "$pathParameter must use the FileSystem provider." }
    if (-not [IO.Path]::IsPathRooted($resolvedPath)) {
        $resolvedPath = Join-Path $ExecutionContext.SessionState.Path.CurrentFileSystemLocation.Path $resolvedPath
    } elseif ($resolvedPath -match '^[\\/](?![\\/])') {
        $resolvedPath = Join-Path $ExecutionContext.SessionState.Path.CurrentFileSystemLocation.Drive.Root $resolvedPath.TrimStart('\', '/')
    }
    $resolvedPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath(
        $resolvedPath, [ref]$pathProvider, [ref]$pathDrive)
    if ($pathProvider.Name -ne 'FileSystem') { throw "$pathParameter must use the FileSystem provider." }
    Set-Variable -Name $pathParameter -Value ([IO.Path]::GetFullPath($resolvedPath))
}
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $projectRoot ('build/ci-' + $Mode.ToLowerInvariant()) }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $projectRoot 'dist' }
$projectDefinition = Get-Content -LiteralPath (Join-Path $projectRoot 'CMakeLists.txt') -Raw
if ($projectDefinition -notmatch 'project\(MoBuVideoPlayer VERSION ([0-9]+\.[0-9]+\.[0-9]+)') {
    throw 'The project version was not found in CMakeLists.txt.'
}
$version = $Matches[1]
$archiveName = "MoBuVideoPlayer-$version-Mobu2026-win64.zip"
$archive = Join-Path $OutputDirectory $archiveName
$checksum = $archive + '.sha256'
$notes = Join-Path $projectRoot "docs/releases/$version.md"

$runStatus = 'failed'
$stage = 'preflight'
$testRunComplete = $false
try {
    if ($PrepareHostedTools) {
        if ($Mode -ne 'Core' -or $env:GITHUB_ACTIONS -ne 'true' -or $env:RUNNER_ENVIRONMENT -ne 'github-hosted') {
            throw 'Automatic test-tool download is limited to the GitHub-hosted Core CI job.'
        }
        if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue) -or
            -not (Get-Command ffprobe -ErrorAction SilentlyContinue)) {
            # A moving latest URL would make reference-pixel changes impossible to attribute to a tool version.
            $toolVersion = '9.0.2'
            $toolHash = '60f467265b1e312373dbcd92200c2618a74850f98d3d078e94296bb3fa2047ba'
            $toolDirectory = Join-Path $BuildDirectory 'ci-tools'
            $null = New-Item -ItemType Directory -Path $toolDirectory -Force
            $toolArchive = Join-Path $toolDirectory "ffmpeg-$toolVersion-essentials_build.zip"
            $toolUri = "https://github.com/GyanD/codexffmpeg/releases/download/$toolVersion/ffmpeg-$toolVersion-essentials_build.zip"
            Invoke-WebRequest -Uri $toolUri -OutFile $toolArchive
            if ((Get-FileHash -LiteralPath $toolArchive -Algorithm SHA256).Hash -ine $toolHash) {
                throw 'The CI-only FFmpeg download failed the pinned SHA256 check.'
            }
            Add-Type -AssemblyName System.IO.Compression.FileSystem
            $toolZip = [IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($toolArchive))
            try {
                foreach ($toolName in @('ffmpeg.exe', 'ffprobe.exe')) {
                    $entry = $toolZip.GetEntry("ffmpeg-$toolVersion-essentials_build/bin/$toolName")
                    if (-not $entry) { throw "The pinned FFmpeg archive is missing $toolName." }
                    [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $toolDirectory $toolName), $true)
                }
            } finally {
                $toolZip.Dispose()
            }
            $env:PATH = [IO.Path]::GetFullPath($toolDirectory) + [IO.Path]::PathSeparator + $env:PATH
        }
    }

    if ($Mode -ne 'Core') {
        if ($Tag -cne ('v' + $version)) {
            throw "The release tag must exactly match v$version from CMakeLists.txt."
        }
        if (-not (Test-Path -LiteralPath $notes -PathType Leaf)) {
            throw "Write release notes in docs/releases/$version.md before creating the release tag."
        }
    }

    if ($Mode -eq 'Core') {
        $stage = 'source-check'
        & (Join-Path $PSScriptRoot 'check-public-source.ps1') -Root $projectRoot `
            -ReportPath (Join-Path $BuildDirectory 'public-source-results.json')
        $stage = 'build-and-test'
        & (Join-Path $PSScriptRoot 'build.ps1') -CoreOnly -RequireIntegrationTests `
            -BuildDirectory $BuildDirectory -Configuration Release
        $testRunComplete = $true
    } elseif ($Mode -eq 'PackageInput') {
        $stage = 'source-check'
        & (Join-Path $PSScriptRoot 'check-public-source.ps1') -Root $projectRoot `
            -ReportPath (Join-Path $BuildDirectory 'public-source-results.json')
        $stage = 'verify-input'
        if (-not $PluginPath -or -not $MetadataPath) {
            throw 'PackageInput requires the local plug-in DLL and its provenance metadata.'
        }
        & (Join-Path $PSScriptRoot 'verify-release-input.ps1') -PluginPath $PluginPath `
            -MetadataPath $MetadataPath -ExpectedVersion $version -SourceRoot $projectRoot `
            -ReportPath (Join-Path $BuildDirectory 'input-verification.json')
        $stage = 'package'
        & (Join-Path $PSScriptRoot 'package.ps1') -PluginPath $PluginPath `
            -OutputDirectory $OutputDirectory -ExpectedVersion $version
    }

    if ($Mode -ne 'Core') {
        $stage = 'verify-assets'
        if (-not (Test-Path -LiteralPath $archive -PathType Leaf) -or
            -not (Test-Path -LiteralPath $checksum -PathType Leaf)) {
            throw 'The exact versioned release ZIP and its SHA256 file are both required.'
        }
        $checksumText = (Get-Content -LiteralPath $checksum -Raw).Trim()
        $checksumPattern = '^([a-fA-F0-9]{64})\s+\*?' + [regex]::Escape($archiveName) + '$'
        if ($checksumText -notmatch $checksumPattern) {
            throw 'The checksum must name only the expected release archive.'
        }
        $expectedHash = $Matches[1]
        $actualHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
        if ($expectedHash -ine $actualHash) { throw 'The release archive SHA256 does not match its checksum file.' }
        & (Join-Path $PSScriptRoot 'verify-package.ps1') -ArchivePath $archive `
            -ExpectedVersion $version -CompareSourceRoot $projectRoot -RequireChecksum `
            -ReportPath (Join-Path $BuildDirectory 'package-verification.json')
        if ($env:GITHUB_OUTPUT) {
            @(
                "version=$version"
                "archive=$archive"
                "checksum=$checksum"
                "notes=$notes"
            ) | Add-Content -LiteralPath $env:GITHUB_OUTPUT -Encoding utf8
        }
        Write-Output "Verified release assets: $archiveName and $archiveName.sha256"
    }

    $stage = 'complete'
    $runStatus = 'passed'
} finally {
    $testNames = @()
    $integration = [ordered]@{}
    if ($testRunComplete) {
        $testLog = Join-Path $BuildDirectory 'Testing/Temporary/LastTest.log'
        if (Test-Path -LiteralPath $testLog -PathType Leaf) {
            $testNames = @(Get-Content -LiteralPath $testLog | ForEach-Object {
                if ($_ -match '^\d+/\d+ Test: ([A-Za-z0-9_.-]+)$') { $Matches[1] }
            } | Sort-Object -Unique)
        }
        foreach ($item in @(@{name='decoder';file='fixtures/integration-results.json'},
            @{name='audio';file='audio-fixtures/integration-results.json'})) {
            $resultPath = Join-Path $BuildDirectory $item.file
            if (Test-Path -LiteralPath $resultPath -PathType Leaf) {
                $result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
                if ($item.name -eq 'decoder' -and $result.test.exit_code -eq 0 -and
                    $result.test.stdout -match '(?m)^RESULT passed=([0-9]+) failed=0\r?$') {
                    $integration[$item.name] = @{status='passed';count=[int]$Matches[1]}
                } elseif ($item.name -eq 'audio' -and $result.status -eq 'passed') {
                    $integration[$item.name] = @{status='passed';count=@($result.checks).Count}
                }
            }
        }
    }
    # Raw CTest and fixture reports contain absolute source paths; only these selected fields are public artifacts.
    $summary = [ordered]@{
        mode = $Mode
        version = $version
        status = $runStatus
        stage = $stage
        testCount = $testNames.Count
        tests = $testNames
        integration = $integration
    }
    $summaryPath = [IO.Path]::GetFullPath((Join-Path $BuildDirectory 'public-ci-summary.json'))
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($summaryPath))
    [IO.File]::WriteAllText($summaryPath, ($summary | ConvertTo-Json -Depth 5) + "`n", [Text.UTF8Encoding]::new($false))
}
