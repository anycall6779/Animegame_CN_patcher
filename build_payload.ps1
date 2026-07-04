param(
  [string]$SdkRoot = $env:ANDROID_SDK_ROOT,
  [string]$NdkRoot = $env:ANDROID_NDK_HOME,
  [string]$Gjs = "translations\libfrida-gadget.script_KR.js",
  [string]$OutDir = "payload"
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = $root

if (-not $SdkRoot) {
  $SdkRoot = Join-Path $env:LOCALAPPDATA "Android\Sdk"
}
if (-not (Test-Path -LiteralPath $SdkRoot)) {
  throw "Android SDK was not found. Pass -SdkRoot or set ANDROID_SDK_ROOT."
}

$out = Join-Path $repo $OutDir
$build = Join-Path $root "build\payload"
$classes = Join-Path $build "classes"
$stubClasses = Join-Path $build "stub_classes"
$dexWork = Join-Path $build "dex"
New-Item -ItemType Directory -Force $out, $build, $classes, $stubClasses, $dexWork | Out-Null

$androidJar = Get-ChildItem -Path (Join-Path $SdkRoot "platforms") -Recurse -File -Filter android.jar |
  Sort-Object FullName -Descending |
  Select-Object -First 1
if (-not $androidJar) {
  throw "android.jar was not found under $SdkRoot\platforms."
}

$d8 = Get-ChildItem -Path (Join-Path $SdkRoot "build-tools") -Recurse -File -Filter d8.bat |
  Sort-Object FullName -Descending |
  Select-Object -First 1
if (-not $d8) {
  throw "d8.bat was not found under $SdkRoot\build-tools."
}

Remove-Item -Recurse -Force $classes, $stubClasses, $dexWork -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $classes, $stubClasses, $dexWork | Out-Null

$stubSrc = Join-Path $build "ComboApplication.java"
$stubText = @"
package com.combosdk.openapi;

public class ComboApplication extends android.app.Application {
    public ComboApplication() {
    }
}
"@
[System.IO.File]::WriteAllText($stubSrc, $stubText, [System.Text.UTF8Encoding]::new($false))

& javac -encoding UTF-8 -source 8 -target 8 -cp $androidJar.FullName -d $stubClasses $stubSrc
if ($LASTEXITCODE -ne 0) { throw "javac failed while compiling ComboApplication stub." }

$stubJar = Join-Path $build "combo-stub.jar"
if (Test-Path -LiteralPath $stubJar) { Remove-Item -LiteralPath $stubJar -Force }
& jar cf $stubJar -C $stubClasses .
if ($LASTEXITCODE -ne 0) { throw "jar failed while creating ComboApplication stub jar." }

$loaderSrc = Join-Path $root "loader_java\com\combosdk\openapi\ComboAppProxy.java"
& javac -encoding UTF-8 -source 8 -target 8 -cp "$($androidJar.FullName);$stubJar" -d $classes $loaderSrc
if ($LASTEXITCODE -ne 0) { throw "javac failed while compiling ComboAppProxy." }

& $d8.FullName --min-api 26 --output $dexWork (Join-Path $classes "com\combosdk\openapi\ComboAppProxy.class")
if ($LASTEXITCODE -ne 0) { throw "d8 failed while generating classes.dex." }

Copy-Item -LiteralPath (Join-Path $dexWork "classes.dex") -Destination (Join-Path $out "classes5.dex") -Force
Write-Host "Output dex: $(Join-Path $out 'classes5.dex')"

$so = Join-Path $out "libanimegame_native_localify.so"
if (Test-Path -LiteralPath $so) {
  Remove-Item -LiteralPath $so -Force
}

$gjsPath = Join-Path $repo $Gjs
if (-not (Test-Path -LiteralPath $gjsPath)) {
  throw "g.js not found: $gjsPath"
}

$blobCpp = Join-Path $build "animegame_translation_blob.cpp"
$blobBin = Join-Path $build "animegame_translation_blob.bin"
python (Join-Path $root "build_tools\make_translation_blob_cpp.py") $gjsPath $blobCpp --blob-out $blobBin
if ($LASTEXITCODE -ne 0) { throw "translation blob generation failed." }

if (-not $NdkRoot) {
  $defaultNdk = Join-Path $SdkRoot "ndk"
  if (Test-Path -LiteralPath $defaultNdk) {
    $NdkRoot = (Get-ChildItem -Directory $defaultNdk | Sort-Object Name -Descending | Select-Object -First 1).FullName
  }
}

if (-not $NdkRoot -or -not (Test-Path -LiteralPath $NdkRoot)) {
  throw "Android NDK was not found. classes5.dex was generated from SDK/d8, but libanimegame_native_localify.so was not built. Install NDK or pass -NdkRoot."
}

$clang = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android26-clang++.cmd"
if (-not (Test-Path -LiteralPath $clang)) {
  throw "Missing compiler: $clang"
}

$src = Join-Path $root "native\animegame_native_localify.cpp"
& $clang -std=c++17 -fPIC -shared -O2 `
  $src $blobCpp `
  -llog -ldl -o $so
if ($LASTEXITCODE -ne 0) { throw "native compile failed." }

Write-Host "Output so : $so"
