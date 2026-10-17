#requires -Version 5.1
[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Medium')]
param(
    [string]$PluginDirectory = '',
    [string]$ConfigPath = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'scripts/PluginPath.ps1')

if (-not $PluginDirectory) {
    $PluginDirectory = Join-Path $PSScriptRoot 'plugins'
    $developmentDirectory = Join-Path $PSScriptRoot 'build/package/plugins'
    if (-not (Test-Path -LiteralPath $PluginDirectory -PathType Container) -and
        (Test-Path -LiteralPath $developmentDirectory -PathType Container)) {
        $PluginDirectory = $developmentDirectory
    }
}

$registration = @{ Action = 'Add'; PluginDirectory = $PluginDirectory }
if ($ConfigPath) { $registration.ConfigPath = $ConfigPath }
foreach ($option in @('WhatIf', 'Confirm')) {
    if ($PSBoundParameters.ContainsKey($option)) { $registration[$option] = $PSBoundParameters[$option] }
}
$result = Invoke-MoBuPluginPathRegistration @registration
$result
if ($result.Changed) {
    Write-Host 'Registered. Start MotionBuilder normally, then open Open Reality > Tools > Native Video.'
    Write-Host 'Keep this plugin folder in place. Run Uninstall.ps1 before moving it.'
}
