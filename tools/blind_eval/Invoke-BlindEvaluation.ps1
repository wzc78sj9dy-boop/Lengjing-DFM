[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Manifest,

    [Parameter(Mandatory = $true)]
    [string]$CorpusRoot,

    [Parameter(Mandatory = $true)]
    [string]$Detector,

    [AllowEmptyCollection()]
    [AllowEmptyString()]
    [string[]]$DetectorArguments = @(),

    [Parameter(Mandatory = $true)]
    [string]$Output,

    [ValidateRange(100, 600000)]
    [int]$TimeoutMs = 5000
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$nativeRunnerType = [AppDomain]::CurrentDomain.GetAssemblies() |
    ForEach-Object {
        $_.GetType('BlindEval.NativeProcessRunner', $false)
    } |
    Where-Object { $null -ne $_ } |
    Select-Object -First 1
if ($null -eq $nativeRunnerType) {
    Add-Type -Path (Join-Path $PSScriptRoot 'NativeProcessRunner.cs')
}

function Resolve-Application {
    param([Parameter(Mandatory = $true)][string]$Value)

    if (Test-Path -LiteralPath $Value -PathType Leaf) {
        return (Resolve-Path -LiteralPath $Value).ProviderPath
    }

    $command = Get-Command -Name $Value -CommandType Application -ErrorAction Stop |
        Select-Object -First 1
    return $command.Source
}

function ConvertTo-QuotedArgument {
    param([AllowEmptyString()][string]$Value)

    if ($Value.IndexOf([char]0) -ge 0) {
        throw 'Detector arguments contain an unsupported character.'
    }
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') {
        return $Value
    }

    $builder = New-Object System.Text.StringBuilder
    [void]$builder.Append('"')
    $slashCount = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') {
            ++$slashCount
            continue
        }
        if ($character -eq '"') {
            if ($slashCount -gt 0) {
                [void]$builder.Append(('\' * ($slashCount * 2)))
            }
            [void]$builder.Append('\')
            [void]$builder.Append('"')
            $slashCount = 0
            continue
        }
        if ($slashCount -gt 0) {
            [void]$builder.Append(('\' * $slashCount))
            $slashCount = 0
        }
        [void]$builder.Append($character)
    }
    if ($slashCount -gt 0) {
        [void]$builder.Append(('\' * ($slashCount * 2)))
    }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function Invoke-Detector {
    param(
        [Parameter(Mandatory = $true)][string]$Application,
        [AllowEmptyCollection()]
        [AllowEmptyString()]
        [string[]]$PrefixArguments = @(),
        [Parameter(Mandatory = $true)][string]$InputPath,
        [Parameter(Mandatory = $true)][int]$ProcessTimeoutMs
    )

    $arguments = @($PrefixArguments) + @($InputPath)
    $argumentLine = ($arguments | ForEach-Object {
        ConvertTo-QuotedArgument -Value $_
    }) -join ' '

    try {
        $processResult = [BlindEval.NativeProcessRunner]::Run(
            $Application,
            $argumentLine,
            $ProcessTimeoutMs,
            262144)
        if (-not $processResult.Started -or
            $processResult.TimedOut -or
            $processResult.OutputLimitExceeded -or
            $processResult.StreamReadFailed -or
            $processResult.ExitCode -ne 0) {
            return @{ success = $false; detected = $false }
        }

        $stdout = $processResult.StandardOutput
        $lines = @($stdout -split "\r?\n" | Where-Object {
            $_.Trim().Length -gt 0
        })
        if ($lines.Count -eq 0) {
            return @{ success = $false; detected = $false }
        }

        try {
            $payload = $lines[-1] | ConvertFrom-Json -ErrorAction Stop
        } catch {
            return @{ success = $false; detected = $false }
        }
        if ($payload.PSObject.Properties.Name -notcontains 'detected') {
            return @{ success = $false; detected = $false }
        }

        $value = $payload.detected
        if ($value -is [bool]) {
            return @{ success = $true; detected = [bool]$value }
        }
        if (($value -is [int] -or $value -is [long]) -and
            ($value -eq 0 -or $value -eq 1)) {
            return @{ success = $true; detected = ($value -eq 1) }
        }
        return @{ success = $false; detected = $false }
    } catch {
        return @{ success = $false; detected = $false }
    }
}

function Get-Ratio {
    param([int]$Numerator, [int]$Denominator)

    if ($Denominator -eq 0) {
        return $null
    }
    return [Math]::Round(
        [double]$Numerator / [double]$Denominator,
        6,
        [MidpointRounding]::AwayFromZero)
}

function Test-CorpusFile {
    param(
        [Parameter(Mandatory = $true)][string]$RootPath,
        [Parameter(Mandatory = $true)][string]$CandidatePath
    )

    $comparison = [StringComparison]::OrdinalIgnoreCase
    $root = [IO.Path]::GetFullPath($RootPath)
    $rootPathLength = [IO.Path]::GetPathRoot($root).Length
    if ($root.Length -gt $rootPathLength) {
        $root = $root.TrimEnd('\', '/')
    }
    $candidate = [IO.Path]::GetFullPath($CandidatePath)
    $prefix = $root.TrimEnd('\', '/') +
        [IO.Path]::DirectorySeparatorChar
    if (-not $candidate.StartsWith($prefix, $comparison) -or
        -not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        return $false
    }

    $current = Get-Item -LiteralPath $candidate -Force
    while ($null -ne $current) {
        if ([string]::Equals($current.FullName, $root, $comparison)) {
            return $true
        }
        if (($current.Attributes -band
                [IO.FileAttributes]::ReparsePoint) -ne 0) {
            return $false
        }
        if ($current -is [IO.DirectoryInfo]) {
            $current = $current.Parent
        } else {
            $current = $current.Directory
        }
    }
    return $false
}

$manifestPath = (Resolve-Path -LiteralPath $Manifest).ProviderPath
$corpusPath = (Resolve-Path -LiteralPath $CorpusRoot).ProviderPath
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw 'Manifest must be a file.'
}
if (-not (Test-Path -LiteralPath $corpusPath -PathType Container)) {
    throw 'CorpusRoot must be a directory.'
}

$detectorPath = Resolve-Application -Value $Detector
$rows = @(Import-Csv -LiteralPath $manifestPath)
if ($rows.Count -eq 0) {
    throw 'Manifest has no samples.'
}

$requiredColumns = @('sample_id', 'label', 'input_path')
foreach ($column in $requiredColumns) {
    if ($rows[0].PSObject.Properties.Name -notcontains $column) {
        throw "Manifest is missing column '$column'."
    }
}

$ids = New-Object 'System.Collections.Generic.HashSet[string]' (
    [StringComparer]::Ordinal)
$inputPaths = New-Object 'System.Collections.Generic.HashSet[string]' (
    [StringComparer]::OrdinalIgnoreCase)
$samples = New-Object 'System.Collections.Generic.List[object]'
$cohortBuilder = New-Object System.Text.StringBuilder
$minimumClassCount = 10
$positiveCount = 0
$negativeCount = 0

for ($rowIndex = 0; $rowIndex -lt $rows.Count; ++$rowIndex) {
    $row = $rows[$rowIndex]
    $sampleId = [string]$row.sample_id
    if ([string]::IsNullOrWhiteSpace($sampleId) -or
        -not $ids.Add($sampleId)) {
        throw "Manifest row $($rowIndex + 2) has an invalid sample_id."
    }

    $labelText = [string]$row.label
    if ($labelText -ne '0' -and $labelText -ne '1') {
        throw "Manifest row $($rowIndex + 2) has an invalid label."
    }
    $label = [int]$labelText
    if ($label -eq 1) {
        ++$positiveCount
    } else {
        ++$negativeCount
    }

    $relativePath = [string]$row.input_path
    if ([string]::IsNullOrWhiteSpace($relativePath)) {
        throw "Manifest row $($rowIndex + 2) has an empty input_path."
    }
    try {
        $candidatePath = [IO.Path]::GetFullPath(
            (Join-Path -Path $corpusPath -ChildPath $relativePath))
    } catch {
        throw "Manifest row $($rowIndex + 2) has an invalid input_path."
    }
    if (-not (Test-CorpusFile `
            -RootPath $corpusPath `
            -CandidatePath $candidatePath) -or
        -not $inputPaths.Add($candidatePath)) {
        throw "Manifest row $($rowIndex + 2) has an invalid input_path."
    }

    $fileHash = (Get-FileHash `
        -LiteralPath $candidatePath `
        -Algorithm SHA256).Hash
    foreach ($field in @(
            $sampleId,
            $labelText,
            $relativePath,
            $fileHash)) {
        [void]$cohortBuilder.Append($field.Length)
        [void]$cohortBuilder.Append(':')
        [void]$cohortBuilder.Append($field)
    }
    [void]$cohortBuilder.Append(';')

    $samples.Add([pscustomobject]@{
        label = $label
        input_path = $candidatePath
    })
}

if ($positiveCount -lt $minimumClassCount -or
    $negativeCount -lt $minimumClassCount) {
    throw "Each label requires at least $minimumClassCount samples."
}

$cohortBytes = (New-Object System.Text.UTF8Encoding($false)).GetBytes(
    $cohortBuilder.ToString())
$cohortHasher = [Security.Cryptography.SHA256]::Create()
try {
    $cohortHashBytes = $cohortHasher.ComputeHash($cohortBytes)
} finally {
    $cohortHasher.Dispose()
}
$cohortHash = ([BitConverter]::ToString($cohortHashBytes)).Replace('-', '')

$truePositive = 0
$trueNegative = 0
$falsePositive = 0
$falseNegative = 0
$attemptedCount = 0
$evaluatedCount = 0
$errorCount = 0
$startedAt = [Diagnostics.Stopwatch]::StartNew()

foreach ($sample in $samples) {
    ++$attemptedCount
    $result = Invoke-Detector `
        -Application $detectorPath `
        -PrefixArguments $DetectorArguments `
        -InputPath $sample.input_path `
        -ProcessTimeoutMs $TimeoutMs
    if (-not $result.success) {
        ++$errorCount
        break
    }
    ++$evaluatedCount

    if ($sample.label -eq 1 -and $result.detected) {
        ++$truePositive
    } elseif ($sample.label -eq 1) {
        ++$falseNegative
    } elseif ($result.detected) {
        ++$falsePositive
    } else {
        ++$trueNegative
    }
}

$startedAt.Stop()
$notEvaluatedCount = $rows.Count - $attemptedCount
$evaluationStatus = 'complete'
$reportedTruePositive = $truePositive
$reportedTrueNegative = $trueNegative
$reportedFalsePositive = $falsePositive
$reportedFalseNegative = $falseNegative
$recall = $null
$specificity = $null
$precision = $null
$falsePositiveRate = $null
$balancedAccuracy = $null

if ($errorCount -eq 0) {
    $positiveDenominator = $truePositive + $falseNegative
    $negativeDenominator = $trueNegative + $falsePositive
    $recall = Get-Ratio `
        -Numerator $truePositive `
        -Denominator $positiveDenominator
    $specificity = Get-Ratio `
        -Numerator $trueNegative `
        -Denominator $negativeDenominator
    $precision = Get-Ratio `
        -Numerator $truePositive `
        -Denominator ($truePositive + $falsePositive)
    $falsePositiveRate = Get-Ratio `
        -Numerator $falsePositive `
        -Denominator $negativeDenominator
    if ($positiveDenominator -gt 0 -and $negativeDenominator -gt 0) {
        $balancedAccuracy = [Math]::Round(
            (([double]$truePositive / [double]$positiveDenominator) +
             ([double]$trueNegative / [double]$negativeDenominator)) /
                2.0,
            6,
            [MidpointRounding]::AwayFromZero)
    }
} else {
    $evaluationStatus = 'incomplete'
    $reportedTruePositive = $null
    $reportedTrueNegative = $null
    $reportedFalsePositive = $null
    $reportedFalseNegative = $null
}

$report = [ordered]@{
    schema_version = 2
    evaluation_status = $evaluationStatus
    cohort_sha256 = $cohortHash
    sample_count = $rows.Count
    minimum_class_count = $minimumClassCount
    attempted_count = $attemptedCount
    evaluated_count = $evaluatedCount
    error_count = $errorCount
    not_evaluated_count = $notEvaluatedCount
    positive_count = $positiveCount
    negative_count = $negativeCount
    true_positive = $reportedTruePositive
    true_negative = $reportedTrueNegative
    false_positive = $reportedFalsePositive
    false_negative = $reportedFalseNegative
    recall = $recall
    specificity = $specificity
    precision = $precision
    false_positive_rate = $falsePositiveRate
    balanced_accuracy = $balancedAccuracy
    elapsed_ms = $startedAt.ElapsedMilliseconds
}

$outputPath = [IO.Path]::GetFullPath($Output)
$outputDirectory = Split-Path -Parent $outputPath
if (-not (Test-Path -LiteralPath $outputDirectory -PathType Container)) {
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
}

$json = $report | ConvertTo-Json -Depth 3
$temporaryPath = "$outputPath.tmp.$PID"
$encoding = New-Object System.Text.UTF8Encoding($false)
try {
    [IO.File]::WriteAllText($temporaryPath, $json, $encoding)
    Move-Item -LiteralPath $temporaryPath -Destination $outputPath -Force
} finally {
    if (Test-Path -LiteralPath $temporaryPath) {
        Remove-Item -LiteralPath $temporaryPath -Force
    }
}

Write-Output $json
if ($errorCount -gt 0) {
    throw "$errorCount detector invocation(s) failed."
}
