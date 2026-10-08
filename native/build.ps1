param([switch]$Test)
$ErrorActionPreference = 'Stop'
$taskVsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $taskVsWhere)) { throw 'Install Visual Studio 2022 Build Tools with Desktop development with C++ and a Windows 10/11 SDK.' }
$taskVisualStudio = & $taskVsWhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $taskVisualStudio) { throw 'Visual Studio 2022 C++ tools were not found.' }
$taskMsBuild = Join-Path $taskVisualStudio 'MSBuild\Current\Bin\MSBuild.exe'
& $taskMsBuild (Join-Path $PSScriptRoot 'AppleMusicPresenceNative.vcxproj') /m:2 /nologo /v:minimal /p:Configuration=Release /p:Platform=x64
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($Test) {
    & $taskMsBuild (Join-Path $PSScriptRoot 'NativeTests.vcxproj') /m:2 /nologo /v:minimal /p:Configuration=Release /p:Platform=x64
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & (Join-Path $PSScriptRoot '..\build\native\Release\NativeTests.exe')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
