# Builds the Slice it All Windows installer with Inno Setup.
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$iss = Join-Path $PSScriptRoot "SliceItAll.iss"

$isccCandidates = @(
    "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
)
$iscc = $isccCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) {
    throw "Inno Setup 6 is not installed. Install JRSoftware.InnoSetup, then run this script again."
}

$crtCandidates = @(
    "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Redist\MSVC\14.51.36231\x64\Microsoft.VC145.CRT",
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC"
)
$crt = $crtCandidates | Where-Object { Test-Path (Join-Path $_ "vcruntime140.dll") } | Select-Object -First 1
if (-not $crt) {
    $found = Get-ChildItem "C:\Program Files\Microsoft Visual Studio" -Recurse -Filter "vcruntime140.dll" -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "\\x64\\Microsoft\.VC\d+\.CRT\\" -and $_.FullName -notmatch "debug" } |
        Select-Object -First 1
    if ($found) { $crt = $found.DirectoryName }
}
if (-not $crt) {
    throw "Could not find the Visual C++ CRT DLLs to ship with the installer."
}

$buildDir = Join-Path $root "build\src\RelWithDebInfo"
foreach ($name in @("SliceItAll.exe", "SliceItAll.dll", "OCCTWrapper.dll", "libgmp-10.dll", "libmpfr-4.dll", "WebView2Loader.dll")) {
    if (-not (Test-Path (Join-Path $buildDir $name))) {
        throw "Missing $name in $buildDir. Build RelWithDebInfo first."
    }
}

New-Item -ItemType Directory -Force -Path (Join-Path $root "dist") | Out-Null
& $iscc "/DCrtDir=$crt" $iss
if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE" }
Write-Host "Installer written to $root\dist\SliceItAll-0.9.3-win64-setup.exe"
