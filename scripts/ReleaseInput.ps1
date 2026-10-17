#requires -Version 7.0

function Get-ReleaseInputHash {
    param([byte[]]$Bytes)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($Bytes)).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Get-ReleaseInputSource {
    param([Parameter(Mandatory = $true)][string]$SourceRoot)
    $pathProvider = $null
    $pathDrive = $null
    $resolvedPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath(
        $SourceRoot, [ref]$pathProvider, [ref]$pathDrive)
    if ($pathProvider.Name -ne 'FileSystem') { throw 'SourceRoot must use the FileSystem provider.' }
    if (-not [IO.Path]::IsPathRooted($resolvedPath)) {
        $resolvedPath = Join-Path $ExecutionContext.SessionState.Path.CurrentFileSystemLocation.Path $resolvedPath
    } elseif ($resolvedPath -match '^[\\/](?![\\/])') {
        $resolvedPath = Join-Path $ExecutionContext.SessionState.Path.CurrentFileSystemLocation.Drive.Root $resolvedPath.TrimStart('\', '/')
    }
    $resolvedPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath(
        $resolvedPath, [ref]$pathProvider, [ref]$pathDrive)
    if ($pathProvider.Name -ne 'FileSystem') { throw 'SourceRoot must use the FileSystem provider.' }
    $rootPath = [IO.Path]::GetFullPath($resolvedPath)
    $definition = [IO.File]::ReadAllText((Join-Path $rootPath 'CMakeLists.txt'))
    if ($definition -notmatch 'project\(MoBuVideoPlayer VERSION ([0-9]+\.[0-9]+\.[0-9]+)') {
        throw 'The native source project version is missing.'
    }
    $version = $Matches[1]
    $paths = [Collections.Generic.List[string]]::new()
    $paths.Add('CMakeLists.txt')
    foreach ($directory in @('src', 'include')) {
        $folder = Join-Path $rootPath $directory
        if (-not [IO.Directory]::Exists($folder)) { throw "Missing native source directory: $directory" }
        foreach ($file in Get-ChildItem -LiteralPath $folder -File -Recurse) {
            if ($file.Extension -notin @('.cpp', '.cxx', '.h', '.hpp')) { continue }
            if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Linked native source files are not accepted.' }
            $paths.Add([IO.Path]::GetRelativePath($rootPath, $file.FullName).Replace('\', '/'))
        }
    }
    if ($paths.Count -lt 3) { throw 'Native source files are missing.' }
    $orderedPaths = $paths.ToArray()
    [Array]::Sort($orderedPaths, [StringComparer]::Ordinal)
    $files = @()
    $canonical = [Text.StringBuilder]::new()
    foreach ($relative in $orderedPaths) {
        $hash = Get-ReleaseInputHash ([IO.File]::ReadAllBytes((Join-Path $rootPath $relative)))
        $files += [ordered]@{ path = $relative; sha256 = $hash }
        [void]$canonical.Append($hash).Append('  ').Append($relative).Append("`n")
    }
    return [pscustomobject]@{
        Version = $version
        Files = $files
        SHA256 = Get-ReleaseInputHash ([Text.Encoding]::UTF8.GetBytes($canonical.ToString()))
    }
}

function Get-ReleaseInputPluginHash {
    param([Parameter(Mandatory = $true)][string]$PluginPath)
    $file = Get-Item -LiteralPath $PluginPath -ErrorAction Stop
    if ($file.PSIsContainer -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $file.Length -gt 64MB) {
        throw 'The release input must be a regular DLL no larger than 64 MiB.'
    }
    $bytes = [IO.File]::ReadAllBytes($file.FullName)
    if ($bytes.Length -lt 128 -or $bytes[0] -ne 77 -or $bytes[1] -ne 90) { throw 'The release DLL has no valid PE header.' }
    $offset = [BitConverter]::ToInt32($bytes, 60)
    if ($offset -lt 64 -or $offset -gt $bytes.Length - 26 -or
        [BitConverter]::ToUInt32($bytes, $offset) -ne 0x4550 -or
        [BitConverter]::ToUInt16($bytes, $offset + 4) -ne 0x8664 -or
        [BitConverter]::ToUInt16($bytes, $offset + 24) -ne 0x20b -or
        -not ([BitConverter]::ToUInt16($bytes, $offset + 22) -band 0x2000)) {
        throw 'The release input must be an AMD64 PE32+ DLL.'
    }
    return Get-ReleaseInputHash $bytes
}

function Assert-ReleaseInputProperties {
    param($Object, [string[]]$Names)
    if ($null -eq $Object) { throw 'Release input metadata is missing.' }
    $actual = @($Object.PSObject.Properties.Name | Sort-Object)
    $expected = @($Names | Sort-Object)
    if (($actual -join "`n") -cne ($expected -join "`n")) { throw 'Release input metadata contains missing or unexpected fields.' }
}
