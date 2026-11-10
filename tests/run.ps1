param(
  [string]$Compiler = "C:\mingw64\bin\g++.exe",
  [string]$IrLibrary = "$env:USERPROFILE\Documents\Arduino\libraries\IRremoteESP8266"
)

$ErrorActionPreference = "Stop"
$testOutput = Join-Path ([IO.Path]::GetTempPath()) "espIRemoteControl-tests"
New-Item -ItemType Directory -Force -Path $testOutput | Out-Null
$testExe = Join-Path $testOutput "ac_control_test.exe"
$protocolFlags = Get-Content (Join-Path $PSScriptRoot "..\ir_receive_demo\build_opt.h")
$testSources = "IRsend.cpp", "IRrecv.cpp", "IRtimer.cpp", "IRutils.cpp",
               "IRtext.cpp", "ir_Midea.cpp"
$testSources = $testSources | ForEach-Object { Join-Path $IrLibrary "src\$_" }
& $Compiler -std=c++11 -DUNIT_TEST @protocolFlags `
  "-I$IrLibrary\src" "-I$IrLibrary\test" -ffunction-sections -fdata-sections `
  (Join-Path $PSScriptRoot "ac_control_test.cpp") @testSources `
  "-Wl,--gc-sections" -o $testExe
if ($LASTEXITCODE -ne 0) { throw "Native test compilation failed" }
& $testExe
if ($LASTEXITCODE -ne 0) { throw "Native tests failed" }
