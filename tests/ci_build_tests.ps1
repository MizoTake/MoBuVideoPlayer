#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$ReportPath = ''
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$fixtureName = 'MoBuCiBuildTests-' + [guid]::NewGuid().ToString('N')
$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$fixtureRoot = Join-Path $temporaryRoot $fixtureName
$utf8 = [Text.UTF8Encoding]::new($false)
$script:Results = [Collections.Generic.List[object]]::new()
$environmentBefore = @{}
foreach ($name in @('MOBU_CI_FIXTURE_LOG', 'GITHUB_OUTPUT', 'GITHUB_ACTIONS', 'RUNNER_ENVIRONMENT',
    'RELEASE_TAG', 'EXPECTED_RELEASE_ID', 'RELEASE_VERSION', 'RELEASE_ARCHIVE', 'RELEASE_CHECKSUM', 'RELEASE_NOTES', 'GH_REPO')) {
    $environmentBefore[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}

function Write-Fixture([string]$Name, [string]$Value) {
    [IO.File]::WriteAllText((Join-Path $fixtureRoot $Name), $Value.Replace("`r`n", "`n"), $utf8)
}

function Get-FixtureCalls {
    @(Get-Content -LiteralPath $env:MOBU_CI_FIXTURE_LOG | ForEach-Object { $_ | ConvertFrom-Json })
}

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Reject([scriptblock]$Run, [string]$Pattern) {
    $rejected = $false
    try { & $Run }
    catch {
        Require ($_.Exception.Message -match $Pattern) $_.Exception.Message
        $rejected = $true
    }
    Require $rejected 'Expected a failure.'
}

function Check([string]$Name, [scriptblock]$Test) {
    try {
        & $Test
        $script:Results.Add(@{ name = $Name; passed = $true })
        Write-Output "PASS $Name"
    } catch {
        $script:Results.Add(@{ name = $Name; passed = $false; error = $_.Exception.Message })
        throw
    }
}

function Invoke-CiTestLocation([string]$WorkingDirectory, [string]$ProcessDirectory, [scriptblock]$Body) {
    $locationBefore = Get-Location
    $processBefore = [IO.Directory]::GetCurrentDirectory()
    try {
        Set-Location -LiteralPath $WorkingDirectory
        [IO.Directory]::SetCurrentDirectory($ProcessDirectory)
        Require ([IO.Path]::GetFullPath('relative-location-probe') -cne (Join-Path $WorkingDirectory 'relative-location-probe')) 'The regression setup did not separate PowerShell and process locations.'
        & $Body
        Require ([IO.Directory]::GetCurrentDirectory() -ceq $ProcessDirectory) 'The CI entry changed the process working directory.'
    }
    finally {
        Set-Location -LiteralPath $locationBefore.Path
        [IO.Directory]::SetCurrentDirectory($processBefore)
    }
}

function Invoke-WebRequest {
    throw 'Network requests are forbidden in the CI orchestration fixtures.'
}

function Get-WorkflowStepScript([string]$Name) {
    $lines = [IO.File]::ReadAllLines((Join-Path $repoRoot '.github/workflows/release.yml'))
    $stepFound = $false
    $collecting = $false
    $code = [Collections.Generic.List[string]]::new()
    foreach ($line in $lines) {
        if ($line -ceq ('      - name: ' + $Name)) { $stepFound = $true; continue }
        if (-not $stepFound) { continue }
        if ($line -ceq '        run: |') { $collecting = $true; continue }
        if ($collecting) {
            if (-not $line.StartsWith('          ')) { break }
            $code.Add($line.Substring(10))
        } elseif ($line.StartsWith('      - name:')) { break }
    }
    Require ($code.Count -gt 0) "The workflow step script is missing: $Name"
    return [scriptblock]::Create(($code -join "`n"))
}

function Set-GhFixture([object[]]$Releases) {
    $script:GhCalls = [Collections.Generic.List[object]]::new()
    $script:GhReleases = [Collections.Generic.Queue[object]]::new()
    foreach ($release in $Releases) { $script:GhReleases.Enqueue($release) }
}

function New-GhReleaseFixture([int]$Id = 42, [bool]$Draft = $true, [string[]]$Assets = @()) {
    return @{ databaseId = $Id; tagName = 'v0.2.1'; isDraft = $Draft; assets = @($Assets | ForEach-Object { @{ name = $_ } }) }
}

function gh {
    $arguments = @($args | ForEach-Object { [string]$_ })
    $script:GhCalls.Add([pscustomobject]@{ arguments = $arguments; command = ($arguments[0..1] -join ' ') })
    $global:LASTEXITCODE = 0
    if ($arguments[0] -ceq 'api') { return 'refs/tags/' + $env:RELEASE_TAG }
    if ($arguments[0] -cne 'release') { throw 'Unexpected GitHub operation in fixture.' }
    if ($arguments[1] -ceq 'view') {
        Require ($script:GhReleases.Count -gt 0) 'The workflow queried an unexpected release snapshot.'
        return $script:GhReleases.Dequeue() | ConvertTo-Json -Depth 5 -Compress
    }
    if ($arguments[1] -cnotin @('download', 'edit', 'upload')) { throw 'The workflow attempted a forbidden release operation.' }
}

try {
    $null = New-Item -ItemType Directory -Path (Join-Path $fixtureRoot 'scripts'), (Join-Path $fixtureRoot 'docs/releases')
    Copy-Item -LiteralPath (Join-Path $repoRoot 'scripts/ci-build.ps1') -Destination (Join-Path $fixtureRoot 'scripts/ci-build.ps1')
    Write-Fixture 'CMakeLists.txt' 'project(MoBuVideoPlayer VERSION 0.2.1 LANGUAGES CXX)'
    Write-Fixture 'docs/releases/0.2.1.md' 'Release notes'
    Write-Fixture 'scripts/build.ps1' @'
param($BuildDirectory, $Configuration, [switch]$CoreOnly, [switch]$RequireIntegrationTests)
$null = New-Item -ItemType Directory -Path (Join-Path $BuildDirectory 'Testing/Temporary'), (Join-Path $BuildDirectory 'fixtures'), (Join-Path $BuildDirectory 'audio-fixtures') -Force
@('1/2 Test: playback', '2/2 Test: native_decoder_integration', 'Directory: private-path-must-not-escape') | Set-Content -LiteralPath (Join-Path $BuildDirectory 'Testing/Temporary/LastTest.log')
@{ source = 'private-source-must-not-escape'; test = @{ exit_code = 0; stdout = "private-command-must-not-escape`nRESULT passed=73 failed=0`n" } } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $BuildDirectory 'fixtures/integration-results.json')
@{ status = 'passed'; checks = @(@{ name = 'first'; passed = $true }, @{ name = 'second'; passed = $true }); private = 'private-audio-must-not-escape' } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $BuildDirectory 'audio-fixtures/integration-results.json')
@{ kind = 'Build'; core = [bool]$CoreOnly; integration = [bool]$RequireIntegrationTests; configuration = $Configuration; buildDirectory = $BuildDirectory } | ConvertTo-Json -Compress | Add-Content -LiteralPath $env:MOBU_CI_FIXTURE_LOG
'@
    Write-Fixture 'scripts/check-public-source.ps1' @'
param($Root, $ReportPath)
@{ kind = 'SourceCheck'; root = $Root; report = $ReportPath } | ConvertTo-Json -Compress | Add-Content -LiteralPath $env:MOBU_CI_FIXTURE_LOG
'@
    Write-Fixture 'scripts/verify-release-input.ps1' @'
param($PluginPath, $MetadataPath, $ExpectedVersion, $SourceRoot, $ReportPath)
if ((Get-Content -LiteralPath $MetadataPath -Raw) -cne 'fixture provenance') { throw 'Fixture provenance was rejected.' }
@{ kind = 'InputVerify'; plugin = $PluginPath; metadata = $MetadataPath; version = $ExpectedVersion; sourceRoot = $SourceRoot; report = $ReportPath } | ConvertTo-Json -Compress | Add-Content -LiteralPath $env:MOBU_CI_FIXTURE_LOG
'@
    Write-Fixture 'scripts/package.ps1' @'
param($PluginPath, $OutputDirectory, $ExpectedVersion)
$null = New-Item -ItemType Directory -Path $OutputDirectory -Force
$name = "MoBuVideoPlayer-$ExpectedVersion-Mobu2026-win64.zip"
$path = Join-Path $OutputDirectory $name
[IO.File]::WriteAllBytes($path, [byte[]](1, 2, 3, 4))
$hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([IO.File]::ReadAllBytes($path))).ToLowerInvariant()
($hash + '  ' + $name) | Set-Content -LiteralPath ($path + '.sha256') -Encoding utf8NoBOM
@{ kind = 'Package'; plugin = $PluginPath; version = $ExpectedVersion; output = $OutputDirectory } | ConvertTo-Json -Compress | Add-Content -LiteralPath $env:MOBU_CI_FIXTURE_LOG
'@
    Write-Fixture 'scripts/verify-package.ps1' @'
param($ArchivePath, $ExpectedVersion, $CompareSourceRoot, $ReportPath, [switch]$RequireChecksum)
@{ kind = 'Verify'; version = $ExpectedVersion; requireChecksum = [bool]$RequireChecksum; archive = $ArchivePath; sourceRoot = $CompareSourceRoot; report = $ReportPath } | ConvertTo-Json -Compress | Add-Content -LiteralPath $env:MOBU_CI_FIXTURE_LOG
'@
    $ciScript = Join-Path $fixtureRoot 'scripts/ci-build.ps1'
    $ciOutput = Join-Path $fixtureRoot 'output'
    $ciBuild = Join-Path $fixtureRoot 'build'
    $archive = Join-Path $ciOutput 'MoBuVideoPlayer-0.2.1-Mobu2026-win64.zip'
    $inputPlugin = Join-Path $fixtureRoot 'local-plugin.dll'
    $inputMetadata = $inputPlugin + '.json'
    [IO.File]::WriteAllBytes($inputPlugin, [byte[]](1, 2, 3, 4))
    [IO.File]::WriteAllText($inputMetadata, 'fixture provenance')
    $env:MOBU_CI_FIXTURE_LOG = Join-Path $fixtureRoot 'calls.jsonl'
    $env:GITHUB_OUTPUT = Join-Path $fixtureRoot 'github-output.txt'

    Check 'Core checks public source and requires all native tests without the SDK' {
        & $ciScript -Mode Core -BuildDirectory $ciBuild
        $calls = Get-FixtureCalls
        Require (($calls.kind -join ',') -eq 'SourceCheck,Build') 'Unexpected Core call order.'
        Require ($calls[1].core -and $calls[1].integration -and $calls[1].configuration -eq 'Release') 'Core build flags changed.'
    }
    Check 'Public summary contains test names and counts without raw report fields' {
        $json = Get-Content -LiteralPath (Join-Path $ciBuild 'public-ci-summary.json') -Raw
        $summary = $json | ConvertFrom-Json
        Require ($summary.status -eq 'passed' -and $summary.testCount -eq 2) 'CTest names or status are missing.'
        Require ($summary.integration.decoder.count -eq 73 -and $summary.integration.audio.count -eq 2) 'Integration counts are missing.'
        Require ($json -notmatch 'private-') 'Raw report fields escaped into the public summary.'
    }
    Check 'A mismatched version tag fails before packaging' {
        Reject { & $ciScript -Mode PackageInput -Tag 'v0.2.2' } 'exactly match'
    }
    Check 'A tag with different letter case fails before packaging' {
        Reject { & $ciScript -Mode PackageInput -Tag 'V0.2.1' } 'exactly match'
    }
    Check 'Missing release notes fail before packaging' {
        Move-Item -LiteralPath (Join-Path $fixtureRoot 'docs/releases/0.2.1.md') -Destination (Join-Path $fixtureRoot 'docs/releases/backup.md')
        try { Reject { & $ciScript -Mode PackageInput -Tag 'v0.2.1' } 'Write release notes' }
        finally { Move-Item -LiteralPath (Join-Path $fixtureRoot 'docs/releases/backup.md') -Destination (Join-Path $fixtureRoot 'docs/releases/0.2.1.md') }
    }
    Check 'A package input requires both the DLL and provenance path' {
        [IO.File]::WriteAllText($env:MOBU_CI_FIXTURE_LOG, '')
        Reject { & $ciScript -Mode PackageInput -Tag 'v0.2.1' -PluginPath $inputPlugin } 'requires the local plug-in DLL'
        Require (((Get-FixtureCalls).kind -join ',') -eq 'SourceCheck') 'A missing input reached the package operation.'
    }
    Check 'Rejected provenance stops packaging before any artifact is generated' {
        [IO.File]::WriteAllText($env:MOBU_CI_FIXTURE_LOG, '')
        [IO.File]::WriteAllText($inputMetadata, 'untrusted provenance')
        try {
            Reject { & $ciScript -Mode PackageInput -Tag 'v0.2.1' -PluginPath $inputPlugin -MetadataPath $inputMetadata } 'Fixture provenance was rejected'
            Require (((Get-FixtureCalls).kind -join ',') -eq 'SourceCheck') 'Rejected provenance reached the package operation.'
        } finally {
            [IO.File]::WriteAllText($inputMetadata, 'fixture provenance')
        }
    }
    Check 'A local DLL is provenance checked and packaged without any SDK build' {
        [IO.File]::WriteAllText($env:MOBU_CI_FIXTURE_LOG, '')
        & $ciScript -Mode PackageInput -Tag 'v0.2.1' -PluginPath $inputPlugin -MetadataPath $inputMetadata -BuildDirectory $ciBuild -OutputDirectory $ciOutput
        $calls = Get-FixtureCalls
        Require (($calls.kind -join ',') -eq 'SourceCheck,InputVerify,Package,Verify') 'Unexpected PackageInput call order.'
        Require ($calls[1].plugin -eq $inputPlugin -and $calls[1].metadata -eq $inputMetadata -and $calls[1].sourceRoot -eq $fixtureRoot) 'Provenance did not check the expected inputs against this source tree.'
        Require ($calls[2].plugin -eq $inputPlugin -and $calls[2].version -eq '0.2.1') 'Package input flags changed.'
        Require ($calls[3].requireChecksum) 'The final verifier must require a checksum.'
        Require ((Get-Content -LiteralPath $env:GITHUB_OUTPUT -Raw) -match 'version=0.2.1') 'GitHub output fields are missing.'
    }
    Check 'Downloaded assets are verified without invoking the build or package steps' {
        [IO.File]::WriteAllText($env:MOBU_CI_FIXTURE_LOG, '')
        & $ciScript -Mode VerifyRelease -Tag 'v0.2.1' -BuildDirectory $ciBuild -OutputDirectory $ciOutput
        Require (((Get-FixtureCalls).kind -join ',') -eq 'Verify') 'VerifyRelease invoked build operations.'
    }
    Check 'Altered archive bytes fail before the package verifier' {
        [IO.File]::WriteAllBytes($archive, [byte[]](5, 6, 7))
        Reject { & $ciScript -Mode VerifyRelease -Tag 'v0.2.1' -OutputDirectory $ciOutput } 'SHA256 does not match'
    }
    Check 'A checksum naming another archive is rejected' {
        ('a' * 64 + '  another.zip') | Set-Content -LiteralPath ($archive + '.sha256')
        Reject { & $ciScript -Mode VerifyRelease -Tag 'v0.2.1' -OutputDirectory $ciOutput } 'name only the expected'
    }
    Check 'A missing checksum sidecar is rejected' {
        Move-Item -LiteralPath ($archive + '.sha256') -Destination ($archive + '.sha256.bak')
        Reject { & $ciScript -Mode VerifyRelease -Tag 'v0.2.1' -OutputDirectory $ciOutput } 'both required'
    }
    Check 'A local run cannot download CI test tools' {
        $env:GITHUB_ACTIONS = 'false'
        $env:RUNNER_ENVIRONMENT = 'github-hosted'
        Reject { & $ciScript -Mode Core -PrepareHostedTools } 'limited to the GitHub-hosted'
    }
    Check 'A package-only run cannot download native test tools' {
        $env:GITHUB_ACTIONS = 'true'
        $env:RUNNER_ENVIRONMENT = 'github-hosted'
        Reject { & $ciScript -Mode PackageInput -Tag 'v0.2.1' -PrepareHostedTools } 'limited to the GitHub-hosted'
    }
    Check 'A non-hosted Core run cannot download native test tools' {
        $env:GITHUB_ACTIONS = 'true'
        $env:RUNNER_ENVIRONMENT = 'self-hosted'
        Reject { & $ciScript -Mode Core -PrepareHostedTools } 'limited to the GitHub-hosted'
    }
    $preflightStep = Get-WorkflowStepScript 'Require an existing version tag and an unpublished release draft'
    $downloadStep = Get-WorkflowStepScript 'Download the exact local DLL and provenance assets'
    $uploadStep = Get-WorkflowStepScript 'Add the verified ZIP and checksum to the same release draft'
    $env:RELEASE_TAG = 'v0.2.1'
    $env:EXPECTED_RELEASE_ID = '42'
    $env:RELEASE_VERSION = '0.2.1'
    $env:RELEASE_ARCHIVE = $archive
    $env:RELEASE_CHECKSUM = $archive + '.sha256'
    $env:RELEASE_NOTES = Join-Path $fixtureRoot 'docs/releases/0.2.1.md'
    $env:GH_REPO = 'fixture/repository'
    Check 'Workflow preflight records the identity of the selected release draft' {
        Set-GhFixture @((New-GhReleaseFixture))
        & $preflightStep
        Require ((Get-Content -LiteralPath $env:GITHUB_OUTPUT -Raw) -match 'release_id=42') 'The original release ID was not recorded.'
        Require (@($script:GhCalls | Where-Object command -in @('release edit', 'release upload')).Count -eq 0) 'Preflight mutated the release.'
    }
    Check 'Workflow rejects a recreated release before downloading inputs' {
        Set-GhFixture @((New-GhReleaseFixture -Id 43))
        Reject { & $downloadStep } 'matching unpublished draft'
        Require (@($script:GhCalls | Where-Object command -eq 'release download').Count -eq 0) 'The workflow downloaded a replacement release.'
    }
    Check 'Workflow rejects a published release before downloading inputs' {
        Set-GhFixture @((New-GhReleaseFixture -Draft $false))
        Reject { & $downloadStep } 'matching unpublished draft'
    }
    Check 'Workflow downloads only the exact DLL and metadata names from the original draft' {
        Set-GhFixture @((New-GhReleaseFixture))
        & $downloadStep
        $download = @($script:GhCalls | Where-Object command -eq 'release download')
        Require ($download.Count -eq 1) 'The workflow did not download the matching release.'
        Require (($download[0].arguments -join ' ') -match '--pattern MoBuVideoPlayer-0.2.1-Mobu2026-win64.dll --pattern MoBuVideoPlayer-0.2.1-Mobu2026-win64.dll.json') 'The input asset patterns changed.'
    }
    Check 'Workflow rejects a published release before any upload or metadata edit' {
        Set-GhFixture @((New-GhReleaseFixture -Draft $false))
        Reject { & $uploadStep } 'matching unpublished draft'
        Require (@($script:GhCalls | Where-Object command -in @('release edit', 'release upload')).Count -eq 0) 'The workflow mutated a published release.'
    }
    Check 'Workflow does not replace or delete an existing output asset' {
        Set-GhFixture @((New-GhReleaseFixture -Assets @([IO.Path]::GetFileName($archive))))
        Reject { & $uploadStep } 'Remove the existing output assets'
        Require (@($script:GhCalls | Where-Object command -in @('release edit', 'release upload')).Count -eq 0) 'A duplicate asset caused a release mutation.'
    }
    Check 'Workflow upload leaves release state unchanged and never clobbers assets' {
        Set-GhFixture @((New-GhReleaseFixture), (New-GhReleaseFixture))
        & $uploadStep
        $mutations = @($script:GhCalls | Where-Object command -in @('release edit', 'release upload'))
        Require (($mutations.command -join ',') -eq 'release edit,release upload') 'Expected release metadata edit and asset upload.'
        Require (@($mutations.arguments | Where-Object { $_ -match '^--(?:draft|clobber|delete)(?:=|$)' }).Count -eq 0) 'The workflow changed release state or replaced assets.'
    }
    Check 'Workflow stops uploading when the draft is published after notes are updated' {
        Set-GhFixture @((New-GhReleaseFixture), (New-GhReleaseFixture -Draft $false))
        Reject { & $uploadStep } 'matching unpublished draft'
        Require (@($script:GhCalls | Where-Object command -eq 'release upload').Count -eq 0) 'The workflow uploaded after publication.'
    }
    Check 'Workflow stops uploading when the release is recreated after notes are updated' {
        Set-GhFixture @((New-GhReleaseFixture), (New-GhReleaseFixture -Id 43))
        Reject { & $uploadStep } 'matching unpublished draft'
        Require (@($script:GhCalls | Where-Object command -eq 'release upload').Count -eq 0) 'The workflow uploaded to a replacement release.'
    }
    $unicodeName = (-join [char[]]@(0x65e5, 0x672c, 0x8a9e)) + ' path'
    $processDirectory = Join-Path $fixtureRoot 'process-directory'
    $null = New-Item -ItemType Directory -Path $processDirectory
    Check 'CI resolves relative Unicode DLL, metadata, build and output paths before delegating' {
        $inputName = $unicodeName + ' input'
        $null = New-Item -ItemType Directory -Path (Join-Path $fixtureRoot $inputName)
        Copy-Item -LiteralPath $inputPlugin -Destination (Join-Path (Join-Path $fixtureRoot $inputName) 'plugin.dll')
        Copy-Item -LiteralPath $inputMetadata -Destination (Join-Path (Join-Path $fixtureRoot $inputName) 'plugin.json')
        $buildName = $unicodeName + ' build'
        $outputName = $unicodeName + ' output'
        [IO.File]::WriteAllText($env:MOBU_CI_FIXTURE_LOG, '')
        Invoke-CiTestLocation $fixtureRoot $processDirectory {
            & $ciScript -Mode PackageInput -Tag 'v0.2.1' -PluginPath (Join-Path $inputName 'plugin.dll') `
                -MetadataPath (Join-Path $inputName 'plugin.json') -BuildDirectory $buildName -OutputDirectory $outputName
        }
        $calls = Get-FixtureCalls
        Require (($calls.kind -join ',') -eq 'SourceCheck,InputVerify,Package,Verify') 'Relative arguments changed the CI call order.'
        Require ($calls[1].plugin -ceq (Join-Path (Join-Path $fixtureRoot $inputName) 'plugin.dll')) 'The DLL argument was not resolved from PowerShell location.'
        Require ($calls[1].metadata -ceq (Join-Path (Join-Path $fixtureRoot $inputName) 'plugin.json')) 'The metadata argument was not resolved from PowerShell location.'
        Require ($calls[0].report -ceq (Join-Path (Join-Path $fixtureRoot $buildName) 'public-source-results.json')) 'The build/report argument was not resolved from PowerShell location.'
        Require ($calls[2].output -ceq (Join-Path $fixtureRoot $outputName)) 'The output argument was not resolved from PowerShell location.'
        Require ([IO.File]::Exists((Join-Path (Join-Path $fixtureRoot $buildName) 'public-ci-summary.json'))) 'The CI summary used the process directory.'
    }
    Check 'CI Core resolves a relative build directory before passing it to the build entry' {
        [IO.File]::WriteAllText($env:MOBU_CI_FIXTURE_LOG, '')
        Invoke-CiTestLocation $fixtureRoot $processDirectory {
            & $ciScript -Mode Core -BuildDirectory ($unicodeName + ' core build')
        }
        $calls = Get-FixtureCalls
        Require ($calls[1].buildDirectory -ceq (Join-Path $fixtureRoot ($unicodeName + ' core build'))) 'The Core build argument remained relative.'
    }
    Check 'CI package paths support a FileSystem PowerShell drive' {
        $driveName = 'MoBuCi' + [guid]::NewGuid().ToString('N').Substring(0, 8)
        $null = New-PSDrive -Name $driveName -PSProvider FileSystem -Root $fixtureRoot
        try {
            [IO.File]::WriteAllText($env:MOBU_CI_FIXTURE_LOG, '')
            Invoke-CiTestLocation $fixtureRoot $processDirectory {
                & $ciScript -Mode PackageInput -Tag 'v0.2.1' -PluginPath ($driveName + ':\local-plugin.dll') `
                    -MetadataPath ($driveName + ':\local-plugin.dll.json') -BuildDirectory ($driveName + ':\drive build') `
                    -OutputDirectory ($driveName + ':\' + $unicodeName + ' drive output')
            }
            $calls = Get-FixtureCalls
            Require ($calls[1].plugin -ceq $inputPlugin -and $calls[1].metadata -ceq $inputMetadata) 'The FileSystem drive inputs were not resolved.'
            Require ($calls[2].output -ceq (Join-Path $fixtureRoot ($unicodeName + ' drive output'))) 'The FileSystem drive output was not resolved.'
        }
        finally { Remove-PSDrive -Name $driveName }
    }
    Check 'CI explicitly rejects non-FileSystem output paths before delegating' {
        [IO.File]::WriteAllText($env:MOBU_CI_FIXTURE_LOG, '')
        Reject { & $ciScript -Mode Core -OutputDirectory 'Env:PATH' -BuildDirectory $ciBuild } 'FileSystem'
        Require ((Get-FixtureCalls).Count -eq 0) 'A non-FileSystem output reached a delegated operation.'
    }
    Check 'CI resolves FileSystem-qualified relative paths before delegating' {
        [IO.File]::WriteAllText($env:MOBU_CI_FIXTURE_LOG, '')
        Invoke-CiTestLocation $fixtureRoot $processDirectory {
            & $ciScript -Mode PackageInput -Tag 'v0.2.1' -PluginPath 'FileSystem::local-plugin.dll' `
                -MetadataPath 'FileSystem::local-plugin.dll.json' -BuildDirectory 'FileSystem::qualified build' `
                -OutputDirectory 'FileSystem::qualified output'
        }
        $calls = Get-FixtureCalls
        Require ($calls[1].plugin -ceq $inputPlugin -and $calls[1].metadata -ceq $inputMetadata) 'The provider-qualified relative inputs were not resolved.'
        Require ($calls[2].output -ceq (Join-Path $fixtureRoot 'qualified output')) 'The provider-qualified relative output was not resolved.'
    }
} finally {
    foreach ($name in $environmentBefore.Keys) {
        [Environment]::SetEnvironmentVariable($name, $environmentBefore[$name], 'Process')
    }
    if ($ReportPath) {
        $destination = [IO.Path]::GetFullPath($ReportPath)
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination))
        $report = @{ status = 'passed'; count = $script:Results.Count; checks = @($script:Results) }
        if (@($script:Results | Where-Object { -not $_.passed }).Count) { $report.status = 'failed' }
        [IO.File]::WriteAllText($destination, ($report | ConvertTo-Json -Depth 6) + "`n", $utf8)
    }
    if (Test-Path -LiteralPath $fixtureRoot) {
        $resolvedFixture = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $fixtureRoot).ProviderPath)
        $temporaryPrefix = $temporaryRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
        if (-not $resolvedFixture.StartsWith($temporaryPrefix, [StringComparison]::OrdinalIgnoreCase) -or
            [IO.Path]::GetFileName($resolvedFixture) -cne $fixtureName) {
            throw 'The fixture cleanup target escaped its temporary directory.'
        }
        Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
    }
}
