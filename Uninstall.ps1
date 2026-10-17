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

$registration = @{ Action = 'Remove'; PluginDirectory = $PluginDirectory }
if ($ConfigPath) { $registration.ConfigPath = $ConfigPath }
foreach ($option in @('WhatIf', 'Confirm')) {
    if ($PSBoundParameters.ContainsKey($option)) { $registration[$option] = $PSBoundParameters[$option] }
}
$result = Invoke-MoBuPluginPathRegistration @registration
$result
if ($result.Changed) {
    Write-Host 'Unregistered. This plugin folder is no longer in the additional plug-in search paths.'
    Write-Host 'The extracted files and generated media remain available.'
}
