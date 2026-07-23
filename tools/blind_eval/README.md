# Blind Evaluation Runner

This runner evaluates an external detector as a black box. The saved report
contains aggregate counts and rates only.

The runner targets Windows PowerShell 5.1 or later. Detector processes start
suspended, enter a kill-on-close Windows Job, and then resume. The timeout
covers the full process tree and redirected output. Each output stream is
limited to 262,144 characters.

## Manifest

Use a CSV file with these columns:

```csv
sample_id,label,input_path
case_001,1,trace_a.bin
case_002,0,trace_b.bin
```

- `sample_id` must be unique.
- `label` must be `0` or `1`.
- `input_path` must resolve to a file below `CorpusRoot`.
- Input paths must be unique and may not cross a reparse point.
- Each label must contain at least 10 samples.

## Detector contract

The detector receives one positional argument: the absolute sample path. Its
last non-empty stdout line must be one of:

```json
{"detected":true}
```

```json
{"detected":false}
```

The detector remains an external black box. The runner saves no sample-level
decision log. Detector invocation errors stop the run. An incomplete report
contains coverage counts while confusion-matrix counts and rates are `null`.
Use one fixed, access-controlled cohort for comparisons. Cohort aggregation is
an output-shaping boundary rather than a differential-privacy boundary. Compare
`cohort_sha256` across runs to reject accidental cohort drift.

## Run

From a PowerShell session:

```powershell
$detectorArgs = @('-NoProfile', '-File', 'E:\path\detector.ps1')
.\Invoke-BlindEvaluation.ps1 `
    -Manifest 'E:\path\manifest.csv' `
    -CorpusRoot 'E:\path\corpus' `
    -Detector 'powershell.exe' `
    -DetectorArguments $detectorArgs `
    -Output 'E:\path\aggregate.json'
```

Schema version 2 reports completion status, cohort counts, attempted and
evaluated counts, confusion-matrix counts, recall, specificity, precision,
false-positive rate, balanced accuracy, invocation errors, and elapsed time.

## Self-test

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
    -File .\Test-BlindEvaluation.ps1
```
