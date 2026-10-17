function ConvertTo-MoBuPluginFullPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path) -or $Path -match '[\x00-\x1f;]') {
        throw 'Plugin and configuration paths must be nonempty and cannot contain control characters or semicolons.'
    }
    $expanded = [Environment]::ExpandEnvironmentVariables($Path.Trim())
    $resolved = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($expanded)
    $fullPath = [IO.Path]::GetFullPath($resolved)
    $root = [IO.Path]::GetPathRoot($fullPath)
    while ($fullPath.Length -gt $root.Length -and ($fullPath.EndsWith('\') -or $fullPath.EndsWith('/'))) {
        $fullPath = $fullPath.Substring(0, $fullPath.Length - 1)
    }
    return $fullPath
}

function Get-MoBuPluginPathKey {
    param([Parameter(Mandatory = $true)][string]$Path)

    return (ConvertTo-MoBuPluginFullPath -Path $Path).Replace('/', '\').ToUpperInvariant()
}

function Test-MoBuPluginBytesEqual {
    param([byte[]]$Left, [byte[]]$Right)

    if ($Left.Length -ne $Right.Length) { return $false }
    for ($index = 0; $index -lt $Left.Length; $index++) {
        if ($Left[$index] -ne $Right[$index]) { return $false }
    }
    return $true
}

function Read-MoBuPluginPathConfiguration {
    param([Parameter(Mandatory = $true)][byte[]]$Bytes)

    $encoding = New-Object Text.UTF8Encoding($false, $true)
    $hasBom = $Bytes.Length -ge 3 -and $Bytes[0] -eq 239 -and $Bytes[1] -eq 187 -and $Bytes[2] -eq 191
    $offset = 0
    if ($hasBom) { $offset = 3 }
    try { $content = $encoding.GetString($Bytes, $offset, $Bytes.Length - $offset) }
    catch { throw 'The configuration is not valid UTF-8. No changes were made.' }
    if ($content.IndexOf([char]0) -ge 0) { throw 'The configuration contains a NUL character.' }

    $lines = New-Object 'Collections.Generic.List[object]'
    foreach ($match in [regex]::Matches($content, '([^\r\n]*)(\r\n|\n|\r|$)')) {
        if ($match.Length -eq 0) { continue }
        $lines.Add([pscustomobject]@{ Text = $match.Groups[1].Value; End = $match.Groups[2].Value })
    }
    $newline = "`n"
    $newlineMatch = [regex]::Match($content, '\r\n|\n|\r')
    if ($newlineMatch.Success) { $newline = $newlineMatch.Value }
    $sectionIndex = -1
    $sectionEnd = $lines.Count
    $inSection = $false
    $countIndex = -1
    $countPrefix = ''
    $countSuffix = ''
    $count = -1
    $seenKeys = @{}
    $slots = @{}

    for ($index = 0; $index -lt $lines.Count; $index++) {
        $line = $lines[$index].Text
        $trimmed = $line.Trim()
        if ($trimmed.Length -eq 0 -or $trimmed.StartsWith(';') -or $trimmed.StartsWith('#')) { continue }
        if ($trimmed.StartsWith('[')) {
            $header = [regex]::Match($line, '^\s*\[([^\]\r\n]+)\]\s*(?:;.*)?$')
            if (-not $header.Success) { throw 'A configuration section header is malformed.' }
            if ($inSection) { $sectionEnd = $index }
            $inSection = $false
            if ($header.Groups[1].Value.Trim() -ieq 'AdditionnalPluginPath') {
                if ($sectionIndex -ge 0) { throw 'The configuration contains duplicate AdditionnalPluginPath sections.' }
                $sectionIndex = $index
                $inSection = $true
            }
            continue
        }
        if (-not $inSection) { continue }
        $entry = [regex]::Match($line, '^([ \t]*)([^=]+?)([ \t]*=[ \t]*)([^;]*?)([ \t]*(?:;.*)?)$')
        if (-not $entry.Success) { throw 'An additional plugin path entry is malformed.' }
        $key = $entry.Groups[2].Value.Trim()
        if ([string]::IsNullOrWhiteSpace($key) -or $seenKeys.ContainsKey($key)) {
            throw 'The additional plugin path section contains an empty or duplicate key.'
        }
        $seenKeys[$key] = $true
        $value = $entry.Groups[4].Value.Trim()
        if ($key -ieq 'Count') {
            $parsedCount = 0
            if ($value -notmatch '^[0-9]+$' -or -not [int]::TryParse($value, [ref]$parsedCount) -or $parsedCount -gt 4096) {
                throw 'Additional plugin path Count must be an integer from 0 through 4096.'
            }
            $count = $parsedCount
            $countIndex = $index
            $countPrefix = $entry.Groups[1].Value + $entry.Groups[2].Value + $entry.Groups[3].Value
            $countSuffix = $entry.Groups[5].Value
        }
        elseif ($key -imatch '^PlugInPath') {
            $slotMatch = [regex]::Match($key, '^PlugInPath(0|[1-9][0-9]*)$', [Text.RegularExpressions.RegexOptions]::IgnoreCase)
            $slot = 0
            if (-not $slotMatch.Success -or -not [int]::TryParse($slotMatch.Groups[1].Value, [ref]$slot) -or $slot -gt 4095) {
                throw 'An additional plugin path index is malformed or exceeds the safety bound.'
            }
            if ([string]::IsNullOrWhiteSpace($value)) { throw 'An additional plugin path is empty.' }
            $decodedValue = $value.Replace('\\', '\')
            [void](Get-MoBuPluginPathKey -Path $decodedValue)
            $slots[$slot] = [pscustomobject]@{
                Index = $index
                Value = $decodedValue
                RawValue = $value
                Leading = $entry.Groups[1].Value
                Equals = $entry.Groups[3].Value
                Suffix = $entry.Groups[5].Value
            }
        }
    }
    if ($sectionIndex -ge 0) {
        if ($countIndex -lt 0) { throw 'The additional plugin path section has no Count entry.' }
        # Inactive slots are ambiguous; silently reusing one could enable an unrelated plugin.
        if ($slots.Count -ne $count) { throw 'Additional plugin path slots do not match Count; check inactive or missing entries.' }
        for ($slot = 0; $slot -lt $count; $slot++) {
            if (-not $slots.ContainsKey($slot)) { throw 'An additional plugin path index is missing or outside Count.' }
        }
    }

    $paths = New-Object 'Collections.Generic.List[string]'
    $rawPaths = New-Object 'Collections.Generic.List[string]'
    for ($slot = 0; $slot -lt $count; $slot++) {
        $paths.Add($slots[$slot].Value)
        $rawPaths.Add($slots[$slot].RawValue)
    }
    return [pscustomobject]@{
        Lines = $lines; Newline = $newline; HasBom = $hasBom
        HasFinalNewline = $content.EndsWith("`n") -or $content.EndsWith("`r")
        SectionIndex = $sectionIndex; SectionEnd = $sectionEnd
        CountIndex = $countIndex; CountPrefix = $countPrefix; CountSuffix = $countSuffix
        Slots = $slots; Paths = [string[]]$paths.ToArray(); RawPaths = [string[]]$rawPaths.ToArray()
    }
}

function New-MoBuPluginPathConfigurationBytes {
    param(
        [Parameter(Mandatory = $true)]$Configuration,
        [AllowEmptyCollection()][string[]]$Paths,
        [AllowEmptyCollection()][string[]]$RawPaths,
        [AllowEmptyCollection()][int[]]$PathSourceIndices
    )

    $records = New-Object 'Collections.Generic.List[object]'
    $insertIndex = $Configuration.CountIndex
    $slotLines = @{}
    foreach ($slot in $Configuration.Slots.Keys) { $slotLines[$Configuration.Slots[$slot].Index] = $slot }
    if ($Configuration.Slots.Count -gt 0) {
        $insertIndex = ($Configuration.Slots.Values | Measure-Object -Property Index -Maximum).Maximum
    }
    for ($index = 0; $index -lt $Configuration.Lines.Count; $index++) {
        $line = $Configuration.Lines[$index]
        $text = $line.Text
        if ($index -eq $Configuration.CountIndex) {
            $text = $Configuration.CountPrefix + $Paths.Count + $Configuration.CountSuffix
        }
        if ($slotLines.ContainsKey($index)) {
            $slot = $slotLines[$index]
            if ($slot -lt $Paths.Count) {
                $sourceIndex = $PathSourceIndices[$slot]
                $format = $Configuration.Slots[$sourceIndex]
                if ($sourceIndex -eq $slot -and $RawPaths[$slot] -ceq $format.RawValue) {
                    $text = $line.Text
                }
                else {
                    $text = $format.Leading + 'PlugInPath' + $slot + $format.Equals + $RawPaths[$slot] + $format.Suffix
                }
                $records.Add([pscustomobject]@{ Text = $text; End = $line.End })
            }
        }
        else { $records.Add([pscustomobject]@{ Text = $text; End = $line.End }) }
        if ($index -eq $insertIndex) {
            for ($slot = $Configuration.Slots.Count; $slot -lt $Paths.Count; $slot++) {
                $records.Add([pscustomobject]@{ Text = 'PlugInPath' + $slot + ' = ' + $RawPaths[$slot]; End = $Configuration.Newline })
            }
        }
    }
    if ($Configuration.SectionIndex -lt 0) {
        if ($records.Count -gt 0 -and $records[$records.Count - 1].Text.Length -gt 0) {
            $records.Add([pscustomobject]@{ Text = ''; End = $Configuration.Newline })
        }
        $records.Add([pscustomobject]@{ Text = '[AdditionnalPluginPath]'; End = $Configuration.Newline })
        $records.Add([pscustomobject]@{ Text = 'Count = ' + $Paths.Count; End = $Configuration.Newline })
        for ($slot = 0; $slot -lt $Paths.Count; $slot++) {
            $records.Add([pscustomobject]@{ Text = 'PlugInPath' + $slot + ' = ' + $RawPaths[$slot]; End = $Configuration.Newline })
        }
    }
    $builder = New-Object Text.StringBuilder
    for ($index = 0; $index -lt $records.Count; $index++) {
        [void]$builder.Append($records[$index].Text)
        $ending = $records[$index].End
        if ($index -lt $records.Count - 1 -and $ending.Length -eq 0) { $ending = $Configuration.Newline }
        if ($index -eq $records.Count - 1 -and -not $Configuration.HasFinalNewline) { $ending = '' }
        [void]$builder.Append($ending)
    }
    $encoding = New-Object Text.UTF8Encoding($Configuration.HasBom, $true)
    $payload = $encoding.GetBytes($builder.ToString())
    $preamble = $encoding.GetPreamble()
    $output = New-Object byte[] ($preamble.Length + $payload.Length)
    [Array]::Copy($preamble, 0, $output, 0, $preamble.Length)
    [Array]::Copy($payload, 0, $output, $preamble.Length, $payload.Length)
    return ,$output
}

function Invoke-MoBuPluginPathRegistration {
    [CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Medium')]
    param(
        [Parameter(Mandatory = $true)][ValidateSet('Add', 'Remove')][string]$Action,
        [Parameter(Mandatory = $true)][string]$PluginDirectory,
        [string]$ConfigPath
    )

    $ErrorActionPreference = 'Stop'
    $plugin = ConvertTo-MoBuPluginFullPath -Path $PluginDirectory
    $targetKey = Get-MoBuPluginPathKey -Path $plugin
    if ($Action -eq 'Add' -and -not [IO.File]::Exists((Join-Path $plugin 'MoBuVideoPlayer.dll'))) {
        throw 'The plugin directory must contain MoBuVideoPlayer.dll before it can be registered.'
    }
    if ([string]::IsNullOrWhiteSpace($ConfigPath)) {
        if ([string]::IsNullOrWhiteSpace($env:COMPUTERNAME) -or [IO.Path]::GetFileName($env:COMPUTERNAME) -ne $env:COMPUTERNAME) {
            throw 'The machine name is unavailable; specify -ConfigPath explicitly.'
        }
        $documents = [Environment]::GetFolderPath([Environment+SpecialFolder]::MyDocuments)
        $ConfigPath = Join-Path $documents ('MB\2026\config\' + $env:COMPUTERNAME + '.Application.txt')
    }
    $config = ConvertTo-MoBuPluginFullPath -Path $ConfigPath
    if (-not [IO.File]::Exists($config)) {
        throw 'The MotionBuilder configuration does not exist. Start and close MotionBuilder 2026 once, or specify an existing file with -ConfigPath.'
    }
    $original = [IO.File]::ReadAllBytes($config)
    $parsed = Read-MoBuPluginPathConfiguration -Bytes $original
    $before = [string[]]$parsed.Paths
    $afterList = New-Object 'Collections.Generic.List[string]'
    $afterRawList = New-Object 'Collections.Generic.List[string]'
    $afterSources = New-Object 'Collections.Generic.List[int]'
    $hasTarget = $false
    for ($index = 0; $index -lt $before.Count; $index++) {
        $path = $before[$index]
        $matchesTarget = (Get-MoBuPluginPathKey -Path $path) -eq $targetKey
        if ($matchesTarget) { $hasTarget = $true }
        if ($Action -eq 'Add' -or -not $matchesTarget) {
            $afterList.Add($path)
            $afterRawList.Add($parsed.RawPaths[$index])
            $afterSources.Add($index)
        }
    }
    if ($Action -eq 'Add' -and -not $hasTarget) {
        if ($before.Count -ge 4096) { throw 'No additional plugin path slots remain within the safety bound.' }
        foreach ($path in $before) {
            $existingPlugin = Join-Path (ConvertTo-MoBuPluginFullPath -Path $path) 'MoBuVideoPlayer.dll'
            if ([IO.File]::Exists($existingPlugin)) {
                throw ('Another registered directory contains MoBuVideoPlayer.dll. For a dedicated folder, first run Uninstall.ps1 -PluginDirectory "' + $path + '". For a shared folder, move only MoBuVideoPlayer.dll manually; unregistering the whole folder also stops loading its other plugins.')
            }
        }
        $afterList.Add($plugin)
        # SDK preference files escape backslashes; other existing values retain their original bytes.
        $afterRawList.Add($plugin.Replace('\', '\\'))
        $afterSources.Add(-1)
    }
    $after = [string[]]$afterList.ToArray()
    $result = [pscustomobject]@{
        Status = 'Unchanged'; Changed = $false; ConfigPath = $config; BackupPath = $null
        PathsBefore = $before; PathsAfter = $after; PluginDirectory = $plugin
    }
    if (-not $WhatIfPreference -and @(Get-Process -Name motionbuilder -ErrorAction SilentlyContinue).Count -gt 0) {
        # A running host can overwrite a file edited outside its own preferences dialog.
        throw 'Close all MotionBuilder processes before changing plugin registration. Use -WhatIf for a read-only preview.'
    }
    if (($Action -eq 'Add' -and $hasTarget) -or ($Action -eq 'Remove' -and -not $hasTarget)) { return $result }
    $updated = New-MoBuPluginPathConfigurationBytes -Configuration $parsed -Paths $after -RawPaths ([string[]]$afterRawList.ToArray()) -PathSourceIndices ([int[]]$afterSources.ToArray())
    if (-not $PSCmdlet.ShouldProcess($config, ($Action + ' additional plugin path ' + $plugin))) {
        if ($WhatIfPreference) { $result.Status = 'WhatIf' } else { $result.Status = 'Declined' }
        return $result
    }

    $directory = [IO.Path]::GetDirectoryName($config)
    $token = [Guid]::NewGuid().ToString('N')
    $temporary = Join-Path $directory ('.' + [IO.Path]::GetFileName($config) + '.' + $token + '.tmp')
    $backup = Join-Path $directory ([IO.Path]::GetFileName($config) + '.' + $token + '.bak')
    try {
        $stream = New-Object IO.FileStream($temporary, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        try { $stream.Write($updated, 0, $updated.Length); $stream.Flush($true) }
        finally { $stream.Dispose() }
        if (@(Get-Process -Name motionbuilder -ErrorAction SilentlyContinue).Count -gt 0) {
            throw 'MotionBuilder started during registration. No changes were made; close it and retry.'
        }
        if (-not (Test-MoBuPluginBytesEqual -Left $original -Right ([IO.File]::ReadAllBytes($config)))) {
            throw 'The configuration changed during registration. No changes were made; review it and retry.'
        }
        # A same-directory replacement avoids a partially written preferences file after interruption.
        [IO.File]::Replace($temporary, $config, $backup, $true)
        $result.BackupPath = $backup
        $result.Changed = $true
        $result.Status = 'Changed'
        $saved = [IO.File]::ReadAllBytes($config)
        if (-not (Test-MoBuPluginBytesEqual -Left $saved -Right $updated)) {
            throw ('The saved configuration did not match the expected bytes. The original backup is at ' + $backup)
        }
        return $result
    }
    finally {
        if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
    }
}
