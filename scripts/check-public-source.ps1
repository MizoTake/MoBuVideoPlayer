#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    [string]$ReportPath = ''
)

$ErrorActionPreference = 'Stop'
$rootPath = [IO.Path]::GetFullPath($Root)
$tracked = @(& git -C $rootPath -c core.quotepath=false ls-files --cached --others --exclude-standard)
if ($LASTEXITCODE) { throw 'Run this check inside a Git repository.' }
$files = @($tracked | Sort-Object -Unique)
if (-not $files.Count) { throw 'No public source files were found.' }
$allowedRoot = @('.editorconfig', '.gitattributes', '.gitignore', 'CMakeLists.txt',
    'Install.ps1', 'Uninstall.ps1', 'README.md', 'VALIDATION.md', 'CHANGELOG.md', 'RELEASING.md')
$allowedFolders = @{
    '.github' = @('.yml', '.yaml', '.md')
    'docs' = @('.md')
    'include' = @('.h', '.hpp')
    'src' = @('.h', '.hpp', '.cpp', '.cxx')
    'scripts' = @('.ps1', '.py')
    'tests' = @('.ps1', '.py', '.cpp')
}
$patterns = [ordered]@{
    PersonalWindowsPath = '(?i)[a-z]:[\\/]+(?:Users|Documents and Settings)[\\/]+[^\s<>"\x27]+'
    PersonalUnixPath = '/(?:home|Users)/[A-Za-z0-9_.-]+/'
    PersonalProjectPath = '(?i)[a-z]:[\\/]+(?:Project|Projects|UnityProject)[\\/]+'
    GitHubToken = '\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,})\b'
    AwsAccessKey = '\b(?:AKIA|ASIA)[A-Z0-9]{16}\b'
    PrivateKey = '-----BEGIN (?:[A-Z0-9]+ )?PRIVATE KEY-----'
    MachineName = '\b(?:DESKTOP|LAPTOP)-[A-Z0-9]{5,}\b'
}
$utf8 = [Text.UTF8Encoding]::new($false, $true)
$findings = [Collections.Generic.List[object]]::new()
foreach ($relative in $files) {
    $relative = $relative.Replace('\', '/')
    $parts = $relative.Split('/')
    $allowed = $allowedRoot -contains $relative
    if ($parts.Count -gt 1 -and $allowedFolders.ContainsKey($parts[0])) {
        $allowed = $allowedFolders[$parts[0]] -contains [IO.Path]::GetExtension($relative)
    }
    if (-not $allowed -or $parts -contains '..' -or [IO.Path]::IsPathRooted($relative)) {
        $findings.Add([pscustomobject]@{ File = $relative; Reason = 'UnexpectedPublicFile' })
        continue
    }
    $path = Join-Path $rootPath $relative
    if (-not [IO.File]::Exists($path)) {
        $findings.Add([pscustomobject]@{ File = $relative; Reason = 'MissingTrackedFile' })
        continue
    }
    $info = Get-Item -LiteralPath $path -Force
    if ($info.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        $findings.Add([pscustomobject]@{ File = $relative; Reason = 'LinkedFile' })
        continue
    }
    $bytes = [IO.File]::ReadAllBytes($path)
    try { $content = $utf8.GetString($bytes) }
    catch {
        $findings.Add([pscustomobject]@{ File = $relative; Reason = 'InvalidUtf8' })
        continue
    }
    if ($content.Contains([char]0)) { $findings.Add([pscustomobject]@{ File = $relative; Reason = 'BinaryContent' }) }
    if ($content.Contains("`r")) { $findings.Add([pscustomobject]@{ File = $relative; Reason = 'NonLfNewline' }) }
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 239 -and $bytes[1] -eq 187 -and $bytes[2] -eq 191) {
        $findings.Add([pscustomobject]@{ File = $relative; Reason = 'UnexpectedUtf8Bom' })
    }
    foreach ($pattern in $patterns.GetEnumerator()) {
        if ($content -match $pattern.Value) {
            $findings.Add([pscustomobject]@{ File = $relative; Reason = $pattern.Key })
        }
    }
}
$report = [ordered]@{ status = 'passed'; files = $files.Count; findings = @($findings.ToArray()) }
if ($findings.Count) { $report.status = 'failed' }
if ($ReportPath) {
    $output = [IO.Path]::GetFullPath($ReportPath)
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($output))
    [IO.File]::WriteAllText($output, (($report | ConvertTo-Json -Depth 4) + "`n"), $utf8)
}
if ($findings.Count) {
    $findings | Format-Table File, Reason | Out-String | Write-Output
    throw 'Public source inspection failed. Matched values are omitted from diagnostics.'
}
Write-Output "Public source inspection passed: $($files.Count) UTF-8 files."
