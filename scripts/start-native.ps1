param(
    [string]$Url = ""
)

$ErrorActionPreference = "Stop"
$repo = Resolve-Path (Join-Path $PSScriptRoot "..")
$exe = Join-Path $repo "native\build\Release\symbion_native.exe"

if (!(Test-Path $exe)) {
    throw "Native shell is not built. Run: cmake --build native\build --config Release --target symbion_native"
}

$args = @()
if ($Url.Trim()) {
    $args += "--url"
    $args += $Url.Trim()
}

if ($args.Count -gt 0) {
    Start-Process -FilePath $exe -ArgumentList $args -WorkingDirectory $repo
} else {
    Start-Process -FilePath $exe -WorkingDirectory $repo
}
