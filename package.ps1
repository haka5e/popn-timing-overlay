$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
& (Join-Path $projectRoot 'build.ps1')

$version = '1.0.0'
$distRoot = Join-Path $projectRoot 'dist'
$packageName = "popn_timing_overlay-v$version"
$stageRoot = Join-Path $distRoot $packageName
$releaseDll = Join-Path $projectRoot 'build\Release\popn_timing_overlay.dll'

if (Test-Path -LiteralPath $stageRoot) {
    Remove-Item -LiteralPath $stageRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $stageRoot | Out-Null

Copy-Item -LiteralPath $releaseDll -Destination $stageRoot
Copy-Item -LiteralPath (Join-Path $projectRoot 'popn_timing_overlay.ini') -Destination $stageRoot
Copy-Item -LiteralPath (Join-Path $projectRoot 'README.md') -Destination $stageRoot
Copy-Item -LiteralPath (Join-Path $projectRoot 'LICENSE') -Destination $stageRoot

$zipPath = Join-Path $distRoot "$packageName.zip"
if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}
Compress-Archive -Path (Join-Path $stageRoot '*') -DestinationPath $zipPath -CompressionLevel Optimal

$checksums = @(
    Get-FileHash -Algorithm SHA256 -LiteralPath $releaseDll
    Get-FileHash -Algorithm SHA256 -LiteralPath $zipPath
)
$checksumPath = Join-Path $distRoot 'SHA256SUMS.txt'
$checksumLines = $checksums | ForEach-Object {
    '{0}  {1}' -f $_.Hash.ToLowerInvariant(), (Split-Path -Leaf $_.Path)
}
Set-Content -LiteralPath $checksumPath -Value $checksumLines -Encoding ascii

Write-Host "Package: $zipPath"
Write-Host "Checksums: $checksumPath"

