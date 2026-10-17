[CmdletBinding()]
param(
    [string]$MotionBuilderRoot = $env:MOBU_ROOT,
    [string]$BuildDirectory = '',
    [ValidateSet('Release', 'RelWithDebInfo', 'Debug')]
    [string]$Configuration = 'Release',
    [string]$Target = '',
    [switch]$CoreOnly,
    [switch]$RequireIntegrationTests,
    [switch]$SkipTests,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
foreach ($pathParameter in @('MotionBuilderRoot', 'BuildDirectory')) {
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
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $projectRoot 'build' }
if (-not $CoreOnly -and -not $MotionBuilderRoot) {
    $autodeskRoot = Join-Path $env:ProgramFiles 'Autodesk'
    $installation = Get-ChildItem -LiteralPath $autodeskRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -eq 'MotionBuilder 2026' } | Select-Object -First 1
    if ($installation) { $MotionBuilderRoot = $installation.FullName }
}
if (-not $CoreOnly -and (-not $MotionBuilderRoot -or -not (Test-Path -LiteralPath (Join-Path $MotionBuilderRoot 'OpenRealitySDK/PRODUCT_VERSION.json')))) {
    throw 'Specify -MotionBuilderRoot or MOBU_ROOT for an installed MotionBuilder 2026 SDK.'
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio 2022 C++ build tools were not found.' }
$vsRoot = & $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'Install the Visual Studio 2022 Desktop development with C++ component.' }
Import-Module (Join-Path $vsRoot 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$env:VSLANG = '1033'
$ninjaCommand = Get-Command ninja -ErrorAction SilentlyContinue
$ninjaPath = if ($ninjaCommand) { $ninjaCommand.Source } else {
    Join-Path $vsRoot 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
}
if (-not (Test-Path -LiteralPath $ninjaPath)) { throw 'Ninja was not found in the installed Visual Studio components.' }
$pluginEnabled = if ($CoreOnly) { 'OFF' } else { 'ON' }
$integrationRequired = if ($RequireIntegrationTests) { 'ON' } else { 'OFF' }
& cmake -S $projectRoot -B $BuildDirectory -G Ninja "-DCMAKE_BUILD_TYPE=$Configuration" "-DMOBU_ROOT=$MotionBuilderRoot" "-DCMAKE_MAKE_PROGRAM=$ninjaPath" "-DMOBU_BUILD_PLUGIN=$pluginEnabled" "-DMOBU_REQUIRE_INTEGRATION_TESTS=$integrationRequired" '-DBUILD_TESTING=ON'
if ($LASTEXITCODE) { throw 'CMake configuration failed.' }
$buildArguments = @('--build', $BuildDirectory, '--parallel')
if ($Clean) { $buildArguments += '--clean-first' }
if ($Target) { $buildArguments += @('--target', $Target) }
& cmake @buildArguments
if ($LASTEXITCODE) { throw 'Build failed.' }
if (-not $SkipTests) {
    & ctest --test-dir $BuildDirectory --output-on-failure
    if ($LASTEXITCODE) { throw 'Tests failed.' }
}
