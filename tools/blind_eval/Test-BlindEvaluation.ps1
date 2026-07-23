[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-Equal {
    param(
        [Parameter(Mandatory = $true)]$Actual,
        [Parameter(Mandatory = $true)]$Expected,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if ($Actual -ne $Expected) {
        throw "$Name mismatch: expected '$Expected', got '$Actual'."
    }
}

function Assert-Null {
    param(
        [AllowNull()]$Actual,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if ($null -ne $Actual) {
        throw "$Name mismatch: expected null, got '$Actual'."
    }
}

function Remove-TestWork {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    $resolvedRoot = [IO.Path]::GetFullPath($Path)
    $expectedRoot = [IO.Path]::GetFullPath(
        (Join-Path $PSScriptRoot '.test-work'))
    if ($resolvedRoot -ne $expectedRoot) {
        throw 'Test work path validation failed.'
    }

    $junctionPath = Join-Path $Path 'guard_corpus\link'
    if (Test-Path -LiteralPath $junctionPath) {
        $junction = Get-Item -LiteralPath $junctionPath -Force
        if (($junction.Attributes -band
                [IO.FileAttributes]::ReparsePoint) -eq 0) {
            throw 'Expected test junction is not a reparse point.'
        }
        [IO.Directory]::Delete($junctionPath, $false)
    }
    Remove-Item -LiteralPath $Path -Recurse -Force
}

function Invoke-ExpectFailure {
    param(
        [Parameter(Mandatory = $true)][scriptblock]$Action,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $failed = $false
    try {
        & $Action
    } catch {
        $failed = $true
    }
    if (-not $failed) {
        throw "$Name was accepted."
    }
}

$runner = Join-Path $PSScriptRoot 'Invoke-BlindEvaluation.ps1'
$fixtureRoot = Join-Path $PSScriptRoot 'fixtures'
$fixtureCorpus = Join-Path $fixtureRoot 'corpus'
$fixtureManifest = Join-Path $fixtureRoot 'manifest.csv'
$workRoot = Join-Path $PSScriptRoot '.test-work'
$outputPath = Join-Path $workRoot 'report.json'

Remove-TestWork -Path $workRoot
New-Item -ItemType Directory -Path $workRoot | Out-Null

try {
    $detectorArguments = @(
        '-NoProfile',
        '-ExecutionPolicy',
        'Bypass',
        '-File',
        (Join-Path $fixtureRoot 'MockDetector.ps1')
    )
    & $runner `
        -Manifest $fixtureManifest `
        -CorpusRoot $fixtureCorpus `
        -Detector (Join-Path $PSHOME 'powershell.exe') `
        -DetectorArguments $detectorArguments `
        -Output $outputPath `
        -TimeoutMs 5000 | Out-Null

    $reportText = Get-Content -LiteralPath $outputPath -Raw
    $report = $reportText | ConvertFrom-Json
    Assert-Equal $report.schema_version 2 'schema_version'
    Assert-Equal $report.evaluation_status 'complete' 'evaluation_status'
    if ($report.cohort_sha256 -notmatch '^[0-9A-F]{64}$') {
        throw 'cohort_sha256 has an invalid format.'
    }
    Assert-Equal $report.sample_count 20 'sample_count'
    Assert-Equal $report.minimum_class_count 10 'minimum_class_count'
    Assert-Equal $report.attempted_count 20 'attempted_count'
    Assert-Equal $report.evaluated_count 20 'evaluated_count'
    Assert-Equal $report.error_count 0 'error_count'
    Assert-Equal $report.not_evaluated_count 0 'not_evaluated_count'
    Assert-Equal $report.positive_count 10 'positive_count'
    Assert-Equal $report.negative_count 10 'negative_count'
    Assert-Equal $report.true_positive 8 'true_positive'
    Assert-Equal $report.true_negative 8 'true_negative'
    Assert-Equal $report.false_positive 2 'false_positive'
    Assert-Equal $report.false_negative 2 'false_negative'
    Assert-Equal $report.recall 0.8 'recall'
    Assert-Equal $report.specificity 0.8 'specificity'
    Assert-Equal $report.precision 0.8 'precision'
    Assert-Equal $report.false_positive_rate 0.2 'false_positive_rate'
    Assert-Equal $report.balanced_accuracy 0.8 'balanced_accuracy'

    $forbiddenFields = @('samples', 'sample_id', 'input_path')
    foreach ($field in $forbiddenFields) {
        if ($report.PSObject.Properties.Name -contains $field) {
            throw "Aggregate report leaked field '$field'."
        }
    }
    if ($reportText -match
        'case_0|positive_detected|positive_clear|negative_detected|negative_clear') {
        throw 'Aggregate report leaked a fixture identifier.'
    }

    $argumentDetector = Join-Path $workRoot 'ArgumentDetector.ps1'
    @'
param(
    [AllowEmptyString()][string]$EmptyValue,
    [string]$QuotedValue,
    [string]$InputPath
)
if ($EmptyValue.Length -ne 0 -or $QuotedValue -ne 'a"b') {
    exit 2
}
$value = (Get-Content -LiteralPath $InputPath -Raw).Trim()
if ($value -eq 'FIXTURE_DETECTED') {
    Write-Output '{"detected":true}'
    return
}
if ($value -eq 'FIXTURE_CLEAR') {
    Write-Output '{"detected":false}'
    return
}
exit 3
'@ | Set-Content -LiteralPath $argumentDetector -Encoding ASCII
    $argumentDetectorArguments = @(
        '-NoProfile',
        '-ExecutionPolicy',
        'Bypass',
        '-File',
        $argumentDetector,
        '',
        'a"b'
    )
    $argumentOutput = Join-Path $workRoot 'argument_report.json'
    & $runner `
        -Manifest $fixtureManifest `
        -CorpusRoot $fixtureCorpus `
        -Detector (Join-Path $PSHOME 'powershell.exe') `
        -DetectorArguments $argumentDetectorArguments `
        -Output $argumentOutput `
        -TimeoutMs 5000 | Out-Null
    $argumentReport = Get-Content -LiteralPath $argumentOutput -Raw |
        ConvertFrom-Json
    Assert-Equal $argumentReport.error_count 0 `
        'detector_arguments.error_count'

    $lexicalManifest = Join-Path $workRoot 'lexical_escape.csv'
    @(
        'sample_id,label,input_path',
        'case_escape,0,..\MockDetector.ps1'
    ) | Set-Content -LiteralPath $lexicalManifest -Encoding ASCII
    Invoke-ExpectFailure -Name 'Lexical corpus escape' -Action {
        & $runner `
            -Manifest $lexicalManifest `
            -CorpusRoot $fixtureCorpus `
            -Detector (Join-Path $PSHOME 'powershell.exe') `
            -DetectorArguments $detectorArguments `
            -Output (Join-Path $workRoot 'lexical_escape.json') `
            -TimeoutMs 5000 | Out-Null
    }

    $guardCorpus = Join-Path $workRoot 'guard_corpus'
    $guardTarget = Join-Path $workRoot 'guard_target'
    New-Item -ItemType Directory -Path $guardCorpus | Out-Null
    New-Item -ItemType Directory -Path $guardTarget | Out-Null
    Set-Content `
        -LiteralPath (Join-Path $guardTarget 'outside.txt') `
        -Value 'FIXTURE_CLEAR' `
        -Encoding ASCII
    $junctionPath = Join-Path $guardCorpus 'link'
    New-Item `
        -ItemType Junction `
        -Path $junctionPath `
        -Target $guardTarget | Out-Null
    $junctionManifest = Join-Path $workRoot 'junction_escape.csv'
    @(
        'sample_id,label,input_path',
        'case_junction,0,link\outside.txt'
    ) | Set-Content -LiteralPath $junctionManifest -Encoding ASCII
    Invoke-ExpectFailure -Name 'Junction corpus escape' -Action {
        & $runner `
            -Manifest $junctionManifest `
            -CorpusRoot $guardCorpus `
            -Detector (Join-Path $PSHOME 'powershell.exe') `
            -DetectorArguments $detectorArguments `
            -Output (Join-Path $workRoot 'junction_escape.json') `
            -TimeoutMs 5000 | Out-Null
    }

    $smallManifest = Join-Path $workRoot 'small_cohort.csv'
    @(
        'sample_id,label,input_path',
        'case_small,1,positive_detected_1.txt'
    ) | Set-Content -LiteralPath $smallManifest -Encoding ASCII
    Invoke-ExpectFailure -Name 'Small cohort' -Action {
        & $runner `
            -Manifest $smallManifest `
            -CorpusRoot $fixtureCorpus `
            -Detector (Join-Path $PSHOME 'powershell.exe') `
            -DetectorArguments $detectorArguments `
            -Output (Join-Path $workRoot 'small_cohort.json') `
            -TimeoutMs 5000 | Out-Null
    }

    $invalidLabelManifest = Join-Path $workRoot 'invalid_label.csv'
    @(
        'sample_id,label,input_path',
        'case_label,+01,positive_detected_1.txt'
    ) | Set-Content -LiteralPath $invalidLabelManifest -Encoding ASCII
    Invoke-ExpectFailure -Name 'Non-literal label' -Action {
        & $runner `
            -Manifest $invalidLabelManifest `
            -CorpusRoot $fixtureCorpus `
            -Detector (Join-Path $PSHOME 'powershell.exe') `
            -DetectorArguments $detectorArguments `
            -Output (Join-Path $workRoot 'invalid_label.json') `
            -TimeoutMs 5000 | Out-Null
    }

    $emptyArgumentCorpus = Join-Path $workRoot 'empty argument corpus'
    New-Item -ItemType Directory -Path $emptyArgumentCorpus | Out-Null
    $emptyArgumentManifest = Join-Path $workRoot 'empty_arguments.csv'
    $emptyArgumentRows = New-Object 'System.Collections.Generic.List[string]'
    $emptyArgumentRows.Add('sample_id,label,input_path')
    for ($index = 0; $index -lt 40; ++$index) {
        $label = if ($index -lt 10) { 1 } else { 0 }
        $detected = $index -ge 30
        $fileName = 'sample_{0:D2}.json' -f $index
        $payload = if ($detected) {
            '{"detected":true}'
        } else {
            '{"detected":false}'
        }
        [IO.File]::WriteAllText(
            (Join-Path $emptyArgumentCorpus $fileName),
            $payload,
            (New-Object System.Text.UTF8Encoding($false)))
        $emptyArgumentRows.Add(
            ('case_round_{0:D2},{1},{2}' -f $index, $label, $fileName))
    }
    $emptyArgumentRows |
        Set-Content -LiteralPath $emptyArgumentManifest -Encoding ASCII
    $emptyArgumentOutput = Join-Path $workRoot 'empty_arguments.json'
    & $runner `
        -Manifest $emptyArgumentManifest `
        -CorpusRoot $emptyArgumentCorpus `
        -Detector (Join-Path $env:SystemRoot 'System32\more.com') `
        -Output $emptyArgumentOutput `
        -TimeoutMs 5000 | Out-Null
    $roundingReport = Get-Content -LiteralPath $emptyArgumentOutput -Raw |
        ConvertFrom-Json
    Assert-Equal $roundingReport.attempted_count 40 `
        'empty_arguments.attempted_count'
    Assert-Equal $roundingReport.recall 0 'rounding.recall'
    Assert-Equal $roundingReport.specificity 0.666667 `
        'rounding.specificity'
    Assert-Equal $roundingReport.balanced_accuracy 0.333333 `
        'rounding.balanced_accuracy'

    $childWriter = Join-Path $workRoot 'ChildWriter.ps1'
    @'
param([string]$Marker)
Start-Sleep -Milliseconds 1200
[IO.File]::WriteAllText($Marker, 'escaped')
'@ | Set-Content -LiteralPath $childWriter -Encoding ASCII
    $timeoutDetector = Join-Path $workRoot 'TimeoutDetector.ps1'
    @'
param([string]$InputPath)
$child = Join-Path $PSScriptRoot 'ChildWriter.ps1'
$marker = Join-Path $PSScriptRoot 'child_marker.txt'
$arguments = @(
    '-NoProfile',
    '-ExecutionPolicy',
    'Bypass',
    '-File',
    $child,
    $marker
)
Start-Process `
    -FilePath (Join-Path $PSHOME 'powershell.exe') `
    -ArgumentList $arguments `
    -WindowStyle Hidden | Out-Null
Start-Sleep -Seconds 5
'@ | Set-Content -LiteralPath $timeoutDetector -Encoding ASCII

    $timeoutArguments = @(
        '-NoProfile',
        '-ExecutionPolicy',
        'Bypass',
        '-File',
        $timeoutDetector
    )
    $timeoutOutput = Join-Path $workRoot 'timeout.json'
    $timeoutWatch = [Diagnostics.Stopwatch]::StartNew()
    Invoke-ExpectFailure -Name 'Detector timeout' -Action {
        & $runner `
            -Manifest $fixtureManifest `
            -CorpusRoot $fixtureCorpus `
            -Detector (Join-Path $PSHOME 'powershell.exe') `
            -DetectorArguments $timeoutArguments `
            -Output $timeoutOutput `
            -TimeoutMs 300 | Out-Null
    }
    $timeoutWatch.Stop()
    if ($timeoutWatch.ElapsedMilliseconds -gt 3000) {
        throw "Detector timeout exceeded bound: $($timeoutWatch.ElapsedMilliseconds)ms."
    }
    Start-Sleep -Milliseconds 1500
    if (Test-Path -LiteralPath (Join-Path $workRoot 'child_marker.txt')) {
        throw 'Timed-out detector child escaped the job.'
    }

    $timeoutReport = Get-Content -LiteralPath $timeoutOutput -Raw |
        ConvertFrom-Json
    Assert-Equal $timeoutReport.evaluation_status 'incomplete' `
        'timeout.evaluation_status'
    Assert-Equal $timeoutReport.attempted_count 1 `
        'timeout.attempted_count'
    Assert-Equal $timeoutReport.evaluated_count 0 `
        'timeout.evaluated_count'
    Assert-Equal $timeoutReport.error_count 1 'timeout.error_count'
    Assert-Equal $timeoutReport.not_evaluated_count 19 `
        'timeout.not_evaluated_count'
    Assert-Null $timeoutReport.true_positive 'timeout.true_positive'
    Assert-Null $timeoutReport.true_negative 'timeout.true_negative'
    Assert-Null $timeoutReport.false_positive 'timeout.false_positive'
    Assert-Null $timeoutReport.false_negative 'timeout.false_negative'
    Assert-Null $timeoutReport.recall 'timeout.recall'
    Assert-Null $timeoutReport.specificity 'timeout.specificity'
    Assert-Null $timeoutReport.precision 'timeout.precision'
    Assert-Null $timeoutReport.false_positive_rate `
        'timeout.false_positive_rate'
    Assert-Null $timeoutReport.balanced_accuracy `
        'timeout.balanced_accuracy'

    $quickExitDetector = Join-Path $workRoot 'QuickExitDetector.cmd'
    @'
@echo off
start "" /b "%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0ChildWriter.ps1" "%~dp0quick_marker.txt"
echo {"detected":true}
exit /b 0
'@ | Set-Content -LiteralPath $quickExitDetector -Encoding ASCII
    $quickOutput = Join-Path $workRoot 'quick_exit.json'
    $quickArguments = @('/d', '/s', '/c', $quickExitDetector)
    $quickWatch = [Diagnostics.Stopwatch]::StartNew()
    Invoke-ExpectFailure -Name 'Inherited output handle timeout' -Action {
        & $runner `
            -Manifest $fixtureManifest `
            -CorpusRoot $fixtureCorpus `
            -Detector (Join-Path $env:SystemRoot 'System32\cmd.exe') `
            -DetectorArguments $quickArguments `
            -Output $quickOutput `
            -TimeoutMs 300 | Out-Null
    }
    $quickWatch.Stop()
    if ($quickWatch.ElapsedMilliseconds -gt 3000) {
        throw "Inherited handle timeout exceeded bound: $($quickWatch.ElapsedMilliseconds)ms."
    }
    Start-Sleep -Milliseconds 1500
    if (Test-Path -LiteralPath (Join-Path $workRoot 'quick_marker.txt')) {
        throw 'Quick-exit detector child escaped the job.'
    }

    $floodDetector = Join-Path $workRoot 'FloodDetector.ps1'
    @'
param([string]$InputPath)
[Console]::Out.Write(('x' * 300000))
'@ | Set-Content -LiteralPath $floodDetector -Encoding ASCII
    $floodArguments = @(
        '-NoProfile',
        '-ExecutionPolicy',
        'Bypass',
        '-File',
        $floodDetector
    )
    $floodOutput = Join-Path $workRoot 'flood.json'
    $floodWatch = [Diagnostics.Stopwatch]::StartNew()
    Invoke-ExpectFailure -Name 'Detector output limit' -Action {
        & $runner `
            -Manifest $fixtureManifest `
            -CorpusRoot $fixtureCorpus `
            -Detector (Join-Path $PSHOME 'powershell.exe') `
            -DetectorArguments $floodArguments `
            -Output $floodOutput `
            -TimeoutMs 5000 | Out-Null
    }
    $floodWatch.Stop()
    if ($floodWatch.ElapsedMilliseconds -gt 3000) {
        throw "Detector output limit exceeded bound: $($floodWatch.ElapsedMilliseconds)ms."
    }
    $floodReport = Get-Content -LiteralPath $floodOutput -Raw |
        ConvertFrom-Json
    Assert-Equal $floodReport.evaluation_status 'incomplete' `
        'flood.evaluation_status'
    Assert-Null $floodReport.recall 'flood.recall'
} finally {
    Remove-TestWork -Path $workRoot
}

Write-Output 'blind evaluation self-test passed'
