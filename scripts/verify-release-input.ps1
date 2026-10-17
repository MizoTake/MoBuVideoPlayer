#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PluginPath,
    [Parameter(Mandatory = $true)][string]$MetadataPath,
    [Parameter(Mandatory = $true)][string]$ExpectedVersion,
    [string]$SourceRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$ReportPath = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ReleaseInput.ps1')
foreach ($pathParameter in @('PluginPath', 'MetadataPath', 'SourceRoot', 'ReportPath')) {
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
$file = Get-Item -LiteralPath $MetadataPath -ErrorAction Stop
if ($file.PSIsContainer -or $file.Length -gt 1MB -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
    throw 'Release input metadata must be a regular JSON file no larger than 1 MiB.'
}
$utf8 = [Text.UTF8Encoding]::new($false, $true)
$metadata = $utf8.GetString([IO.File]::ReadAllBytes($file.FullName)) | ConvertFrom-Json
Assert-ReleaseInputProperties $metadata @('format_version', 'name', 'version', 'host', 'platform',
    'dll_sha256', 'native_source_sha256', 'native_files')
if (($metadata.format_version -isnot [int] -and $metadata.format_version -isnot [long]) -or $metadata.format_version -ne 1 -or
    $metadata.name -cne 'MoBuVideoPlayer' -or $metadata.host -cne 'MotionBuilder 2026' -or
    $metadata.platform -cne 'Windows x64' -or $metadata.version -cne $ExpectedVersion -or
    $metadata.dll_sha256 -cnotmatch '^[a-f0-9]{64}$' -or $metadata.native_source_sha256 -cnotmatch '^[a-f0-9]{64}$') {
    throw 'Release input metadata identity, version or hash format is invalid.'
}
$source = Get-ReleaseInputSource -SourceRoot $SourceRoot
if ($source.Version -cne $ExpectedVersion) { throw 'The release input version differs from the checked-out project version.' }
$hash = Get-ReleaseInputPluginHash -PluginPath $PluginPath
if ($hash -cne $metadata.dll_sha256) { throw 'The release input DLL SHA256 does not match its metadata.' }
if ($source.SHA256 -cne $metadata.native_source_sha256 -or @($metadata.native_files).Count -ne $source.Files.Count) {
    throw 'The native source fingerprint differs from the checked-out tag.'
}
for ($index = 0; $index -lt $source.Files.Count; $index++) {
    $entry = $metadata.native_files[$index]
    Assert-ReleaseInputProperties $entry @('path', 'sha256')
    if ($entry.path -cne $source.Files[$index].path -or $entry.sha256 -cne $source.Files[$index].sha256) {
        throw 'The native source file list differs from the checked-out tag.'
    }
}
$report = [ordered]@{
    status = 'passed'
    version = $ExpectedVersion
    dll_filename = [IO.Path]::GetFileName($PluginPath)
    dll_sha256 = $hash
    native_source_sha256 = $source.SHA256
    native_files = $source.Files
}
if ($ReportPath) {
    $destination = [IO.Path]::GetFullPath($ReportPath)
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination))
    [IO.File]::WriteAllText($destination, (($report | ConvertTo-Json -Depth 5) + "`n"), $utf8)
}
[pscustomobject]$report
