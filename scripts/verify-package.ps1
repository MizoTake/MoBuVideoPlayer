#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ArchivePath,
    [string]$ExpectedVersion = '',
    [string]$CompareSourceRoot = '',
    [switch]$RequireChecksum,
    [string]$ReportPath = ''
)

$ErrorActionPreference = 'Stop'
$expectedFiles = @('Install.ps1', 'README.md', 'Uninstall.ps1', 'VALIDATION.md',
    'docs/DEPENDENCIES.md', 'plugins/MoBuVideoPlayer.dll', 'scripts/PluginPath.ps1', 'scripts/launch.ps1')
$utf8 = New-Object Text.UTF8Encoding($false, $true)
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archiveFile = Get-Item -LiteralPath $ArchivePath -ErrorAction Stop
$archive = [IO.Compression.ZipFile]::OpenRead($archiveFile.FullName)
$result = $null

function Get-EntryBytes {
    param([IO.Compression.ZipArchiveEntry]$Entry)
    $stream = $Entry.Open()
    $memory = New-Object IO.MemoryStream
    try {
        $stream.CopyTo($memory)
        return ,$memory.ToArray()
    }
    finally { $memory.Dispose(); $stream.Dispose() }
}

function Get-BytesHash {
    param([byte[]]$Bytes)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($algorithm.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $algorithm.Dispose() }
}

function Assert-NativePlugin {
    param([byte[]]$Bytes)
    if ($Bytes.Length -lt 64 -or $Bytes[0] -ne 0x4d -or $Bytes[1] -ne 0x5a) { throw 'The plugin is not a native PE DLL.' }
    $offset = [BitConverter]::ToUInt32($Bytes, 0x3c)
    if ($offset -gt $Bytes.Length - 26 -or [BitConverter]::ToUInt32($Bytes, $offset) -ne 0x00004550) {
        throw 'The plugin PE header is malformed.'
    }
    if ([BitConverter]::ToUInt16($Bytes, $offset + 4) -ne 0x8664 -or
        [BitConverter]::ToUInt16($Bytes, $offset + 24) -ne 0x20b) { throw 'The plugin must be an AMD64 PE32+ DLL.' }
    if (([BitConverter]::ToUInt16($Bytes, $offset + 22) -band 0x2000) -eq 0) { throw 'The native plugin does not have the PE DLL flag.' }
}

function Assert-WindowsPowerShellSyntax {
    param([hashtable]$Scripts)
    if ($PSVersionTable.PSVersion.Major -eq 5) { return $PSVersionTable.PSVersion.ToString() }
    $windowsPowerShell = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
    if (-not (Test-Path -LiteralPath $windowsPowerShell -PathType Leaf)) { throw 'Windows PowerShell 5.1 is required to check the installer scripts.' }
    $parserCode = @'
$ErrorActionPreference = 'Stop'
try {
    $scripts = [Console]::In.ReadToEnd() | ConvertFrom-Json
    foreach ($property in $scripts.PSObject.Properties) {
        $text = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($property.Value))
        $tokens = $null
        $errors = $null
        [Management.Automation.Language.Parser]::ParseInput($text, [ref]$tokens, [ref]$errors) | Out-Null
        if ($errors.Count) { throw ('PowerShell 5.1 syntax error in ' + $property.Name + ': ' + $errors[0].Message) }
    }
    [Console]::Out.WriteLine($PSVersionTable.PSVersion.ToString())
    exit 0
}
catch { [Console]::Error.WriteLine($_.Exception.Message); exit 1 }
'@
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $windowsPowerShell
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($parserCode))
    $start.Arguments = '-NoProfile -NonInteractive -EncodedCommand ' + $encoded
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $start
    try {
        $null = $process.Start()
        $process.StandardInput.Write(($Scripts | ConvertTo-Json -Compress))
        $process.StandardInput.Close()
        if (-not $process.WaitForExit(30000)) {
            $process.Kill()
            throw 'Windows PowerShell 5.1 syntax checking timed out.'
        }
        $output = $process.StandardOutput.ReadToEnd().Trim()
        $errorText = $process.StandardError.ReadToEnd().Trim()
        if ($process.ExitCode -ne 0) { throw "Windows PowerShell 5.1 parser failed: $errorText" }
        return $output
    }
    finally { $process.Dispose() }
}

try {
    $entries = @{}
    foreach ($entry in $archive.Entries) {
        $name = $entry.FullName
        if ($name -match '(^/|\\|:|(^|/)\.\.?(/|$)|[\x00-\x1f])' -or $name.EndsWith('/')) { throw "Unsafe ZIP entry: $name" }
        if ($entries.ContainsKey($name)) { throw "Duplicate ZIP entry: $name" }
        if ($name -cne 'manifest.json' -and $expectedFiles -cnotcontains $name) { throw "Unexpected ZIP entry outside the release allowlist: $name" }
        if ($entry.Length -gt 64MB) { throw "Unexpected release entry size: $name" }
        $entries[$name] = $entry
    }
    foreach ($name in ($expectedFiles + 'manifest.json')) {
        if (-not $entries.ContainsKey($name)) { throw "Missing release file: $name" }
    }
    $manifest = $utf8.GetString((Get-EntryBytes $entries['manifest.json'])) | ConvertFrom-Json
    if ($manifest.name -cne 'MoBuVideoPlayer' -or $manifest.host -cne 'MotionBuilder 2026' -or
        $manifest.platform -cne 'Windows x64' -or $manifest.version -notmatch '^\d+\.\d+\.\d+$') { throw 'The release manifest identity is invalid.' }
    if ($ExpectedVersion -and $manifest.version -cne $ExpectedVersion) { throw "The release version differs from $ExpectedVersion." }
    $declared = @{}
    foreach ($file in $manifest.files) {
        if ($expectedFiles -cnotcontains $file.path -or $declared.ContainsKey($file.path) -or $file.sha256 -cnotmatch '^[0-9a-f]{64}$') {
            throw 'The manifest file allowlist, uniqueness or SHA256 is invalid.'
        }
        $declared[$file.path] = $file.sha256
    }
    if ($declared.Count -ne $expectedFiles.Count) { throw 'The manifest does not declare the full release payload.' }
    $verified = @()
    $scriptPayloads = @{}
    foreach ($name in $expectedFiles) {
        $bytes = Get-EntryBytes $entries[$name]
        $hash = Get-BytesHash $bytes
        if ($hash -cne $declared[$name]) { throw "SHA256 mismatch: $name" }
        if ($name.EndsWith('.ps1')) {
            $tokens = $null
            $parseErrors = $null
            [Management.Automation.Language.Parser]::ParseInput($utf8.GetString($bytes), [ref]$tokens, [ref]$parseErrors) | Out-Null
            if ($parseErrors.Count) { throw "PowerShell syntax error in ${name}: $($parseErrors[0].Message)" }
            $scriptPayloads[$name] = [Convert]::ToBase64String($bytes)
        }
        if ($name -ceq 'plugins/MoBuVideoPlayer.dll') { Assert-NativePlugin $bytes }
        if ($CompareSourceRoot) {
            $sourcePath = Join-Path $CompareSourceRoot $name
            if ($name -cne 'plugins/MoBuVideoPlayer.dll' -or (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
                if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf) -or
                    (Get-BytesHash ([IO.File]::ReadAllBytes($sourcePath))) -cne $hash) {
                    throw "The packaged file differs from its source: $name"
                }
            }
        }
        $verified += [ordered]@{ path = $name; sha256 = $hash; bytes = $bytes.Length }
    }
    $parserVersion = Assert-WindowsPowerShellSyntax $scriptPayloads
    $archiveHash = Get-BytesHash ([IO.File]::ReadAllBytes($archiveFile.FullName))
    $checksumFile = $archiveFile.FullName + '.sha256'
    if ($RequireChecksum -and -not (Test-Path -LiteralPath $checksumFile -PathType Leaf)) { throw 'The release SHA256 sidecar is missing.' }
    if (Test-Path -LiteralPath $checksumFile -PathType Leaf) {
        $expectedChecksum = "$archiveHash  $($archiveFile.Name)"
        if ([IO.File]::ReadAllText($checksumFile, $utf8).TrimEnd("`r", "`n") -cne $expectedChecksum) { throw 'The release SHA256 sidecar differs from the archive.' }
    }
    $result = [pscustomobject][ordered]@{
        status = 'passed'
        name = $manifest.name
        version = $manifest.version
        archive = $archiveFile.Name
        sha256 = $archiveHash
        powershell_parser = $parserVersion
        files = $verified
    }
}
finally { $archive.Dispose() }

if ($ReportPath) {
    $destination = [IO.Path]::GetFullPath($ReportPath)
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) | Out-Null
    [IO.File]::WriteAllText($destination, ($result | ConvertTo-Json -Depth 6) + "`n", $utf8)
}
$result
