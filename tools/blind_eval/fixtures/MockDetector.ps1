param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$InputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$value = (Get-Content -LiteralPath $InputPath -Raw).Trim()
if ($value -eq 'FIXTURE_DETECTED') {
    Write-Output '{"detected":true}'
    return
}
if ($value -eq 'FIXTURE_CLEAR') {
    Write-Output '{"detected":false}'
    return
}
throw 'Unexpected fixture value.'
