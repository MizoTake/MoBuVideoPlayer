[CmdletBinding()]
param(
    [string]$CorePath,
    [string]$ReportPath
)

$ErrorActionPreference = 'Stop'
if (-not $CorePath) { $CorePath = Join-Path (Split-Path -Parent $PSScriptRoot) 'scripts/PluginPath.ps1' }
$script:MockHostRunning = $false
$script:MockHostCalls = 0
$script:MockHostAfterCall = 0
$script:MockMutationPath = $null
$script:MockMutationBytes = $null
$script:Cases = New-Object 'System.Collections.Generic.List[object]'
$script:Results = New-Object 'System.Collections.Generic.List[object]'
$script:Utf8 = New-Object System.Text.UTF8Encoding($false, $true)
$script:FixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('MoBuPluginPathTests-' + [guid]::NewGuid().ToString('N'))
$script:CaseRoot = $null

function Get-Process {
    [CmdletBinding()]
    param([string[]]$Name)
    $script:MockHostCalls++
    if ($script:MockMutationPath -and $script:MockHostCalls -eq 2) {
        [System.IO.File]::WriteAllBytes($script:MockMutationPath, $script:MockMutationBytes)
    }
    if ($script:MockHostRunning -or ($script:MockHostAfterCall -gt 0 -and $script:MockHostCalls -ge $script:MockHostAfterCall)) {
        [pscustomobject]@{ Id = 424242; ProcessName = 'motionbuilder'; Name = 'motionbuilder' }
    }
}

function Set-HostMock {
    param([bool]$Running, [int]$AfterCall = 0, [string]$MutationPath, [byte[]]$MutationBytes)
    $script:MockHostRunning = $Running
    $script:MockHostCalls = 0
    $script:MockHostAfterCall = $AfterCall
    $script:MockMutationPath = $MutationPath
    $script:MockMutationBytes = $MutationBytes
}

function Get-FixtureEncoder {
    $script:Utf8
}

function Add-Test {
    param([string]$Name, [scriptblock]$Body)
    $script:Cases.Add([pscustomobject]@{ Name = $Name; Body = $Body })
}

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Assert-Equal {
    param($Expected, $Actual, [string]$Message)
    if ($Expected -cne $Actual) { throw ($Message + ': expected <' + $Expected + '>, actual <' + $Actual + '>') }
}

function Assert-Paths {
    param([string[]]$Expected, $Actual)
    $values = @($Actual)
    Assert-Equal $Expected.Count $values.Count 'The path count differs'
    for ($index = 0; $index -lt $Expected.Count; $index++) {
        Assert-Equal $Expected[$index] $values[$index] ('The path differs at index ' + $index)
    }
}

function Get-BytesIdentity {
    param([string]$Path)
    [Convert]::ToBase64String([System.IO.File]::ReadAllBytes($Path))
}

function Get-SourceSHA256 {
    param([string]$Path)
    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    $stream = $null
    try {
        $stream = [System.IO.File]::OpenRead([System.IO.Path]::GetFullPath($Path))
        [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()
    } finally {
        if ($null -ne $stream) { $stream.Dispose() }
        $algorithm.Dispose()
    }
}

function New-Fixture {
    param([string]$Content = "Config File Version 1`n`n[AdditionnalPluginPath]`nCount = 0`n", [byte[]]$Bytes)
    $directory = Join-Path $script:CaseRoot ([guid]::NewGuid().ToString('N'))
    $plugin = Join-Path $directory 'plugin folder'
    [void][System.IO.Directory]::CreateDirectory($plugin)
    [System.IO.File]::WriteAllText((Join-Path $plugin 'MoBuVideoPlayer.dll'), 'fixture only', $script:Utf8)
    $config = Join-Path $directory 'FIXTURE.Application.txt'
    if ($null -ne $Bytes) {
        [System.IO.File]::WriteAllBytes($config, $Bytes)
    } else {
        [System.IO.File]::WriteAllText($config, $Content, $script:Utf8)
    }
    [pscustomobject]@{ Root = $directory; Plugin = $plugin; Config = $config }
}

function Read-Fixture {
    param($Fixture)
    [System.IO.File]::ReadAllText($Fixture.Config, $script:Utf8)
}

function Invoke-Registration {
    param($Fixture, [string]$Action = 'Add', [switch]$Preview, [string]$PluginDirectory)
    if (-not $PluginDirectory) { $PluginDirectory = $Fixture.Plugin }
    Invoke-MoBuPluginPathRegistration -Action $Action -PluginDirectory $PluginDirectory -ConfigPath $Fixture.Config -WhatIf:$Preview -Confirm:$false
}

function Invoke-Entry {
    param($Fixture, [string]$Action = 'Add', [switch]$Preview)
    $name = if ($Action -eq 'Add') { 'Install.ps1' } else { 'Uninstall.ps1' }
    $entry = Join-Path (Split-Path -Parent $PSScriptRoot) $name
    & $entry -PluginDirectory $Fixture.Plugin -ConfigPath $Fixture.Config -WhatIf:$Preview -Confirm:$false
}

function Assert-RejectedWithoutChanges {
    param($Fixture, [string]$Action = 'Add', [string]$PluginDirectory)
    $original = Get-BytesIdentity $Fixture.Config
    $filesBefore = @(Get-ChildItem -LiteralPath $Fixture.Root -File -Recurse).Count
    $rejected = $false
    try { $null = Invoke-Registration $Fixture -Action $Action -PluginDirectory $PluginDirectory } catch { $rejected = $true }
    Assert-True $rejected 'The invalid operation was not rejected'
    Assert-Equal $original (Get-BytesIdentity $Fixture.Config) 'Rejected operations must preserve the original bytes'
    Assert-Equal $filesBefore @(Get-ChildItem -LiteralPath $Fixture.Root -File -Recurse).Count 'Rejected operations must not create a backup or temporary file'
}

Add-Test 'Add registers the first path and backs up exact original bytes' {
    $fixture = New-Fixture
    $original = Get-BytesIdentity $fixture.Config
    $result = Invoke-Registration $fixture
    Assert-True $result.Changed 'The first registration must change the configuration'
    Assert-Equal 'Changed' $result.Status 'The registration status differs'
    Assert-Equal $fixture.Config $result.ConfigPath 'The selected configuration differs'
    Assert-Equal $fixture.Plugin $result.PluginDirectory 'The selected plugin directory differs'
    Assert-Paths @() $result.PathsBefore
    Assert-Paths @($fixture.Plugin) $result.PathsAfter
    Assert-True ([System.IO.File]::Exists($result.BackupPath)) 'A backup must exist'
    Assert-Equal $original (Get-BytesIdentity $result.BackupPath) 'The backup must contain the exact previous bytes'
    $text = Read-Fixture $fixture
    Assert-True ($text -match '(?m)^Count\s*=\s*1\s*$') 'Count was not updated'
    Assert-True ($text.Contains('PlugInPath0')) 'The registered path key is missing'
    Assert-True ($text.Contains($fixture.Plugin.Replace('\', '\\'))) 'The stored path must use SDK backslash escaping'
}

Add-Test 'Add appends while preserving existing paths and unrelated settings' {
    $fixture = New-Fixture "Config File Version 1`n[Preferences]`nKeep = yes`n[AdditionnalPluginPath]`nCount = 2`nPlugInPath0 = C:\Other A\plugins`nUnknownKey = retained`nPlugInPath1 = C:\Other B\plugins`n[Render]`nQuality = high`n"
    $result = Invoke-Registration $fixture
    Assert-Paths @('C:\Other A\plugins', 'C:\Other B\plugins') $result.PathsBefore
    Assert-Paths @('C:\Other A\plugins', 'C:\Other B\plugins', $fixture.Plugin) $result.PathsAfter
    $text = Read-Fixture $fixture
    foreach ($retained in @('Keep = yes', 'UnknownKey = retained', 'Quality = high')) {
        Assert-True ($text.Contains($retained)) ('Unrelated setting changed: ' + $retained)
    }
}

Add-Test 'Repeated Add is byte identical and creates no new backup' {
    $fixture = New-Fixture
    $null = Invoke-Registration $fixture
    $original = Get-BytesIdentity $fixture.Config
    $countBefore = @(Get-ChildItem -LiteralPath $fixture.Root -File -Recurse).Count
    $result = Invoke-Registration $fixture
    Assert-True (-not $result.Changed) 'Repeated Add must be unchanged'
    Assert-Equal 'Unchanged' $result.Status 'Repeated Add status differs'
    Assert-Equal $original (Get-BytesIdentity $fixture.Config) 'Repeated Add changed bytes'
    Assert-True ([string]::IsNullOrEmpty($result.BackupPath)) 'Unchanged operations must not create a backup'
    Assert-Equal $countBefore @(Get-ChildItem -LiteralPath $fixture.Root -File -Recurse).Count 'Repeated Add created files'
}

Add-Test 'Add recognizes case slash and trailing separator equivalents' {
    $fixture = New-Fixture
    $existing = $fixture.Plugin.ToUpperInvariant().Replace('\', '/') + '/'
    [System.IO.File]::WriteAllText($fixture.Config, "[AdditionnalPluginPath]`nCount = 1`nPlugInPath0 = $existing`n", $script:Utf8)
    $original = Get-BytesIdentity $fixture.Config
    $result = Invoke-Registration $fixture
    Assert-True (-not $result.Changed) 'Equivalent paths must not be appended'
    Assert-Paths @($existing) $result.PathsAfter
    Assert-Equal $original (Get-BytesIdentity $fixture.Config) 'Equivalent Add changed bytes'
}

Add-Test 'Add recognizes the SDK escaped drive path without rewriting it' {
    $fixture = New-Fixture
    $existing = $fixture.Plugin.ToUpperInvariant() + '\'
    $stored = $existing.Replace('\', '\\')
    [System.IO.File]::WriteAllText($fixture.Config, "[AdditionnalPluginPath]`nCount = 1`nPlugInPath0 = $stored`n", $script:Utf8)
    $original = Get-BytesIdentity $fixture.Config
    $result = Invoke-Registration $fixture
    Assert-True (-not $result.Changed) 'The SDK escaped path was not recognized as the existing target'
    Assert-Paths @($existing) $result.PathsBefore
    Assert-Paths @($existing) $result.PathsAfter
    Assert-Equal $original (Get-BytesIdentity $fixture.Config) 'An existing SDK escaped path was rewritten'
}

Add-Test 'Add creates a missing section without changing prior content' {
    $content = "Config File Version 1`n[Other]`nEnabled = yes`n"
    $fixture = New-Fixture $content
    $result = Invoke-Registration $fixture
    Assert-True $result.Changed 'Add must create the missing section'
    Assert-True ((Read-Fixture $fixture).StartsWith($content)) 'The prior configuration content changed'
    Assert-Paths @($fixture.Plugin) $result.PathsAfter
}

Add-Test 'Remove deletes only the target and compacts the remaining indices' {
    $fixture = New-Fixture
    $content = "[AdditionnalPluginPath]`nCount = 3`nPlugInPath0 = C:\First\plugins`nPlugInPath1 = $($fixture.Plugin)`nUnknown = keep`nPlugInPath2 = C:\Last\plugins`n[Other]`nNumber = 7`n"
    [System.IO.File]::WriteAllText($fixture.Config, $content, $script:Utf8)
    $result = Invoke-Registration $fixture -Action Remove
    Assert-True $result.Changed 'Remove must change the matching configuration'
    Assert-Paths @('C:\First\plugins', 'C:\Last\plugins') $result.PathsAfter
    $text = Read-Fixture $fixture
    Assert-True ($text -match '(?m)^Count\s*=\s*2\s*$') 'Count did not shrink'
    Assert-True ($text -match '(?m)^PlugInPath1\s*=\s*C:\\Last\\plugins\s*$') 'The remaining paths were not compacted'
    Assert-True ($text -notmatch '(?m)^PlugInPath2\s*=') 'An inactive indexed path remains'
    Assert-True ($text.Contains('Unknown = keep') -and $text.Contains('Number = 7')) 'Remove changed unrelated values'
}

Add-Test 'Remove deletes every equivalent target path while keeping other duplicates' {
    $fixture = New-Fixture
    $equivalent = $fixture.Plugin.ToUpperInvariant().Replace('\', '/') + '/'
    [System.IO.File]::WriteAllText($fixture.Config, "[AdditionnalPluginPath]`nCount = 4`nPlugInPath0 = $($fixture.Plugin)`nPlugInPath1 = C:\Other\plugins`nPlugInPath2 = $equivalent`nPlugInPath3 = C:\Other\plugins`n", $script:Utf8)
    $result = Invoke-Registration $fixture -Action Remove
    Assert-Paths @('C:\Other\plugins', 'C:\Other\plugins') $result.PathsAfter
    Assert-True $result.Changed 'Equivalent target paths were not removed'
}

Add-Test 'Remove preserves raw SDK escaped paths and comments while compacting indices' {
    $fixture = New-Fixture
    $targetStored = $fixture.Plugin.Replace('\', '\\')
    $retainedStored = 'C:\\Other Folder\\plugins'
    [System.IO.File]::WriteAllText($fixture.Config, "[AdditionnalPluginPath]`nCount = 2`nPlugInPath0 = $targetStored`nPlugInPath1 = $retainedStored ; keep comment`n", $script:Utf8)
    $result = Invoke-Registration $fixture -Action Remove
    Assert-Paths @('C:\Other Folder\plugins') $result.PathsAfter
    Assert-True ((Read-Fixture $fixture).Contains('PlugInPath0 = ' + $retainedStored + ' ; keep comment')) 'The retained raw path or its comment changed'
}

Add-Test 'Remove decodes an SDK escaped UNC path without accessing the network' {
    $fixture = New-Fixture
    $unc = '\\fixture-server\fixture-share\plugins'
    $stored = $unc.Replace('\', '\\')
    [System.IO.File]::WriteAllText($fixture.Config, "[AdditionnalPluginPath]`nCount = 1`nPlugInPath0 = $stored`n", $script:Utf8)
    $result = Invoke-Registration $fixture -Action Remove -PluginDirectory $unc
    Assert-True $result.Changed 'The SDK escaped UNC target was not removed'
    Assert-Paths @($unc) $result.PathsBefore
    Assert-Paths @() $result.PathsAfter
}

Add-Test 'Remove of the final path preserves a valid empty section' {
    $fixture = New-Fixture
    $null = Invoke-Registration $fixture
    $result = Invoke-Registration $fixture -Action Remove
    Assert-Paths @() $result.PathsAfter
    Assert-True ((Read-Fixture $fixture) -match '(?m)^Count\s*=\s*0\s*$') 'The empty Count is missing'
    Assert-True ((Read-Fixture $fixture) -notmatch '(?m)^PlugInPath[0-9]+\s*=') 'A removed path key remains'
}

Add-Test 'Repeated Remove is byte identical and creates no backup' {
    $fixture = New-Fixture
    $original = Get-BytesIdentity $fixture.Config
    $result = Invoke-Registration $fixture -Action Remove
    Assert-True (-not $result.Changed) 'Removing an absent path must be unchanged'
    Assert-Equal $original (Get-BytesIdentity $fixture.Config) 'Unchanged Remove changed bytes'
    Assert-True ([string]::IsNullOrEmpty($result.BackupPath)) 'Unchanged Remove created a backup'
}

Add-Test 'Remove does not require the plugin directory to still exist' {
    $fixture = New-Fixture
    $missing = Join-Path $fixture.Root 'already removed plugin'
    [System.IO.File]::WriteAllText($fixture.Config, "[AdditionnalPluginPath]`nCount = 1`nPlugInPath0 = $missing`n", $script:Utf8)
    $result = Invoke-Registration $fixture -Action Remove -PluginDirectory $missing
    Assert-True $result.Changed 'A missing plugin directory must still be unregistered'
    Assert-Paths @() $result.PathsAfter
}

Add-Test 'Remove leaves a missing section byte identical' {
    $fixture = New-Fixture "[Other]`nEnabled = yes`n"
    $original = Get-BytesIdentity $fixture.Config
    $result = Invoke-Registration $fixture -Action Remove
    Assert-True (-not $result.Changed) 'Remove must not create a missing section'
    Assert-Equal $original (Get-BytesIdentity $fixture.Config) 'Remove changed the absent-section configuration'
}

foreach ($action in @('Add', 'Remove')) {
    $caseAction = $action
    Add-Test ("WhatIf $caseAction reports proposed paths without writing") ({
        $fixture = New-Fixture
        if ($caseAction -eq 'Remove') { $null = Invoke-Registration $fixture }
        $original = Get-BytesIdentity $fixture.Config
        $fileCount = @(Get-ChildItem -LiteralPath $fixture.Root -File -Recurse).Count
        $result = Invoke-Registration $fixture -Action $caseAction -Preview
        Assert-True (-not $result.Changed) 'WhatIf must not mark a write as completed'
        Assert-Equal 'WhatIf' $result.Status 'WhatIf status differs'
        Assert-Equal $original (Get-BytesIdentity $fixture.Config) 'WhatIf changed bytes'
        Assert-True ([string]::IsNullOrEmpty($result.BackupPath)) 'WhatIf created a backup'
        Assert-Equal $fileCount @(Get-ChildItem -LiteralPath $fixture.Root -File -Recurse).Count 'WhatIf created a file'
        if ($caseAction -eq 'Add') { Assert-Paths @($fixture.Plugin) $result.PathsAfter } else { Assert-Paths @() $result.PathsAfter }
    }.GetNewClosure())
}

foreach ($action in @('Add', 'Remove')) {
    $caseAction = $action
    Add-Test ("Running MotionBuilder blocks $caseAction before any write") ({
        $fixture = New-Fixture
        if ($caseAction -eq 'Remove') { $null = Invoke-Registration $fixture }
        Set-HostMock $true
        Assert-RejectedWithoutChanges $fixture -Action $caseAction
    }.GetNewClosure())
}

Add-Test 'WhatIf is available while MotionBuilder is running' {
    $fixture = New-Fixture
    $original = Get-BytesIdentity $fixture.Config
    $script:MockHostRunning = $true
    $result = Invoke-Registration $fixture -Preview
    Assert-Equal 'WhatIf' $result.Status 'Running-host preview is unavailable'
    Assert-Equal $original (Get-BytesIdentity $fixture.Config) 'Running-host preview changed bytes'
}

Add-Test 'A MotionBuilder start during registration leaves the configuration untouched' {
    $fixture = New-Fixture
    Set-HostMock $false -AfterCall 2
    Assert-RejectedWithoutChanges $fixture
}

Add-Test 'Concurrent configuration changes survive a rejected registration' {
    $fixture = New-Fixture
    $external = "[AdditionnalPluginPath]`nCount = 0`n[External edit]`nKeep = concurrent change`n"
    $changedBytes = $script:Utf8.GetBytes($external)
    $filesBefore = @(Get-ChildItem -LiteralPath $fixture.Root -File -Recurse).Count
    Set-HostMock $false -MutationPath $fixture.Config -MutationBytes $changedBytes
    $rejected = $false
    try { $null = Invoke-Registration $fixture } catch { $rejected = $true }
    Assert-True $rejected 'The concurrent configuration edit was not detected'
    Assert-Equal ([Convert]::ToBase64String($changedBytes)) (Get-BytesIdentity $fixture.Config) 'Registration overwrote the concurrent edit'
    Assert-Equal $filesBefore @(Get-ChildItem -LiteralPath $fixture.Root -File -Recurse).Count 'A rejected concurrent registration left a temporary file or backup'
}

foreach ($lineEnding in @("`n", "`r`n")) {
    foreach ($withBom in @($false, $true)) {
        $caseEnding = $lineEnding
        $caseBom = $withBom
        $label = if ($caseEnding -eq "`n") { 'LF' } else { 'CRLF' }
        Add-Test ("Add and Remove preserve UTF8 $label BOM=$caseBom and Unicode") ({
            $unicode = [string][char]0x65E5 + [char]0x672C + [char]0x8A9E
            $content = @('Config File Version 1', '[Other]', ('Name = ' + $unicode), '[AdditionnalPluginPath]', 'Count = 0', '') -join $caseEnding
            $payload = (Get-FixtureEncoder).GetBytes($content)
            if ($caseBom) { $payload = [byte[]](0xEF, 0xBB, 0xBF) + $payload }
            $fixture = New-Fixture -Bytes $payload
            $null = Invoke-Registration $fixture
            $null = Invoke-Registration $fixture -Action Remove
            $bytes = [System.IO.File]::ReadAllBytes($fixture.Config)
            $hasBom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
            Assert-Equal $caseBom $hasBom 'The UTF8 BOM changed'
            $text = Read-Fixture $fixture
            Assert-True ($text.Contains('Name = ' + $unicode)) 'Unicode settings changed'
            if ($caseEnding -eq "`n") {
                Assert-True (-not $text.Contains("`r")) 'LF changed to CRLF'
            } else {
                Assert-True (-not $text.Replace("`r`n", '').Contains("`n")) 'A bare LF was added to CRLF content'
            }
        }.GetNewClosure())
    }
}

Add-Test 'Add requires a plugin DLL before touching configuration' {
    $fixture = New-Fixture
    [System.IO.File]::Delete((Join-Path $fixture.Plugin 'MoBuVideoPlayer.dll'))
    Assert-RejectedWithoutChanges $fixture
}

Add-Test 'Add rejects a second registered copy of the same plugin' {
    $fixture = New-Fixture
    $otherPlugin = Join-Path $fixture.Root 'previous plugin copy'
    [void][System.IO.Directory]::CreateDirectory($otherPlugin)
    [System.IO.File]::WriteAllText((Join-Path $otherPlugin 'MoBuVideoPlayer.dll'), 'other fixture', $script:Utf8)
    [System.IO.File]::WriteAllText($fixture.Config, "[AdditionnalPluginPath]`nCount = 1`nPlugInPath0 = $otherPlugin`n", $script:Utf8)
    Assert-RejectedWithoutChanges $fixture
}

Add-Test 'Relative plugin paths resolve to the requested fixture directory' {
    $fixture = New-Fixture
    Push-Location -LiteralPath $fixture.Root
    try { $result = Invoke-Registration $fixture -PluginDirectory '.\plugin folder' } finally { Pop-Location }
    Assert-Paths @($fixture.Plugin) $result.PathsAfter
}

Add-Test 'Add and Remove preserve inline comments and final newline absence' {
    $fixture = New-Fixture "[Other]`nKeep = yes ; unchanged`n[AdditionnalPluginPath] ; header`nCount = 0 ; count comment"
    $null = Invoke-Registration $fixture
    $null = Invoke-Registration $fixture -Action Remove
    $text = Read-Fixture $fixture
    Assert-True ($text.Contains('Keep = yes ; unchanged')) 'An unrelated inline comment changed'
    Assert-True ($text.Contains('[AdditionnalPluginPath] ; header')) 'The header comment changed'
    Assert-True ($text.Contains('Count = 0 ; count comment')) 'The Count comment changed'
    Assert-True (-not $text.EndsWith("`n")) 'A final newline was added'
}

Add-Test 'Install and Uninstall entries forward explicit paths and remain idempotent' {
    $fixture = New-Fixture
    $installed = Invoke-Entry $fixture
    Assert-True $installed.Changed 'Install.ps1 did not register the requested directory'
    Assert-Equal $fixture.Config $installed.ConfigPath 'Install.ps1 ignored ConfigPath'
    Assert-Paths @($fixture.Plugin) $installed.PathsAfter
    $removed = Invoke-Entry $fixture -Action Remove
    Assert-True $removed.Changed 'Uninstall.ps1 did not remove the requested directory'
    Assert-Paths @() $removed.PathsAfter
    $original = Get-BytesIdentity $fixture.Config
    $repeated = Invoke-Entry $fixture -Action Remove
    Assert-True (-not $repeated.Changed) 'Repeated Uninstall.ps1 changed configuration'
    Assert-Equal $original (Get-BytesIdentity $fixture.Config) 'Repeated Uninstall.ps1 changed bytes'
    Assert-True ([System.IO.File]::Exists((Join-Path $fixture.Plugin 'MoBuVideoPlayer.dll'))) 'Uninstall.ps1 deleted plugin files'
}

foreach ($action in @('Add', 'Remove')) {
    $caseAction = $action
    Add-Test ("Entry $caseAction forwards WhatIf without writing") ({
        $fixture = New-Fixture
        if ($caseAction -eq 'Remove') { $null = Invoke-Registration $fixture }
        $original = Get-BytesIdentity $fixture.Config
        $fileCount = @(Get-ChildItem -LiteralPath $fixture.Root -File -Recurse).Count
        $result = Invoke-Entry $fixture -Action $caseAction -Preview
        Assert-Equal 'WhatIf' $result.Status 'The entry did not forward WhatIf'
        Assert-True (-not $result.Changed) 'The entry preview reported a completed change'
        Assert-Equal $original (Get-BytesIdentity $fixture.Config) 'The entry preview changed bytes'
        Assert-Equal $fileCount @(Get-ChildItem -LiteralPath $fixture.Root -File -Recurse).Count 'The entry preview created files'
    }.GetNewClosure())
}

$invalidCases = [ordered]@{
    'Negative Count' = "[AdditionnalPluginPath]`nCount = -1`n"
    'Nonnumeric Count' = "[AdditionnalPluginPath]`nCount = one`n"
    'Excessive Count' = "[AdditionnalPluginPath]`nCount = 4097`n"
    'Missing Count' = "[AdditionnalPluginPath]`nUnknown = value`n"
    'Duplicate section' = "[AdditionnalPluginPath]`nCount = 0`n[additionnalpluginpath]`nCount = 0`n"
    'Duplicate Count key' = "[AdditionnalPluginPath]`nCount = 0`ncount = 0`n"
    'Duplicate indexed key' = "[AdditionnalPluginPath]`nCount = 1`nPlugInPath0 = C:\One`npluginpath0 = C:\Two`n"
    'Missing indexed key' = "[AdditionnalPluginPath]`nCount = 2`nPlugInPath0 = C:\One`n"
    'Index gap' = "[AdditionnalPluginPath]`nCount = 2`nPlugInPath0 = C:\One`nPlugInPath2 = C:\Two`n"
    'Out of range indexed key' = "[AdditionnalPluginPath]`nCount = 1`nPlugInPath0 = C:\One`nPlugInPath1 = C:\Two`n"
    'Empty indexed path' = "[AdditionnalPluginPath]`nCount = 1`nPlugInPath0 = `n"
    'Leading zero path index' = "[AdditionnalPluginPath]`nCount = 1`nPlugInPath00 = C:\One`n"
    'Invalid path index' = "[AdditionnalPluginPath]`nCount = 1`nPlugInPathx = C:\One`n"
}
foreach ($invalid in $invalidCases.GetEnumerator()) {
    $caseContent = $invalid.Value
    Add-Test ("$($invalid.Key) is rejected without writes") ({
        Assert-RejectedWithoutChanges (New-Fixture $caseContent)
    }.GetNewClosure())
}

Add-Test 'Invalid UTF8 is rejected without replacing original bytes' {
    $prefix = $script:Utf8.GetBytes("[AdditionnalPluginPath]`nCount = 0`n[Other]`nInvalid = ")
    Assert-RejectedWithoutChanges (New-Fixture -Bytes ([byte[]]($prefix + [byte[]](0xC3, 0x28, 0x0A))))
}

Add-Test 'UTF16 input is rejected without replacing original bytes' {
    $utf16 = [System.Text.Encoding]::Unicode
    $payload = $utf16.GetPreamble() + $utf16.GetBytes("[AdditionnalPluginPath]`r`nCount = 0`r`n")
    Assert-RejectedWithoutChanges (New-Fixture -Bytes $payload)
}

try {
    if (-not (Test-Path -LiteralPath $CorePath -PathType Leaf)) { throw 'The plugin registration implementation is missing.' }
    . $CorePath
    [void][System.IO.Directory]::CreateDirectory($script:FixtureRoot)
    $index = 0
    foreach ($test in $script:Cases) {
        $index++
        Set-HostMock $false
        $script:CaseRoot = Join-Path $script:FixtureRoot $index.ToString('D3')
        [void][System.IO.Directory]::CreateDirectory($script:CaseRoot)
        try {
            & $test.Body
            $script:Results.Add([pscustomobject]@{ Name = $test.Name; Passed = $true; Error = $null })
            Write-Host ('PASS ' + $test.Name)
        } catch {
            $script:Results.Add([pscustomobject]@{ Name = $test.Name; Passed = $false; Error = $_.Exception.Message })
            Write-Host ('FAIL ' + $test.Name + ': ' + $_.Exception.Message)
        }
    }
    $failed = @($script:Results | Where-Object { -not $_.Passed }).Count
    $report = [pscustomobject]@{
        PowerShell = $PSVersionTable.PSVersion.ToString()
        Total = $script:Results.Count
        Passed = $script:Results.Count - $failed
        Failed = $failed
        Sources = [pscustomobject]@{
            CoreSHA256 = Get-SourceSHA256 $CorePath
            TestsSHA256 = Get-SourceSHA256 $PSCommandPath
            InstallSHA256 = Get-SourceSHA256 (Join-Path (Split-Path -Parent $PSScriptRoot) 'Install.ps1')
            UninstallSHA256 = Get-SourceSHA256 (Join-Path (Split-Path -Parent $PSScriptRoot) 'Uninstall.ps1')
        }
        Tests = $script:Results.ToArray()
    }
    if ($ReportPath) {
        $reportAbsolute = [System.IO.Path]::GetFullPath($ReportPath)
        [void][System.IO.Directory]::CreateDirectory((Split-Path -Parent $reportAbsolute))
        [System.IO.File]::WriteAllText($reportAbsolute, (($report | ConvertTo-Json -Depth 5) + "`n"), $script:Utf8)
    }
    Write-Host ('Result: ' + $report.Passed + '/' + $report.Total + ' passed on PowerShell ' + $report.PowerShell)
    if ($failed -ne 0) { throw ($failed.ToString() + ' plugin path tests failed.') }
} finally {
    $expected = [System.IO.Path]::GetFullPath($script:FixtureRoot)
    $temporary = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if ($expected.StartsWith($temporary, [System.StringComparison]::OrdinalIgnoreCase) -and
        [System.IO.Path]::GetFileName($expected).StartsWith('MoBuPluginPathTests-') -and
        (Test-Path -LiteralPath $expected -PathType Container)) {
        Remove-Item -LiteralPath $expected -Recurse -Force
    }
}
