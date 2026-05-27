param(
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$repo = Resolve-Path (Join-Path $PSScriptRoot "..")
$build = Join-Path $repo "native\build\$Configuration"
$dist = Join-Path $repo "native\dist"
$zip = Join-Path $dist "symbion-native-webview2-release.zip"

if (!(Test-Path (Join-Path $build "symbion_native.exe"))) {
    throw "symbion_native.exe is missing. Run .\scripts\build-native.ps1 first."
}
if (!(Test-Path (Join-Path $build "symbion_backend.exe"))) {
    throw "symbion_backend.exe is missing. Run .\scripts\build-native.ps1 first."
}

New-Item -ItemType Directory -Path $dist -Force | Out-Null
if (Test-Path $zip) { Remove-Item -LiteralPath $zip -Force }

$stage = Join-Path $env:TEMP ("symbion-native-stage-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $stage -Force | Out-Null
Copy-Item (Join-Path $build "symbion_native.exe") $stage
Copy-Item (Join-Path $build "symbion_backend.exe") $stage
Copy-Item (Join-Path $build "WebView2Loader.dll") $stage -ErrorAction SilentlyContinue
Copy-Item (Join-Path $repo "native\README.md") $stage
Copy-Item (Join-Path $repo "config") (Join-Path $stage "config") -Recurse
Copy-Item (Join-Path $repo "native\web") (Join-Path $stage "web") -Recurse
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip -Force
Remove-Item -LiteralPath $stage -Recurse -Force
Get-Item $zip
