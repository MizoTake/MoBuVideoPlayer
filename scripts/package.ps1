#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$MotionBuilderRoot = $env:MOBU_ROOT,
    [string]$PluginPath = '',
    [string]$BuildDirectory = '',
    [string]$OutputDirectory = '',
    [string]$ExpectedVersion = '',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$prebuiltMode = $PSBoundParameters.ContainsKey('PluginPath')
if ($prebuiltMode) {
    if ([string]::IsNullOrWhiteSpace($PluginPath) -or -not (Test-Path -LiteralPath $PluginPath -PathType Leaf)) {
        throw 'The supplied DLL does not exist. Specify -PluginPath with an existing native plugin.'
    }
    $PluginPath = [IO.Path]::GetFullPath($PluginPath)
}
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $projectRoot 'build' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $projectRoot 'dist' }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$outputKey = $OutputDirectory.TrimEnd('\', '/')
$projectDefinition = Get-Content -LiteralPath (Join-Path $projectRoot 'CMakeLists.txt') -Raw
if ($projectDefinition -notmatch 'project\(MoBuVideoPlayer VERSION ([0-9]+\.[0-9]+\.[0-9]+)') {
    throw 'The package version was not found in CMakeLists.txt.'
}
$packageVersion = $Matches[1]
if ($ExpectedVersion -and $ExpectedVersion -cne $packageVersion) { throw 'The requested release version differs from CMakeLists.txt.' }
if (-not $prebuiltMode -and -not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build.ps1') -MotionBuilderRoot $MotionBuilderRoot -BuildDirectory $BuildDirectory -Configuration Release
}
$packageName = "MoBuVideoPlayer-$packageVersion-Mobu2026-win64"
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
$workRoot = Join-Path $OutputDirectory ('.package-' + [guid]::NewGuid().ToString('N'))
$stageRoot = Join-Path $workRoot $packageName
[IO.Directory]::CreateDirectory($stageRoot) | Out-Null
$expectedFiles = @('Install.ps1', 'README.md', 'Uninstall.ps1', 'VALIDATION.md',
    'docs/DEPENDENCIES.md', 'plugins/MoBuVideoPlayer.dll', 'scripts/PluginPath.ps1', 'scripts/launch.ps1')
$utf8 = New-Object Text.UTF8Encoding($false)
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem

function Get-ReleaseFileHash {
    param([string]$Path)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try { return ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $algorithm.Dispose() }
}

try {
    $builtPlugin = Join-Path $BuildDirectory 'package/plugins/MoBuVideoPlayer.dll'
    if ($prebuiltMode) {
        $builtPlugin = $PluginPath
        $suppliedHash = Get-ReleaseFileHash $builtPlugin
        foreach ($name in $expectedFiles) {
            $sourcePath = Join-Path $projectRoot $name
            if ($name -ceq 'plugins/MoBuVideoPlayer.dll') { $sourcePath = $PluginPath }
            $destinationPath = Join-Path $stageRoot $name
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destinationPath)) | Out-Null
            Copy-Item -LiteralPath $sourcePath -Destination $destinationPath
        }
    } else {
        & cmake --install $BuildDirectory --config Release --prefix $stageRoot
        if ($LASTEXITCODE) { throw 'Package staging failed.' }
    }
    $files = @(Get-ChildItem -LiteralPath $stageRoot -File -Recurse)
    $manifest = [ordered]@{ name = 'MoBuVideoPlayer'; version = $packageVersion; host = 'MotionBuilder 2026'; platform = 'Windows x64'; files = @() }
    foreach ($file in $files) {
        $relative = $file.FullName.Substring($stageRoot.Length + 1).Replace('\', '/')
        if ($expectedFiles -cnotcontains $relative) { throw "Unexpected installed file outside the release allowlist: $relative" }
    }
    foreach ($name in $expectedFiles) {
        $path = Join-Path $stageRoot $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing release file: $name" }
        $manifest.files += [ordered]@{ path = $name; sha256 = Get-ReleaseFileHash $path }
    }
    $installedPlugin = Join-Path $stageRoot 'plugins/MoBuVideoPlayer.dll'
    if (-not (Test-Path -LiteralPath $builtPlugin -PathType Leaf) -or
        (Get-ReleaseFileHash $builtPlugin) -cne (Get-ReleaseFileHash $installedPlugin)) {
        if ($prebuiltMode) { throw 'The staged plugin differs from the supplied DLL.' }
        throw 'The staged plugin differs from the Release build output.'
    }
    if ($prebuiltMode -and (Get-ReleaseFileHash $installedPlugin) -cne $suppliedHash) { throw 'The supplied DLL changed during packaging.' }
    $manifestJson = ($manifest | ConvertTo-Json -Depth 5).Replace("`r`n", "`n").Replace("`r", "`n")
    [IO.File]::WriteAllText((Join-Path $stageRoot 'manifest.json'), $manifestJson + "`n", $utf8)
    $stagedArchive = Join-Path $workRoot ($packageName + '.zip')
    $archive = [IO.Compression.ZipFile]::Open($stagedArchive, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($name in (@($expectedFiles + 'manifest.json') | Sort-Object)) {
            $entry = $archive.CreateEntry($name, [IO.Compression.CompressionLevel]::Optimal)
            # Source timestamps must not make identical release payloads produce different archives.
            $entry.LastWriteTime = [DateTimeOffset]::new(1980, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
            $stream = $entry.Open()
            $source = [IO.File]::OpenRead((Join-Path $stageRoot $name))
            try { $source.CopyTo($stream) }
            finally { $source.Dispose(); $stream.Dispose() }
        }
    }
    finally { $archive.Dispose() }
    & (Join-Path $PSScriptRoot 'verify-package.ps1') -ArchivePath $stagedArchive -ExpectedVersion $packageVersion | Out-Null
    $archiveHash = Get-ReleaseFileHash $stagedArchive
    $stagedChecksum = $stagedArchive + '.sha256'
    [IO.File]::WriteAllText($stagedChecksum, "$archiveHash  $packageName.zip`n", $utf8)
    $packageRoot = Join-Path $OutputDirectory $packageName
    $archivePath = Join-Path $OutputDirectory ($packageName + '.zip')
    if (Test-Path -LiteralPath $packageRoot) {
        $resolvedPackage = [IO.Path]::GetFullPath($packageRoot)
        if ([IO.Path]::GetDirectoryName($resolvedPackage).TrimEnd('\', '/') -ine $outputKey -or [IO.Path]::GetFileName($resolvedPackage) -cne $packageName) {
            throw 'The existing stage is outside the intended output directory.'
        }
        Remove-Item -LiteralPath $resolvedPackage -Recurse -Force
    }
    Move-Item -LiteralPath $stageRoot -Destination $packageRoot
    Move-Item -LiteralPath $stagedArchive -Destination $archivePath -Force
    Move-Item -LiteralPath $stagedChecksum -Destination ($archivePath + '.sha256') -Force
    Write-Output "Package: $archivePath"
    Write-Output "SHA256: $archiveHash"
}
finally {
    $resolvedWork = [IO.Path]::GetFullPath($workRoot)
    if ([IO.Path]::GetDirectoryName($resolvedWork).TrimEnd('\', '/') -ieq $outputKey -and [IO.Path]::GetFileName($resolvedWork).StartsWith('.package-')) {
        Remove-Item -LiteralPath $resolvedWork -Recurse -Force -ErrorAction SilentlyContinue
    }
}
