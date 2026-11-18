param(
    [string]$SdkRoot = $env:SNAPBEAUTY_SDK_ROOT,
    [string]$OutDir = 'build/windows',
    [ValidateSet('clang-cl', 'cl')][string]$Compiler = 'clang-cl',
    [switch]$WithMakeup
)
$ErrorActionPreference = 'Stop'
if (-not $SdkRoot) { throw 'Pass -SdkRoot or set SNAPBEAUTY_SDK_ROOT.' }
$sdk = (Resolve-Path -LiteralPath $SdkRoot).Path
foreach ($file in 'include/snapbeauty/desktop.h', 'lib/snapbeauty.lib', 'bin/snapbeauty.dll', 'Assets') {
    if (-not (Test-Path -LiteralPath (Join-Path $sdk $file))) { throw "Invalid SDK root. Missing $file." }
}
$root = Split-Path $PSScriptRoot -Parent
$out = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutDir)
if (Test-Path -LiteralPath $out) { throw 'Choose a new -OutDir for each build.' }
New-Item -ItemType Directory -Path $out | Out-Null
$common = @('/nologo', '/W4', '/O2', '/MT', "/I$sdk/include")
$winLibs = @('user32.lib', 'gdi32.lib', 'ole32.lib', 'mfplat.lib', 'mf.lib', 'mfreadwrite.lib', 'mfuuid.lib')
Push-Location -LiteralPath $out
try {
    & $Compiler @common /TC /std:c11 "$root/examples/minimal.c" /Fesnapbeauty-minimal.exe /link "$sdk/lib/snapbeauty.lib"
    if ($LASTEXITCODE) { throw 'C example build failed.' }
    & $Compiler @common /TP /std:c++17 /EHsc "$PSScriptRoot/main.cpp" /Fesnapbeauty-camera.exe /link "$sdk/lib/snapbeauty.lib" @winLibs
    if ($LASTEXITCODE) { throw 'Camera example build failed.' }
    Copy-Item -LiteralPath "$sdk/bin/snapbeauty.dll" -Destination .
    if ($WithMakeup) {
        & python "$PSScriptRoot/prepare_assets.py" $sdk "$root/assets" "$out/Assets"
        if ($LASTEXITCODE) { throw 'Makeup asset preparation failed.' }
    } else {
        Copy-Item -LiteralPath "$sdk/Assets" -Destination . -Recurse
    }
} finally {
    Pop-Location
}
Write-Output "Built snapbeauty-camera.exe and snapbeauty-minimal.exe in $out"
