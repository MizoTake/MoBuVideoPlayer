#requires -Version 5.1
[CmdletBinding()]
param([string]$ReportPath = '')

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$verifyScript = Join-Path $projectRoot 'scripts/verify-package.ps1'
$packageScript = Join-Path $projectRoot 'scripts/package.ps1'
$definition = Get-Content -LiteralPath (Join-Path $projectRoot 'CMakeLists.txt') -Raw
if ($definition -notmatch 'project\(MoBuVideoPlayer VERSION ([0-9]+\.[0-9]+\.[0-9]+)') { throw 'The fixture package version was not found.' }
$script:FixtureVersion = $Matches[1]
$fixtureRoot = Join-Path ([IO.Path]::GetTempPath()) ('MoBuReleaseTests-' + [guid]::NewGuid().ToString('N'))
$script:Results = New-Object 'Collections.Generic.List[object]'
$script:InstallSource = ''
$script:DifferentBuildOutput = $false
$script:RejectCmake = $false
$utf8 = New-Object Text.UTF8Encoding($false)
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Get-FileHash {
    param([string]$LiteralPath)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($LiteralPath)
    try { return [pscustomobject]@{ Hash = ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() } }
    finally { $stream.Dispose(); $algorithm.Dispose() }
}

function Assert-Fails {
    param([scriptblock]$Body, [string]$Pattern)
    try { & $Body | Out-Null }
    catch {
        if ($_.Exception.Message -notmatch $Pattern) { throw "Unexpected failure: $($_.Exception.Message)" }
        return
    }
    throw 'Expected the operation to reject the invalid release.'
}

function Invoke-Case {
    param([string]$Name, [scriptblock]$Body)
    try {
        & $Body
        $script:Results.Add([pscustomobject]@{ name = $Name; status = 'passed' })
        Write-Host "PASS $Name"
    }
    catch {
        $script:Results.Add([pscustomobject]@{ name = $Name; status = 'failed'; error = $_.Exception.Message })
        Write-Host "FAIL $Name : $($_.Exception.Message) $($_.ScriptStackTrace)"
    }
}

function New-SourceFixture {
    param([string]$Path)
    [IO.Directory]::CreateDirectory((Join-Path $Path 'plugins')) | Out-Null
    [IO.Directory]::CreateDirectory((Join-Path $Path 'scripts')) | Out-Null
    [IO.Directory]::CreateDirectory((Join-Path $Path 'docs')) | Out-Null
    [IO.File]::WriteAllText((Join-Path $Path 'docs/DEPENDENCIES.md'), "Dependencies`n", $utf8)
    foreach ($name in @('README.md', 'VALIDATION.md')) { [IO.File]::WriteAllText((Join-Path $Path $name), "$name`n", $utf8) }
    foreach ($name in @('Install.ps1', 'Uninstall.ps1', 'scripts/launch.ps1', 'scripts/PluginPath.ps1')) {
        [IO.File]::WriteAllText((Join-Path $Path $name), "param()`nWrite-Output 'fixture'`n", $utf8)
    }
    $bytes = New-Object byte[] 256
    $bytes[0] = 0x4d; $bytes[1] = 0x5a; $bytes[0x3c] = 0x80
    $bytes[0x80] = 0x50; $bytes[0x81] = 0x45
    $bytes[0x84] = 0x64; $bytes[0x85] = 0x86
    $bytes[0x96] = 0x00; $bytes[0x97] = 0x20
    $bytes[0x98] = 0x0b; $bytes[0x99] = 0x02
    [IO.File]::WriteAllBytes((Join-Path $Path 'plugins/MoBuVideoPlayer.dll'), $bytes)
}

function cmake {
    if ($RejectCmake) { throw 'CMake must not be invoked when packaging a supplied DLL.' }
    $arguments = @($args)
    if ($arguments[0] -ne '--install' -or $arguments -notcontains '--config' -or $arguments -notcontains 'Release') {
        throw 'Packaging must install the Release configuration.'
    }
    $destination = $arguments[[Array]::IndexOf($arguments, '--prefix') + 1]
    Copy-Item -Path (Join-Path $InstallSource '*') -Destination $destination -Recurse -Force
    $buildPluginDirectory = Join-Path $fixtureRoot 'mock-build/package/plugins'
    [IO.Directory]::CreateDirectory($buildPluginDirectory) | Out-Null
    Copy-Item -LiteralPath (Join-Path $InstallSource 'plugins/MoBuVideoPlayer.dll') -Destination $buildPluginDirectory -Force
    if ($DifferentBuildOutput) { [IO.File]::WriteAllText((Join-Path $buildPluginDirectory 'MoBuVideoPlayer.dll'), 'a different build') }
    $global:LASTEXITCODE = 0
}

function Invoke-FixturePackage {
    param([string]$Output, [string]$Version = $FixtureVersion)
    & $packageScript -SkipBuild -BuildDirectory (Join-Path $fixtureRoot 'mock-build') -OutputDirectory $Output -ExpectedVersion $Version | Out-Null
    Join-Path $Output "MoBuVideoPlayer-$Version-Mobu2026-win64.zip"
}

function Update-Archive {
    param([string]$Path, [string]$Entry, [string]$Text, [switch]$Delete)
    $archive = [IO.Compression.ZipFile]::Open($Path, [IO.Compression.ZipArchiveMode]::Update)
    try {
        $existing = $archive.GetEntry($Entry)
        if ($existing) { $existing.Delete() }
        if (-not $Delete) {
            $stream = $archive.CreateEntry($Entry).Open()
            try {
                $bytes = $utf8.GetBytes($Text)
                $stream.Write($bytes, 0, $bytes.Length)
            }
            finally { $stream.Dispose() }
        }
    }
    finally { $archive.Dispose() }
}

function Assert-ManifestEncoding {
    param([string]$Path)
    $archive = [IO.Compression.ZipFile]::OpenRead($Path)
    $memory = New-Object IO.MemoryStream
    $stream = $null
    try {
        $stream = $archive.GetEntry('manifest.json').Open()
        $stream.CopyTo($memory)
        $bytes = $memory.ToArray()
        $strictUtf8 = New-Object Text.UTF8Encoding($false, $true)
        $null = $strictUtf8.GetString($bytes)
        Assert-True (-not ($bytes.Length -ge 3 -and $bytes[0] -eq 239 -and $bytes[1] -eq 187 -and $bytes[2] -eq 191)) 'The manifest must not have a UTF-8 BOM.'
        Assert-True ($bytes -notcontains [byte]13) 'The manifest must use LF line endings.'
        Assert-True ($bytes.Length -gt 0 -and $bytes[$bytes.Length - 1] -eq 10) 'The manifest must end with LF.'
    }
    finally {
        if ($stream) { $stream.Dispose() }
        $memory.Dispose()
        $archive.Dispose()
    }
}

try {
    [IO.Directory]::CreateDirectory($fixtureRoot) | Out-Null
    $script:InstallSource = Join-Path $fixtureRoot 'source'
    New-SourceFixture $script:InstallSource
    $output = Join-Path $fixtureRoot 'output'
    $archivePath = $null

    Invoke-Case 'A clean Release package contains the installer and exact declared payload' {
        $script:archivePath = Invoke-FixturePackage $output
        $report = & $verifyScript -ArchivePath $script:archivePath -ExpectedVersion $FixtureVersion -CompareSourceRoot $script:InstallSource
        Assert-True ($report.status -eq 'passed' -and $report.files.Count -eq 8) 'The verified payload differs.'
        Assert-True (Test-Path -LiteralPath ($script:archivePath + '.sha256')) 'The release checksum is absent.'
        Assert-ManifestEncoding $script:archivePath
    }
    Invoke-Case 'Stale files in the old stage never enter a new release' {
        $stage = Join-Path $output "MoBuVideoPlayer-$FixtureVersion-Mobu2026-win64"
        [IO.File]::WriteAllText((Join-Path $stage 'private-sdk.txt'), 'private')
        $null = Invoke-FixturePackage $output
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $stage 'private-sdk.txt'))) 'A stale private file remained.'
        & $verifyScript -ArchivePath $script:archivePath | Out-Null
    }
    Invoke-Case 'Repeated packaging has identical ZIP bytes for identical inputs' {
        $before = (Get-FileHash -LiteralPath $script:archivePath).Hash
        $null = Invoke-FixturePackage ($output + [IO.Path]::DirectorySeparatorChar)
        Assert-True ($before -eq (Get-FileHash -LiteralPath $script:archivePath).Hash) 'The archive is nondeterministic.'
    }
    Invoke-Case 'An unexpected CMake install file fails without replacing the previous release' {
        $before = (Get-FileHash -LiteralPath $script:archivePath).Hash
        $privateFile = Join-Path $script:InstallSource 'fbsdk.dll'
        [IO.File]::WriteAllText($privateFile, 'private SDK')
        try { Assert-Fails { Invoke-FixturePackage $output } 'Unexpected|allowlist' }
        finally { [IO.File]::Delete($privateFile) }
        Assert-True ($before -eq (Get-FileHash -LiteralPath $script:archivePath).Hash) 'A rejected release replaced the valid release.'
    }
    Invoke-Case 'A version mismatch fails before staging' {
        Assert-Fails { Invoke-FixturePackage $output '9.9.9' } 'version'
    }
    Invoke-Case 'The manifest is mandatory' {
        $path = Join-Path $fixtureRoot 'missing-manifest.zip'
        Copy-Item -LiteralPath $script:archivePath -Destination $path
        Update-Archive $path 'manifest.json' '' -Delete
        Assert-Fails { & $verifyScript -ArchivePath $path } 'manifest|Missing'
    }
    Invoke-Case 'Changed payload bytes fail SHA256 verification' {
        $path = Join-Path $fixtureRoot 'tampered.zip'
        Copy-Item -LiteralPath $script:archivePath -Destination $path
        Update-Archive $path 'README.md' 'changed'
        Assert-Fails { & $verifyScript -ArchivePath $path } 'SHA256|hash'
    }
    Invoke-Case 'Traversal entry names are rejected' {
        $path = Join-Path $fixtureRoot 'traversal.zip'
        Copy-Item -LiteralPath $script:archivePath -Destination $path
        Update-Archive $path '../private.txt' 'private'
        Assert-Fails { & $verifyScript -ArchivePath $path } 'Unsafe|Unexpected|allowlist'
    }
    Invoke-Case 'Source comparison detects an installer from another checkout' {
        $path = Join-Path $script:InstallSource 'Install.ps1'
        $original = [IO.File]::ReadAllBytes($path)
        try {
            [IO.File]::WriteAllText($path, 'changed installer', $utf8)
            Assert-Fails { & $verifyScript -ArchivePath $script:archivePath -CompareSourceRoot $script:InstallSource } 'source|Source'
        }
        finally { [IO.File]::WriteAllBytes($path, $original) }
    }
    Invoke-Case 'A non-AMD64 DLL is rejected even with its matching manifest' {
        $path = Join-Path $script:InstallSource 'plugins/MoBuVideoPlayer.dll'
        $original = [IO.File]::ReadAllBytes($path)
        try {
            $bytes = $original.Clone(); $bytes[0x84] = 0x4c; $bytes[0x85] = 0x01
            [IO.File]::WriteAllBytes($path, $bytes)
            Assert-Fails { Invoke-FixturePackage (Join-Path $fixtureRoot 'x86-output') } 'AMD64|x64'
        }
        finally { [IO.File]::WriteAllBytes($path, $original) }
    }
    Invoke-Case 'An installer requiring PowerShell 7 fails the Windows PowerShell 5.1 gate' {
        $path = Join-Path $script:InstallSource 'Install.ps1'
        $original = [IO.File]::ReadAllBytes($path)
        try {
            [IO.File]::WriteAllText($path, '$value = $null ?? "PS7 only"', $utf8)
            Assert-Fails { Invoke-FixturePackage (Join-Path $fixtureRoot 'ps7-output') } 'PowerShell.*syntax|parser'
        }
        finally { [IO.File]::WriteAllBytes($path, $original) }
    }
    Invoke-Case 'A staged DLL from another build is rejected' {
        $script:DifferentBuildOutput = $true
        try { Assert-Fails { Invoke-FixturePackage (Join-Path $fixtureRoot 'different-build') } 'Release build output' }
        finally { $script:DifferentBuildOutput = $false }
    }
    Invoke-Case 'A missing installer is rejected' {
        $path = Join-Path $script:InstallSource 'Install.ps1'
        $original = [IO.File]::ReadAllBytes($path)
        try {
            [IO.File]::Delete($path)
            Assert-Fails { Invoke-FixturePackage (Join-Path $fixtureRoot 'missing-installer') } 'Missing release file.*Install.ps1'
        }
        finally { [IO.File]::WriteAllBytes($path, $original) }
    }
    Invoke-Case 'The manifest version must match the requested release version' {
        Assert-Fails { & $verifyScript -ArchivePath $script:archivePath -ExpectedVersion '9.9.9' } 'version'
    }
    Invoke-Case 'An incorrect SHA256 sidecar is rejected' {
        $checksumPath = $script:archivePath + '.sha256'
        $original = [IO.File]::ReadAllBytes($checksumPath)
        try {
            [IO.File]::WriteAllText($checksumPath, ('0' * 64) + '  wrong.zip', $utf8)
            Assert-Fails { & $verifyScript -ArchivePath $script:archivePath -RequireChecksum } 'SHA256 sidecar'
        }
        finally { [IO.File]::WriteAllBytes($checksumPath, $original) }
    }
    Invoke-Case 'A case-variant duplicate ZIP entry is rejected on Windows' {
        $path = Join-Path $fixtureRoot 'duplicate-entry.zip'
        Copy-Item -LiteralPath $script:archivePath -Destination $path
        Update-Archive $path 'readme.md' 'duplicate'
        Assert-Fails { & $verifyScript -ArchivePath $path } 'Duplicate ZIP entry'
    }
    $prebuiltRoot = Join-Path $fixtureRoot 'prebuilt-project'
    New-SourceFixture $prebuiltRoot
    [IO.File]::WriteAllText((Join-Path $prebuiltRoot 'CMakeLists.txt'), "project(MoBuVideoPlayer VERSION $FixtureVersion LANGUAGES CXX)`n", $utf8)
    Copy-Item -LiteralPath $packageScript -Destination (Join-Path $prebuiltRoot 'scripts/package.ps1')
    Copy-Item -LiteralPath $verifyScript -Destination (Join-Path $prebuiltRoot 'scripts/verify-package.ps1')
    $prebuiltPackageScript = Join-Path $prebuiltRoot 'scripts/package.ps1'
    $inputDirectory = Join-Path $fixtureRoot 'supplied-dll'
    [IO.Directory]::CreateDirectory($inputDirectory) | Out-Null
    $inputPlugin = Join-Path $inputDirectory 'MoBuVideoPlayer.dll'
    Copy-Item -LiteralPath (Join-Path $script:InstallSource 'plugins/MoBuVideoPlayer.dll') -Destination $inputPlugin
    $prebuiltOutput = Join-Path $fixtureRoot 'prebuilt-output'
    $prebuiltArchive = Join-Path $prebuiltOutput "MoBuVideoPlayer-$FixtureVersion-Mobu2026-win64.zip"
    $script:RejectCmake = $true
    try {
        Invoke-Case 'A supplied DLL packages without a build script, SDK or CMake invocation' {
            & $prebuiltPackageScript -PluginPath $inputPlugin -MotionBuilderRoot (Join-Path $fixtureRoot 'absent-sdk') `
                -BuildDirectory (Join-Path $fixtureRoot 'absent-build') -OutputDirectory $prebuiltOutput | Out-Null
            $report = & $verifyScript -ArchivePath $prebuiltArchive -ExpectedVersion $FixtureVersion -RequireChecksum -CompareSourceRoot $prebuiltRoot
            $dll = @($report.files | Where-Object path -eq 'plugins/MoBuVideoPlayer.dll')[0]
            Assert-True ($dll.sha256 -ceq (Get-FileHash -LiteralPath $inputPlugin).Hash) 'The supplied DLL bytes changed.'
            Assert-ManifestEncoding $prebuiltArchive
        }
        Invoke-Case 'Private files beside the supplied DLL are excluded from the release' {
            [IO.File]::WriteAllText((Join-Path $inputDirectory 'private-sdk.lib'), 'private')
            [IO.File]::WriteAllText((Join-Path $prebuiltRoot 'private-settings.txt'), 'private')
            & $prebuiltPackageScript -PluginPath $inputPlugin -OutputDirectory $prebuiltOutput | Out-Null
            $report = & $verifyScript -ArchivePath $prebuiltArchive -RequireChecksum
            Assert-True ($report.files.Count -eq 8) 'The supplied-DLL package includes unexpected files.'
        }
        Invoke-Case 'A missing supplied DLL is rejected before publishing' {
            $before = (Get-FileHash -LiteralPath $prebuiltArchive).Hash
            Assert-Fails { & $prebuiltPackageScript -PluginPath (Join-Path $inputDirectory 'missing.dll') -OutputDirectory $prebuiltOutput } 'supplied DLL|PluginPath|not exist|not found'
            Assert-True ($before -ceq (Get-FileHash -LiteralPath $prebuiltArchive).Hash) 'A missing DLL replaced the existing release.'
        }
        Invoke-Case 'An explicitly empty supplied DLL path cannot switch to SDK build mode' {
            Assert-Fails { & $prebuiltPackageScript -PluginPath '' -OutputDirectory $prebuiltOutput } 'supplied DLL'
        }
        Invoke-Case 'A non-PE supplied DLL is rejected before publishing' {
            $original = [IO.File]::ReadAllBytes($inputPlugin)
            $before = (Get-FileHash -LiteralPath $prebuiltArchive).Hash
            try {
                [IO.File]::WriteAllText($inputPlugin, 'not a DLL', $utf8)
                Assert-Fails { & $prebuiltPackageScript -PluginPath $inputPlugin -OutputDirectory $prebuiltOutput } 'native PE DLL|PE header|AMD64'
                Assert-True ($before -ceq (Get-FileHash -LiteralPath $prebuiltArchive).Hash) 'An invalid DLL replaced the existing release.'
            }
            finally { [IO.File]::WriteAllBytes($inputPlugin, $original) }
        }
        Invoke-Case 'A non-AMD64 supplied DLL is rejected' {
            $original = [IO.File]::ReadAllBytes($inputPlugin)
            try {
                $bytes = $original.Clone(); $bytes[0x84] = 0x4c; $bytes[0x85] = 0x01
                [IO.File]::WriteAllBytes($inputPlugin, $bytes)
                Assert-Fails { & $prebuiltPackageScript -PluginPath $inputPlugin -OutputDirectory $prebuiltOutput } 'AMD64|x64'
            }
            finally { [IO.File]::WriteAllBytes($inputPlugin, $original) }
        }
    }
    finally { $script:RejectCmake = $false }
}
finally {
    if ($ReportPath) {
        $report = [ordered]@{ status = 'passed'; checks = @($script:Results.ToArray()) }
        if (@($script:Results | Where-Object status -ne 'passed').Count) { $report.status = 'failed' }
        $reportFile = [IO.Path]::GetFullPath($ReportPath)
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($reportFile)) | Out-Null
        [IO.File]::WriteAllText($reportFile, ($report | ConvertTo-Json -Depth 6) + "`n", $utf8)
    }
    $resolvedFixture = [IO.Path]::GetFullPath($fixtureRoot)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if ($resolvedFixture.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetFileName($resolvedFixture).StartsWith('MoBuReleaseTests-')) {
        Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
    }
}
if (@($script:Results | Where-Object status -ne 'passed').Count) { throw 'Release package tests failed.' }
Write-Output "PASS $($script:Results.Count) release package checks."
