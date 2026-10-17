#requires -Version 7.0
[CmdletBinding()]
param([string]$ReportPath = '')

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$prepareScript = Join-Path $projectRoot 'scripts/prepare-release-input.ps1'
$verifyScript = Join-Path $projectRoot 'scripts/verify-release-input.ps1'
$fixtureRoot = Join-Path ([IO.Path]::GetTempPath()) ('MoBuReleaseInputTests-' + [guid]::NewGuid().ToString('N'))
$fixtureVersion = '0.2.1'
$utf8 = [Text.UTF8Encoding]::new($false)
$results = [Collections.Generic.List[object]]::new()

function Assert-InputTest {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Get-InputTestHash {
    param([byte[]]$Bytes)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($algorithm.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $algorithm.Dispose() }
}

function Write-InputTestMetadata {
    param($Fixture, $Metadata)
    [IO.File]::WriteAllText($Fixture.MetadataPath, ($Metadata | ConvertTo-Json -Depth 8) + "`n", $utf8)
}

function New-InputTestFixture {
    param([switch]$SkipPrepare)
    $caseRoot = Join-Path $fixtureRoot ([guid]::NewGuid().ToString('N'))
    $sourceRoot = Join-Path $caseRoot 'source'
    $output = Join-Path $caseRoot 'input'
    [void][IO.Directory]::CreateDirectory((Join-Path $sourceRoot 'src/media'))
    [void][IO.Directory]::CreateDirectory((Join-Path $sourceRoot 'include/mobu/media'))
    [IO.File]::WriteAllText((Join-Path $sourceRoot 'CMakeLists.txt'), "project(MoBuVideoPlayer VERSION $fixtureVersion LANGUAGES CXX)`n", $utf8)
    [IO.File]::WriteAllText((Join-Path $sourceRoot 'src/media/Fixture.cpp'), "int Fixture() { return 7; }`n", $utf8)
    [IO.File]::WriteAllText((Join-Path $sourceRoot 'include/mobu/media/Fixture.h'), "int Fixture();`n", $utf8)
    $sourcePlugin = Join-Path $caseRoot 'tested-plugin.dll'
    $bytes = [byte[]]::new(256)
    $bytes[0] = 0x4d; $bytes[1] = 0x5a; $bytes[0x3c] = 0x80
    $bytes[0x80] = 0x50; $bytes[0x81] = 0x45
    $bytes[0x84] = 0x64; $bytes[0x85] = 0x86
    $bytes[0x96] = 0x00; $bytes[0x97] = 0x20
    $bytes[0x98] = 0x0b; $bytes[0x99] = 0x02
    [IO.File]::WriteAllBytes($sourcePlugin, $bytes)
    $fixture = [pscustomobject]@{
        Root = $caseRoot
        SourceRoot = $sourceRoot
        OutputDirectory = $output
        SourcePlugin = $sourcePlugin
        PluginPath = ''
        MetadataPath = ''
    }
    if (-not $SkipPrepare) {
        & $prepareScript -PluginPath $sourcePlugin -OutputDirectory $output -SourceRoot $sourceRoot | Out-Null
        $dlls = @(Get-ChildItem -LiteralPath $output -File -Filter '*.dll')
        $metadata = @(Get-ChildItem -LiteralPath $output -File -Filter '*.json')
        Assert-InputTest ($dlls.Count -eq 1 -and $metadata.Count -eq 1) 'Preparation must emit one DLL and one JSON input.'
        $fixture.PluginPath = $dlls[0].FullName
        $fixture.MetadataPath = $metadata[0].FullName
    }
    return $fixture
}

function Invoke-InputTestVerify {
    param($Fixture, [string]$ExpectedVersion = $fixtureVersion, [string]$VerificationReport = '')
    $arguments = @{
        PluginPath = $Fixture.PluginPath
        MetadataPath = $Fixture.MetadataPath
        ExpectedVersion = $ExpectedVersion
        SourceRoot = $Fixture.SourceRoot
    }
    if ($VerificationReport) { $arguments.ReportPath = $VerificationReport }
    & $verifyScript @arguments | Out-Null
}

function Assert-InputTestRejected {
    param([scriptblock]$Body)
    $rejected = $false
    try { & $Body | Out-Null }
    catch { $rejected = $true }
    Assert-InputTest $rejected 'Invalid release input was accepted.'
}

function Invoke-InputTestCase {
    param([string]$Name, [scriptblock]$Body)
    try {
        & $Body
        $results.Add([pscustomobject]@{ name = $Name; status = 'passed' })
        Write-Output "PASS $Name"
    }
    catch {
        $results.Add([pscustomobject]@{ name = $Name; status = 'failed'; error = $_.Exception.Message })
        Write-Output "FAIL $Name : $($_.Exception.Message)"
    }
}

function Invoke-InputTestLocation {
    param([string]$WorkingDirectory, [string]$ProcessDirectory, [scriptblock]$Body)
    $locationBefore = Get-Location
    $processBefore = [IO.Directory]::GetCurrentDirectory()
    try {
        Set-Location -LiteralPath $WorkingDirectory
        [IO.Directory]::SetCurrentDirectory($ProcessDirectory)
        Assert-InputTest ([IO.Path]::GetFullPath('relative-location-probe') -cne (Join-Path $WorkingDirectory 'relative-location-probe')) 'The regression setup did not separate PowerShell and process locations.'
        & $Body
        Assert-InputTest ([IO.Directory]::GetCurrentDirectory() -ceq $ProcessDirectory) 'A release entry changed the process working directory.'
    }
    finally {
        Set-Location -LiteralPath $locationBefore.Path
        [IO.Directory]::SetCurrentDirectory($processBefore)
    }
}

try {
    [void][IO.Directory]::CreateDirectory($fixtureRoot)

    Invoke-InputTestCase 'Prepared release input contains the exact tested DLL and only public metadata' {
        $fixture = New-InputTestFixture
        $preparedBytes = [IO.File]::ReadAllBytes($fixture.PluginPath)
        Assert-InputTest ((Get-InputTestHash $preparedBytes) -ceq (Get-InputTestHash ([IO.File]::ReadAllBytes($fixture.SourcePlugin)))) 'The prepared DLL differs from the selected DLL.'
        $metadataBytes = [IO.File]::ReadAllBytes($fixture.MetadataPath)
        $metadataText = ([Text.UTF8Encoding]::new($false, $true)).GetString($metadataBytes)
        Assert-InputTest (-not ($metadataBytes.Length -ge 3 -and $metadataBytes[0] -eq 0xef -and $metadataBytes[1] -eq 0xbb -and $metadataBytes[2] -eq 0xbf)) 'The public input metadata contains a UTF-8 BOM.'
        Assert-InputTest (-not $metadataText.Contains("`r") -and $metadataText.EndsWith("`n")) 'The public input metadata must use LF and end with a newline.'
        $metadata = $metadataText | ConvertFrom-Json
        $expectedKeys = @('format_version', 'name', 'version', 'host', 'platform', 'dll_sha256', 'native_source_sha256', 'native_files')
        $actualKeys = @($metadata.PSObject.Properties.Name)
        Assert-InputTest ($actualKeys.Count -eq $expectedKeys.Count -and @($actualKeys | Where-Object { $_ -cnotin $expectedKeys }).Count -eq 0) 'The input metadata contains undeclared fields.'
        Assert-InputTest ($metadata.format_version -eq 1 -and $metadata.name -ceq 'MoBuVideoPlayer' -and $metadata.version -ceq $fixtureVersion -and $metadata.host -ceq 'MotionBuilder 2026' -and $metadata.platform -ceq 'Windows x64') 'The input metadata identity differs.'
        Assert-InputTest ($metadata.dll_sha256 -ceq (Get-InputTestHash $preparedBytes)) 'The metadata DLL hash differs.'
        Assert-InputTest ($metadataText -notmatch '(?i)[a-z]:[\\/]|/(?:home|Users)/') 'The input metadata contains an absolute local path.'
        $verificationReport = Join-Path $fixture.Root 'verification.json'
        Invoke-InputTestVerify $fixture -VerificationReport $verificationReport
        Assert-InputTest ([IO.File]::Exists($verificationReport)) 'The verifier did not write its requested report.'
        $reportText = [IO.File]::ReadAllText($verificationReport, $utf8)
        $null = $reportText | ConvertFrom-Json
        Assert-InputTest ($reportText -notmatch '(?i)[a-z]:[\\/]|/(?:home|Users)/') 'The public verification report contains an absolute local path.'
    }

    Invoke-InputTestCase 'The source fingerprint includes sorted CMake, headers, and implementation hashes' {
        $fixture = New-InputTestFixture
        $metadata = [IO.File]::ReadAllText($fixture.MetadataPath, $utf8) | ConvertFrom-Json
        $expectedPaths = @('CMakeLists.txt', 'include/mobu/media/Fixture.h', 'src/media/Fixture.cpp')
        Assert-InputTest (@($metadata.native_files).Count -eq $expectedPaths.Count) 'The source inventory differs.'
        $fingerprintText = ''
        for ($index = 0; $index -lt $expectedPaths.Count; $index++) {
            $path = $expectedPaths[$index]
            $hash = Get-InputTestHash ([IO.File]::ReadAllBytes((Join-Path $fixture.SourceRoot $path)))
            Assert-InputTest ($metadata.native_files[$index].path -ceq $path -and $metadata.native_files[$index].sha256 -ceq $hash) 'The source inventory is incomplete, unsorted, or has an incorrect hash.'
            $fingerprintText += $hash + '  ' + $path + "`n"
        }
        Assert-InputTest ($metadata.native_source_sha256 -ceq (Get-InputTestHash ($utf8.GetBytes($fingerprintText)))) 'The native source fingerprint differs from the documented hash format.'
    }

    Invoke-InputTestCase 'Changing the DLL after preparation is rejected' {
        $fixture = New-InputTestFixture
        $bytes = [IO.File]::ReadAllBytes($fixture.PluginPath)
        $bytes[0xff] = 7
        [IO.File]::WriteAllBytes($fixture.PluginPath, $bytes)
        Assert-InputTestRejected { Invoke-InputTestVerify $fixture }
    }

    Invoke-InputTestCase 'A release version different from the selected input is rejected' {
        $fixture = New-InputTestFixture
        Assert-InputTestRejected { Invoke-InputTestVerify $fixture -ExpectedVersion '9.9.9' }
    }

    Invoke-InputTestCase 'Changing a native source file after preparation is rejected' {
        $fixture = New-InputTestFixture
        [IO.File]::WriteAllText((Join-Path $fixture.SourceRoot 'src/media/Fixture.cpp'), "int Fixture() { return 8; }`n", $utf8)
        Assert-InputTestRejected { Invoke-InputTestVerify $fixture }
    }

    Invoke-InputTestCase 'Adding a native source file after preparation is rejected' {
        $fixture = New-InputTestFixture
        [IO.File]::WriteAllText((Join-Path $fixture.SourceRoot 'src/media/Added.cpp'), "int Added() { return 1; }`n", $utf8)
        Assert-InputTestRejected { Invoke-InputTestVerify $fixture }
    }

    Invoke-InputTestCase 'Removing a native header after preparation is rejected' {
        $fixture = New-InputTestFixture
        [IO.File]::Delete((Join-Path $fixture.SourceRoot 'include/mobu/media/Fixture.h'))
        Assert-InputTestRejected { Invoke-InputTestVerify $fixture }
    }

    Invoke-InputTestCase 'A missing selected DLL is rejected' {
        $fixture = New-InputTestFixture
        [IO.File]::Delete($fixture.PluginPath)
        Assert-InputTestRejected { Invoke-InputTestVerify $fixture }
    }

    Invoke-InputTestCase 'Missing release input metadata is rejected' {
        $fixture = New-InputTestFixture
        [IO.File]::Delete($fixture.MetadataPath)
        Assert-InputTestRejected { Invoke-InputTestVerify $fixture }
    }

    Invoke-InputTestCase 'A non-AMD64 DLL is rejected even with a matching declared hash' {
        $fixture = New-InputTestFixture
        $bytes = [IO.File]::ReadAllBytes($fixture.PluginPath)
        $bytes[0x84] = 0x4c; $bytes[0x85] = 0x01
        [IO.File]::WriteAllBytes($fixture.PluginPath, $bytes)
        $metadata = [IO.File]::ReadAllText($fixture.MetadataPath, $utf8) | ConvertFrom-Json
        $metadata.dll_sha256 = Get-InputTestHash $bytes
        Write-InputTestMetadata $fixture $metadata
        Assert-InputTestRejected { Invoke-InputTestVerify $fixture }
    }

    Invoke-InputTestCase 'A native executable without the DLL flag is rejected' {
        $fixture = New-InputTestFixture
        $bytes = [IO.File]::ReadAllBytes($fixture.PluginPath)
        $bytes[0x97] = 0x00
        [IO.File]::WriteAllBytes($fixture.PluginPath, $bytes)
        $metadata = [IO.File]::ReadAllText($fixture.MetadataPath, $utf8) | ConvertFrom-Json
        $metadata.dll_sha256 = Get-InputTestHash $bytes
        Write-InputTestMetadata $fixture $metadata
        Assert-InputTestRejected { Invoke-InputTestVerify $fixture }
    }

    Invoke-InputTestCase 'A metadata source fingerprint different from the source inventory is rejected' {
        $fixture = New-InputTestFixture
        $metadata = [IO.File]::ReadAllText($fixture.MetadataPath, $utf8) | ConvertFrom-Json
        $metadata.native_source_sha256 = '0' * 64
        Write-InputTestMetadata $fixture $metadata
        Assert-InputTestRejected { Invoke-InputTestVerify $fixture }
    }

    Invoke-InputTestCase 'An undeclared local metadata field is rejected' {
        $fixture = New-InputTestFixture
        $metadata = [IO.File]::ReadAllText($fixture.MetadataPath, $utf8) | ConvertFrom-Json
        $metadata | Add-Member -MemberType NoteProperty -Name local_build_path -Value $fixture.SourceRoot
        Write-InputTestMetadata $fixture $metadata
        Assert-InputTestRejected { Invoke-InputTestVerify $fixture }
    }

    Invoke-InputTestCase 'Metadata with a missing required field is rejected' {
        $fixture = New-InputTestFixture
        $metadata = [IO.File]::ReadAllText($fixture.MetadataPath, $utf8) | ConvertFrom-Json
        $metadata.PSObject.Properties.Remove('host')
        Write-InputTestMetadata $fixture $metadata
        Assert-InputTestRejected { Invoke-InputTestVerify $fixture }
    }

    Invoke-InputTestCase 'Preparation rejects a file that is not a native DLL' {
        $fixture = New-InputTestFixture -SkipPrepare
        [IO.File]::WriteAllText($fixture.SourcePlugin, 'a DLL must contain a PE image', $utf8)
        Assert-InputTestRejected {
            & $prepareScript -PluginPath $fixture.SourcePlugin -OutputDirectory $fixture.OutputDirectory -SourceRoot $fixture.SourceRoot
        }
    }
    $unicodeName = (-join [char[]]@(0x65e5, 0x672c, 0x8a9e)) + ' path'
    $processDirectory = Join-Path $fixtureRoot 'process-directory'
    [void][IO.Directory]::CreateDirectory($processDirectory)
    Invoke-InputTestCase 'Preparation resolves relative source, DLL and output from PowerShell location in a Unicode path' {
        $fixture = New-InputTestFixture -SkipPrepare
        $unicodeCase = Join-Path $fixtureRoot ($unicodeName + ' ' + [guid]::NewGuid().ToString('N'))
        Move-Item -LiteralPath $fixture.Root -Destination $unicodeCase
        $relativeRoot = [IO.Path]::GetFileName($unicodeCase)
        $preparedDirectory = Join-Path $unicodeCase 'prepared input'
        Invoke-InputTestLocation $fixtureRoot $processDirectory {
            & $prepareScript -SourceRoot (Join-Path $relativeRoot 'source') `
                -PluginPath (Join-Path $relativeRoot 'tested-plugin.dll') `
                -OutputDirectory (Join-Path $relativeRoot 'prepared input') | Out-Null
        }
        Assert-InputTest (@(Get-ChildItem -LiteralPath $preparedDirectory -Filter '*.dll').Count -eq 1) 'The relative DLL output was not written under PowerShell location.'
        Assert-InputTest (@(Get-ChildItem -LiteralPath $preparedDirectory -Filter '*.json').Count -eq 1) 'The relative metadata output was not written under PowerShell location.'
    }
    Invoke-InputTestCase 'Verification resolves relative DLL, metadata, source and report from PowerShell location' {
        $fixture = New-InputTestFixture
        $relativeRoot = [IO.Path]::GetFileName($fixture.Root)
        $reportName = $unicodeName + ' report.json'
        Invoke-InputTestLocation $fixtureRoot $processDirectory {
            & $verifyScript -PluginPath (Join-Path $relativeRoot ('input/' + [IO.Path]::GetFileName($fixture.PluginPath))) `
                -MetadataPath (Join-Path $relativeRoot ('input/' + [IO.Path]::GetFileName($fixture.MetadataPath))) `
                -SourceRoot (Join-Path $relativeRoot 'source') -ExpectedVersion $fixtureVersion `
                -ReportPath (Join-Path $relativeRoot $reportName) | Out-Null
        }
        Assert-InputTest ([IO.File]::Exists((Join-Path $fixture.Root $reportName))) 'The verification report was written relative to the process directory.'
    }
    Invoke-InputTestCase 'A relative verification report is written under PowerShell location when all inputs are absolute' {
        $fixture = New-InputTestFixture
        $relativeReport = Join-Path ([IO.Path]::GetFileName($fixture.Root)) ($unicodeName + ' report.json')
        Invoke-InputTestLocation $fixtureRoot $processDirectory { Invoke-InputTestVerify $fixture -VerificationReport $relativeReport }
        Assert-InputTest ([IO.File]::Exists((Join-Path $fixtureRoot $relativeReport))) 'The absolute-input report used the process directory.'
    }
    Invoke-InputTestCase 'Release input paths support a FileSystem PowerShell drive' {
        $fixture = New-InputTestFixture -SkipPrepare
        $driveName = 'MoBuInput' + [guid]::NewGuid().ToString('N').Substring(0, 8)
        $null = New-PSDrive -Name $driveName -PSProvider FileSystem -Root $fixture.Root
        try {
            Invoke-InputTestLocation $fixtureRoot $processDirectory {
                & $prepareScript -SourceRoot ($driveName + ':\source') -PluginPath ($driveName + ':\tested-plugin.dll') `
                    -OutputDirectory ($driveName + ':\' + $unicodeName) | Out-Null
            }
            Assert-InputTest (@(Get-ChildItem -LiteralPath (Join-Path $fixture.Root $unicodeName) -Filter '*.json').Count -eq 1) 'The FileSystem drive output was not created in its target.'
        }
        finally { Remove-PSDrive -Name $driveName }
    }
    Invoke-InputTestCase 'Preparation accepts FileSystem-qualified paths with Unicode, brackets, spaces and a tilde' {
        $fixture = New-InputTestFixture -SkipPrepare
        $output = Join-Path $fixture.Root ($unicodeName + ' [media] ~ output')
        Invoke-InputTestLocation $fixtureRoot $processDirectory {
            & $prepareScript -SourceRoot ('FileSystem::' + $fixture.SourceRoot) -PluginPath ('FileSystem::' + $fixture.SourcePlugin) `
                -OutputDirectory ('FileSystem::' + $output) | Out-Null
        }
        Assert-InputTest (@(Get-ChildItem -LiteralPath $output -Filter '*.json').Count -eq 1) 'The qualified output path was treated as wildcard syntax.'
    }
    Invoke-InputTestCase 'Non-FileSystem source paths are explicitly rejected' {
        $fixture = New-InputTestFixture -SkipPrepare
        $rejected = $false
        try { & $prepareScript -SourceRoot 'Env:PATH' -PluginPath $fixture.SourcePlugin -OutputDirectory $fixture.OutputDirectory | Out-Null }
        catch {
            $rejected = $_.Exception.Message -match 'FileSystem'
        }
        Assert-InputTest $rejected 'The non-FileSystem provider was not explicitly rejected.'
    }
    Invoke-InputTestCase 'FileSystem-qualified relative source and parent paths resolve from PowerShell location' {
        $fixture = New-InputTestFixture -SkipPrepare
        $outputName = $unicodeName + ' qualified relative'
        Invoke-InputTestLocation $fixture.SourceRoot $processDirectory {
            & $prepareScript -SourceRoot 'FileSystem::.' -PluginPath 'FileSystem::..\tested-plugin.dll' `
                -OutputDirectory ('FileSystem::..\' + $outputName) | Out-Null
        }
        Assert-InputTest (@(Get-ChildItem -LiteralPath (Join-Path $fixture.Root $outputName) -Filter '*.json').Count -eq 1) 'A provider-qualified relative path used the process directory.'
    }
    Invoke-InputTestCase 'FileSystem-qualified root-relative paths use the current PowerShell drive root' {
        $fixture = New-InputTestFixture -SkipPrepare
        $driveName = 'MoBuRoot' + [guid]::NewGuid().ToString('N').Substring(0, 8)
        $null = New-PSDrive -Name $driveName -PSProvider FileSystem -Root $fixture.Root
        try {
            Invoke-InputTestLocation ($driveName + ':\source') $processDirectory {
                & $prepareScript -SourceRoot 'FileSystem::\source' -PluginPath 'FileSystem::\tested-plugin.dll' `
                    -OutputDirectory 'FileSystem::\root relative output' | Out-Null
            }
            Assert-InputTest (@(Get-ChildItem -LiteralPath (Join-Path $fixture.Root 'root relative output') -Filter '*.json').Count -eq 1) 'A root-relative provider path ignored the PowerShell drive root.'
        }
        finally { Remove-PSDrive -Name $driveName }
    }
}
finally {
    if ($ReportPath) {
        $report = [ordered]@{ status = 'passed'; checks = @($results.ToArray()) }
        if (@($results | Where-Object status -ne 'passed').Count) { $report.status = 'failed' }
        $reportFile = [IO.Path]::GetFullPath($ReportPath)
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($reportFile))
        [IO.File]::WriteAllText($reportFile, ($report | ConvertTo-Json -Depth 6) + "`n", $utf8)
    }
    $resolvedFixture = [IO.Path]::GetFullPath($fixtureRoot)
    $temporary = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
    if ([IO.Path]::GetDirectoryName($resolvedFixture).TrimEnd('\', '/') -ine $temporary -or
        -not [IO.Path]::GetFileName($resolvedFixture).StartsWith('MoBuReleaseInputTests-')) {
        throw 'Unexpected release input fixture cleanup path.'
    }
    Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
}
if (@($results | Where-Object status -ne 'passed').Count) { throw 'Release input tests failed.' }
Write-Output "PASS $($results.Count) release input checks."
