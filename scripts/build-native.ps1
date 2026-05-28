param(
    [string]$Configuration = "Release",
    [string]$WebView2SdkRoot = ""
)

$ErrorActionPreference = "Stop"
$repo = Resolve-Path (Join-Path $PSScriptRoot "..")
$cmake = "C:\Program Files\CMake\bin\cmake.exe"
if (!(Test-Path $cmake)) {
    $cmake = "cmake"
}

& (Join-Path $PSScriptRoot "extract-master-doc.ps1")

$configureArgs = @(
    "-S", (Join-Path $repo "native"),
    "-B", (Join-Path $repo "native\build"),
    "-G", "Visual Studio 17 2022",
    "-A", "x64"
)

if ($WebView2SdkRoot.Trim()) {
    $configureArgs += "-DSYMBION_WEBVIEW2_SDK_ROOT=$($WebView2SdkRoot.Trim())"
}

& $cmake @configureArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $cmake --build (Join-Path $repo "native\build") --config $Configuration
exit $LASTEXITCODE
