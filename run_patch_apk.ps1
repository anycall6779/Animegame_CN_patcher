param(
  [string]$Apk = "Original.StripResource_13.2.8_341_kZm.apk",
  [string]$Out = "out\animegame_localify_unsigned.apk",
  [switch]$BuildPayload,
  [string]$Gjs = "",
  [string]$NdkRoot = "",
  [switch]$Sign,
  [string]$SignedOut = "out\animegame_localify_signed.apk",
  [string]$Apksigner = "auto-singer-main\apksigner.jar"
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$script = Join-Path $root "patch_original_apk.py"

$argsList = @($script, $Apk, "--out", $Out)

if ($BuildPayload) {
  $argsList += "--build-payload"
}
if ($Gjs) {
  $argsList += @("--gjs", $Gjs)
}
if ($NdkRoot) {
  $argsList += @("--ndk-root", $NdkRoot)
}

if ($Sign) {
  $argsList += @("--sign", "--signed-out", $SignedOut, "--apksigner", $Apksigner)
}

python @argsList
