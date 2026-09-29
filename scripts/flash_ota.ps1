<#
.SYNOPSIS
  Flash a firmware image over HTTP to a TickrMeter (Windows PowerShell 5.1+ / PowerShell 7).

.DESCRIPTION
  The Windows counterpart of scripts/flash_ota.sh - same behaviour, no external tools:
    * TickrDisplay firmware     - POST /update  (multipart field "update")
    * stock TickrMeter firmware - WiFiManager portal, POST /u (multipart field "update"),
                                  reachable at http://192.168.4.1 while the "TickrMeter" AP is up.
  Without -Stock / -Fork the script probes GET /api/status (a cheap endpoint only TickrDisplay
  has) to detect TickrDisplay; if that fails it assumes the stock WiFiManager portal.
  If the TickrDisplay device has an API token (/system -> Security), set the environment variable
  TICKR_API_TOKEN; it is sent as HTTP Basic auth (any user, password = token).

.EXAMPLE
  $env:TICKR_API_TOKEN = "<token>"
  .\scripts\flash_ota.ps1 <device-ip> .\tickrdisplay-<version>.bin

.EXAMPLE
  .\scripts\flash_ota.ps1 192.168.4.1 .\tickrdisplay-<version>.bin -Stock
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)] [string] $DeviceIp,
    [Parameter(Mandatory = $true, Position = 1)] [string] $Firmware,
    [switch] $Stock,
    [switch] $Fork
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Net.Http

function Fail([string] $message, [int] $code) {
    [Console]::Error.WriteLine($message)
    exit $code
}

if ($Stock -and $Fork) { Fail 'Use either -Stock or -Fork, not both' 1 }
if (-not (Test-Path -LiteralPath $Firmware -PathType Leaf)) { Fail "File not found: $Firmware" 1 }
$Firmware = (Resolve-Path -LiteralPath $Firmware).Path

$base = "http://$DeviceIp"
$size = (Get-Item -LiteralPath $Firmware).Length

# First byte must be the ESP32 image magic 0xE9.
$fs = [System.IO.File]::OpenRead($Firmware)
try { $magic = $fs.ReadByte() } finally { $fs.Dispose() }
if ($magic -ne 0xE9) {
    Fail "!! $Firmware does not start with 0xE9 - not an ESP32 application image (did you pick firmware.bin, not a merged/bootloader image?)" 1
}
# Standard Arduino OTA slot on the stock 4 MB table is 0x140000 bytes.
$otaSlotStock = 1310720
if ($size -gt $otaSlotStock) {
    Write-Warning "Image is $size bytes, larger than the stock OTA slot ($otaSlotStock). It will only fit if the device uses a bigger partition table."
}

# Optional API token for TickrDisplay endpoints (ignored by the stock portal).
$authHeader = $null
if ($env:TICKR_API_TOKEN) {
    $authHeader = 'Basic ' + [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes(":$($env:TICKR_API_TOKEN)"))
}

function Invoke-Probe([string] $url) {
    # GET with a 5 s timeout; $null when anything fails (connection, 401, 404 ...).
    $client = New-Object System.Net.Http.HttpClient
    $client.Timeout = [TimeSpan]::FromSeconds(5)
    if ($authHeader) { $client.DefaultRequestHeaders.TryAddWithoutValidation('Authorization', $authHeader) | Out-Null }
    try {
        $r = $client.GetAsync($url).GetAwaiter().GetResult()
        if (-not $r.IsSuccessStatusCode) { return $null }
        return $r.Content.ReadAsStringAsync().GetAwaiter().GetResult()
    } catch { return $null } finally { $client.Dispose() }
}

if ($Stock) { $target = 'stock' }
elseif ($Fork) { $target = 'fork' }
else {
    $status = Invoke-Probe "$base/api/status"
    if ($status) {
        $target = 'fork'
        try {
            $j = $status | ConvertFrom-Json
            Write-Host "Detected TickrDisplay (uptime $($j.uptime_s) s, free heap $($j.heap_free) bytes)"
        } catch { Write-Host 'Detected TickrDisplay firmware' }
    } else {
        $target = 'stock'
        Write-Host 'No /api/status - assuming stock WiFiManager portal'
        Write-Host '   (a TickrDisplay with an API token answers 401 here: set $env:TICKR_API_TOKEN = "<token>")'
    }
}

if ($target -eq 'fork') {
    $url = "$base/update"
} else {
    $url = "$base/u"
    # Sanity check that the stock portal answers (GET /update shows the upload form).
    if (-not (Invoke-Probe "$base/update")) {
        Fail "!! $base/update did not answer. For the stock firmware: power the device from USB, make your home WiFi unavailable, toggle the rear switch off/on, join the open 'TickrMeter' AP and use 192.168.4.1." 2
    }
}

Write-Host "Uploading $Firmware ($size bytes) to $url ..."
Write-Host 'Do NOT remove power until the device reboots.'

$client = New-Object System.Net.Http.HttpClient
$client.Timeout = [TimeSpan]::FromSeconds(600)
if ($target -eq 'fork' -and $authHeader) {
    $client.DefaultRequestHeaders.TryAddWithoutValidation('Authorization', $authHeader) | Out-Null
}
$form = New-Object System.Net.Http.MultipartFormDataContent
$stream = [System.IO.File]::OpenRead($Firmware)
$file = New-Object System.Net.Http.StreamContent($stream)
$file.Headers.ContentType = [System.Net.Http.Headers.MediaTypeHeaderValue]::Parse('application/octet-stream')
$form.Add($file, 'update', [System.IO.Path]::GetFileName($Firmware))

try {
    $resp = $client.PostAsync($url, $form).GetAwaiter().GetResult()
    $code = [int] $resp.StatusCode
    $body = $resp.Content.ReadAsStringAsync().GetAwaiter().GetResult()
} catch {
    Fail "!! upload failed: $($_.Exception.GetBaseException().Message)" 3
} finally {
    $form.Dispose()
    $stream.Dispose()
    $client.Dispose()
}

Write-Host "HTTP $code"
if ($target -eq 'fork') {
    Write-Host $body
    if ($code -ne 200) { exit 4 }
    Write-Host "Device is rebooting into the new image. Reload http://$DeviceIp/system#firmware in ~15 s."
} else {
    # WiFiManager answers with an HTML page containing either "Update successful" or "Update failed".
    if ($body -match 'Update successful') {
        Write-Host 'Stock portal reports: Update successful. Device is rebooting into the new image.'
    } elseif ($body -match 'Update failed') {
        Fail '!! Stock portal reports: Update failed. Reboot and try again (make sure the AP did not time out).' 4
    } else {
        Write-Host "Unexpected response from stock portal (HTTP $code):"
        $text = ($body -replace '<[^>]*>', ' ') -replace '\s+', ' '
        if ($text.Length -gt 600) { $text = $text.Substring(0, 600) }
        Write-Host $text
        if ($code -ne 200) { exit 4 }
    }
}
