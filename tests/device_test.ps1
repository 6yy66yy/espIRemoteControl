param([Parameter(Mandatory)][string]$BaseUrl)
$ErrorActionPreference = "Stop"
$BaseUrl = $BaseUrl.TrimEnd('/')

function Invoke-Device([string]$Path, [string]$Method = "GET", $Value = $null) {
  $parameters = @{ Uri = "$BaseUrl$Path"; Method = $Method; TimeoutSec = 12 }
  if ($null -ne $Value) {
    $parameters.ContentType = "application/json; charset=utf-8"
    $parameters.Body = [Text.Encoding]::UTF8.GetBytes(($Value | ConvertTo-Json -Depth 10 -Compress))
  }
  Invoke-RestMethod @parameters
}

function Assert-Device([bool]$Condition, [string]$Message) {
  if (!$Condition) { throw $Message }
}

function Assert-Rejected($Value, [int]$ExpectedCode) {
  $response = Invoke-WebRequest -Uri "$BaseUrl/api/rules" -Method Put `
    -ContentType 'application/json' -Body ([Text.Encoding]::UTF8.GetBytes(
      ($Value | ConvertTo-Json -Depth 10 -Compress))) -SkipHttpErrorCheck -TimeoutSec 12
  Assert-Device ($response.StatusCode -eq $ExpectedCode) "Unexpected error status $($response.StatusCode)"
  $after = Invoke-Device '/api/rules'
  Assert-Device (($after.rules | ConvertTo-Json -Depth 10 -Compress) -eq $script:expectedRules) `
    'Invalid input modified the active rules'
}

$original = Invoke-Device '/api/rules'
$initialStatus = Invoke-Device '/api/status'
$script:expectedRules = $original.rules | ConvertTo-Json -Depth 10 -Compress
$savedOriginal = @{ version = 1; rules = @($original.rules) }
$changedRules = $false
try {
  # Keep physical IR effects limited to OFF while checking all config paths.
  Invoke-Device '/api/control' 'POST' @{ command = 'OFF' } | Out-Null
  $invalid = @{ version = 1; rules = @(@{
      id = 33; enabled = $true; direction = 'falling'; thresholdC = 22.05;
      windowC = 2; action = 'heat'; targetC = 24
  }) }
  Assert-Rejected $invalid 400
  Assert-Rejected @{ version = 1; rules = @(); padding = ('x' * 4200) } 413
  $badControl = Invoke-WebRequest -Uri "$BaseUrl/api/control" -Method Post `
    -ContentType 'application/json' -Body '{"command":"on#3#bad#0"}' `
    -SkipHttpErrorCheck -TimeoutSec 12
  Assert-Device ($badControl.StatusCode -eq 400) 'Invalid control command accepted'
  $tenRules = @(1..10 | ForEach-Object { @{
      id = 100 + $_; enabled = $false; direction = 'falling'; thresholdC = 22;
      windowC = 2; action = 'heat'; targetC = 24
  } })
  $candidate = @{ version = 1; rules = $tenRules }
  $changedRules = $true
  $saved = Invoke-Device '/api/rules' 'PUT' $candidate
  Assert-Device ($saved.rules.Count -eq 10) 'Ten rules did not save'
  $script:expectedRules = $saved.rules | ConvertTo-Json -Depth 10 -Compress
  $candidate.rules = @($tenRules) + @(@{
      id = 999; enabled = $false; direction = 'rising'; thresholdC = 28;
      windowC = 2; action = 'cool'; targetC = 26
  })
  Assert-Rejected $candidate 400
  $duplicate = $saved | ConvertTo-Json -Depth 10 | ConvertFrom-Json
  $duplicate.rules[1].id = $duplicate.rules[0].id
  Assert-Rejected $duplicate 400
  $offRule = @{ version = 1; rules = @(@{
      id = 333; enabled = $true; direction = 'falling'; thresholdC = 50;
      windowC = 2; action = 'off'; targetC = 24
  }) }
  Invoke-Device '/api/rules' 'PUT' $offRule | Out-Null
  Invoke-Device '/api/control' 'POST' @{ command = 'AUTO' } | Out-Null
  Start-Sleep -Seconds 4
  $triggered = Invoke-Device '/api/status'
  Assert-Device ($triggered.automatic -and $triggered.rules[0].waiting -and
    $triggered.lastRule.id -eq 333 -and !$triggered.lastRule.sent) `
    'Rule did not consume an identical OFF target while remaining automatic'
  Start-Sleep -Seconds 3
  $stillWaiting = Invoke-Device '/api/status'
  Assert-Device ($stillWaiting.lastRule.ageMs -gt $triggered.lastRule.ageMs) `
    'Rule retriggered without resetting'
  Invoke-Device '/api/control' 'POST' @{ command = 'OFF' } | Out-Null
  Invoke-Device '/api/rules' 'PUT' @{ version = 1; rules = $tenRules } | Out-Null
  $stats = @()
  for ($i = 0; $i -lt 16; ++$i) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $status = Invoke-Device '/api/status'
    $watch.Stop()
    Assert-Device ($status.sensor.valid) 'Sensor became invalid during polling'
    Assert-Device ($status.ac.power -eq 'off' -and !$status.automatic) 'Manual state lost'
    $stats += [PSCustomObject]@{ heap = $status.freeHeap; latencyMs = $watch.ElapsedMilliseconds }
    Start-Sleep -Seconds 2
  }
  Write-Output "PASS: API validation, 4 KiB limit, ten-rule save, duplicate rejection, polling"
  Write-Output "Heap range: $(($stats.heap | Measure-Object -Minimum).Minimum)..$(($stats.heap | Measure-Object -Maximum).Maximum) bytes"
  Write-Output "Max HTTP response: $(($stats.latencyMs | Measure-Object -Maximum).Maximum) ms"
} finally {
  if ($changedRules) { Invoke-Device '/api/rules' 'PUT' $savedOriginal | Out-Null }
  if ($initialStatus.automatic) {
    Invoke-Device '/api/control' 'POST' @{ command = 'AUTO' } | Out-Null
  }
}
