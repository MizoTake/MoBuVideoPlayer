#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$PluginPath = '',
    [string]$OutputDirectory = '',
    [string]$SourceRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ReleaseInput.ps1')
foreach ($pathParameter in @('SourceRoot', 'PluginPath', 'OutputDirectory')) {
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
if (-not $PluginPath) { $PluginPath = Join-Path $SourceRoot 'build/package/plugins/MoBuVideoPlayer.dll' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $SourceRoot 'dist/release-input' }
$source = Get-ReleaseInputSource -SourceRoot $SourceRoot
$hash = Get-ReleaseInputPluginHash -PluginPath $PluginPath
$output = [IO.Path]::GetFullPath($OutputDirectory)
[void][IO.Directory]::CreateDirectory($output)
$name = "MoBuVideoPlayer-$($source.Version)-Mobu2026-win64.dll"
$destination = Join-Path $output $name
$metadataPath = $destination + '.json'
$metadata = [ordered]@{
    format_version = 1
    name = 'MoBuVideoPlayer'
    version = $source.Version
    host = 'MotionBuilder 2026'
    platform = 'Windows x64'
    dll_sha256 = $hash
    native_source_sha256 = $source.SHA256
    native_files = $source.Files
}
if ($PluginPath -ine $destination) { [IO.File]::Copy($PluginPath, $destination, $true) }
if ((Get-ReleaseInputPluginHash -PluginPath $destination) -cne $hash) { throw 'The copied release DLL does not match the selected input.' }
$metadataJson = ($metadata | ConvertTo-Json -Depth 5).Replace("`r`n", "`n").Replace("`r", "`n")
[IO.File]::WriteAllText($metadataPath, ($metadataJson + "`n"), [Text.UTF8Encoding]::new($false))
& (Join-Path $PSScriptRoot 'verify-release-input.ps1') -PluginPath $destination -MetadataPath $metadataPath -ExpectedVersion $source.Version -SourceRoot $SourceRoot | Out-Null
Write-Output "Release DLL: $destination"
Write-Output "Release metadata: $metadataPath"
Write-Output "SHA256: $hash"
