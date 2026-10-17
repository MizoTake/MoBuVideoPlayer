#requires -Version 7.0
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$check = Join-Path (Split-Path -Parent $PSScriptRoot) 'scripts/check-public-source.ps1'
$fixtureRoot = Join-Path ([IO.Path]::GetTempPath()) ('MoBuPublicSource-' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($fixtureRoot)
$utf8 = [Text.UTF8Encoding]::new($false)
$cases = @(
    @{ Name = 'Public UTF8 source passes'; Path = 'README.md'; Text = "Example`n"; Pass = $true }
    @{ Name = 'Personal path is rejected'; Path = 'README.md'; Text = ('C:' + '\Users\Example\private.txt'); Pass = $false }
    @{ Name = 'SDK file is rejected'; Path = 'OpenRealitySDK/fbsdk.h'; Text = 'header'; Pass = $false }
    @{ Name = 'Release binary is rejected from source'; Path = 'plugins/example.dll'; Text = 'MZ'; Pass = $false }
    @{ Name = 'Private key is rejected'; Path = 'README.md'; Text = ('-----BEGIN ' + 'PRIVATE KEY-----'); Pass = $false }
    @{ Name = 'Token is rejected without exposing it'; Path = 'README.md'; Text = ('ghp_' + ('x' * 36)); Pass = $false }
    @{ Name = 'CRLF is rejected'; Path = 'README.md'; Text = "Example`r`n"; Pass = $false }
)
try {
    foreach ($case in $cases) {
        $fixture = Join-Path $fixtureRoot ([guid]::NewGuid().ToString('N'))
        [void][IO.Directory]::CreateDirectory($fixture)
        & git -C $fixture init --quiet
        if ($LASTEXITCODE) { throw 'Fixture Git initialization failed.' }
        $file = Join-Path $fixture $case.Path
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($file))
        [IO.File]::WriteAllText($file, $case.Text, $utf8)
        $passed = $true
        $diagnostics = [Collections.Generic.List[string]]::new()
        try { & $check -Root $fixture | ForEach-Object { $diagnostics.Add([string]$_) } }
        catch { $passed = $false }
        if ($passed -ne $case.Pass) { throw ('Unexpected inspection result: ' + $case.Name) }
        if (-not $case.Pass -and ($diagnostics -join "`n").Contains($case.Text)) { throw 'Inspection exposed the matched secret value.' }
        Write-Output ('PASS ' + $case.Name)
    }
} finally {
    $resolved = [IO.Path]::GetFullPath($fixtureRoot)
    $temporary = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($temporary, [StringComparison]::OrdinalIgnoreCase) -or
        -not [IO.Path]::GetFileName($resolved).StartsWith('MoBuPublicSource-')) {
        throw 'Unexpected fixture cleanup path.'
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
