$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildRoot = Join-Path $projectRoot 'build'
$dependencyRoot = Join-Path $buildRoot 'deps'
$minHookRoot = Join-Path $dependencyRoot 'minhook-1.3.4'
$outputRoot = Join-Path $buildRoot 'Release'
$objectRoot = Join-Path $buildRoot 'obj'

New-Item -ItemType Directory -Force $dependencyRoot, $outputRoot, $objectRoot | Out-Null

if (-not (Test-Path (Join-Path $minHookRoot 'include\MinHook.h'))) {
    $archive = Join-Path $dependencyRoot 'minhook-v1.3.4.zip'
    Invoke-WebRequest 'https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.4.zip' -OutFile $archive
    $expectedHash = '172708123DAA0C98D20D3A980B16A50BE14AF243DC95DEE6F79C24193AD010E4'
    $actualHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
    if ($actualHash -ne $expectedHash) {
        Remove-Item -LiteralPath $archive -Force
        throw "MinHook archive checksum mismatch. Expected $expectedHash, got $actualHash."
    }
    Expand-Archive -LiteralPath $archive -DestinationPath $dependencyRoot -Force
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'Visual Studio Build Tools were not found.' }
$vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'The Visual C++ x64 build tools were not found.' }
$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'

$sources = @(
    (Join-Path $projectRoot 'src\plugin.cpp'),
    (Join-Path $minHookRoot 'src\buffer.c'),
    (Join-Path $minHookRoot 'src\hook.c'),
    (Join-Path $minHookRoot 'src\trampoline.c'),
    (Join-Path $minHookRoot 'src\hde\hde64.c')
)
$quotedSources = ($sources | ForEach-Object { '"' + $_ + '"' }) -join ' '
$includeArgs = '/I"' + (Join-Path $minHookRoot 'include') + '" /I"' + (Join-Path $minHookRoot 'src') + '"'
$outputDll = Join-Path $outputRoot 'popn_timing_overlay.dll'
$outputLib = Join-Path $outputRoot 'popn_timing_overlay.lib'
$versionResource = Join-Path $objectRoot 'version.res'
$resourceSource = Join-Path $projectRoot 'src\version.rc'
$compileResource = 'rc /nologo /fo"' + $versionResource + '" "' + $resourceSource + '"'
$compile = 'cl /nologo /LD /O2 /EHsc /std:c++17 /MT /utf-8 /Brepro /DWIN32 /D_WINDOWS /D_USRDLL ' +
    $includeArgs + ' ' + $quotedSources + ' /Fo"' + $objectRoot + '\\" /link /OUT:"' +
    $outputDll + '" /IMPLIB:"' + $outputLib + '" "' + $versionResource +
    '" /Brepro /DYNAMICBASE /NXCOMPAT user32.lib gdi32.lib'
$command = '"' + $vcvars + '" >nul && ' + $compileResource + ' && ' + $compile
& cmd.exe /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE." }

Write-Host "Built: $outputDll"
