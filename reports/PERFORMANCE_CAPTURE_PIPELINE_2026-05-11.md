# Pylon/Nsight Command-Line Performance Capture Pipeline

Date: 2026-05-11
Scope: repeatable GPU Trace capture/export/analysis using the Pylon-packaged Nsight Graphics command line tools.

## Goal

This document keeps the pipeline independent of any particular application. The target executable, working directory, and app arguments are inputs.

The pipeline is:

1. Validate the capture tool and GPU driver.
2. Launch the target app through `ngfx.exe`.
3. Collect a live GPU Trace after a warmup window.
4. Auto-export report tables.
5. Compare `BASE\D3DPERF_EVENTS.xls` across runs.
6. Record command lines, output folders, and timing deltas.

## Tool Paths

Pylon Nsight Graphics CLI:

```powershell
REDACTED_LOCAL_TOOL_PATH
```

Related Pylon tools in the same folder:

```powershell
ngfx-capture.exe
ngfx-replay.exe
ngfx-cpp-export.exe
ngfx-ui.exe
```

Tool role summary:

| Tool | Role | Typical use in this pipeline |
|---|---|---|
| `ngfx.exe` | Main Nsight Graphics command-line front end. It can launch a target process, run activities such as GPU Trace Profiler, and auto-export report tables. | Primary tool for live GPU Trace captures and `BASE\D3DPERF_EVENTS.xls` generation. |
| `ngfx-capture.exe` | Frame-capture CLI. It launches or attaches to an app and records graphics captures such as `.ngfx-capture` files for frame debugging/replay workflows. | Useful when a debuggable frame capture is needed, but not the preferred path for pass-timing `D3DPERF_EVENTS.xls` export. |
| `ngfx-replay.exe` | Capture replayer CLI. It replays `.ngfx-capture` files and can emit metadata, screenshots, logs, or replay performance reports. | Useful for deterministic replay or metadata inspection after a frame capture. It is separate from the live GPU Trace flow. |
| `ngfx-cpp-export.exe` | C++ repro exporter. It converts a graphics capture into a standalone C++ replay/repro project. | Useful for reducing a captured graphics problem into a shareable/debuggable repro. Not needed for routine timing comparisons. |
| `ngfx-ui.exe` | Nsight Graphics GUI launcher. | Use for interactive inspection of captures/traces. Avoid it in automated performance runs unless a human needs to inspect the capture visually. |

Typical capture output root:

```powershell
C:\app\captures
```

## Tool Inspection

Check the CLI is callable:

```powershell
$ngfx = 'REDACTED_LOCAL_TOOL_PATH'
& $ngfx --version
& $ngfx --help
```

Show all GPU Trace options:

```powershell
& $ngfx --help-all
```

The relevant activity is:

```text
--activity "GPU Trace Profiler"
```

Useful general launch options:

```text
--output-dir <folder>
--exe <target executable>
--dir <target working directory>
--args <target command line as one string>
--env "A=1; B=2;"
--no-timeout
--verbose
```

## Driver Preflight

Record the GPU and driver before capturing:

```powershell
nvidia-smi --query-gpu=name,driver_version --format=csv,noheader
```

Keep this line with the report so captures can be compared later.

## Target App Contract

For clean command-line captures, the target app should ideally support:

- A deterministic startup scene or workload.
- An argument to exit automatically after a fixed frame count or duration.
- Stable pass markers, such as PIX/NVTX/D3D markers, so `D3DPERF_EVENTS.xls` has useful rows.
- A way to disable UI overlays if they perturb frame timing.
- Feature toggles controlled entirely by command-line arguments.

The exact target arguments are app-specific and should be treated as data, not baked into this pipeline.

## Live GPU Trace

Prefer live GPU Trace launch/export for pass marker timing.

Replay-based export may be useful for other workflows, but live capture has been the reliable path for producing populated `D3DPERF_EVENTS.xls` tables in this setup.

Pylon/Nsight options that have worked well:

```text
--activity "GPU Trace Profiler"
--start-after-frames 80
--limit-to-frames 1
--max-duration-ms 1000
--auto-export
--allocated-timestamps 300000
--allocated-event-buffer-memory-kb 60000
```

Notes:

- `--start-after-frames 80` gives the app a warmup window.
- `--limit-to-frames 1` captures one frame for a focused pass-marker sample.
- `--allocated-timestamps 300000` avoids timestamp allocation overflow in marker-heavy frames.
- `--allocated-event-buffer-memory-kb 60000` gives the event stream more room.
- Always create `--output-dir` before launching; `ngfx.exe` fails if the directory does not exist.

## Single Capture Template

```powershell
$ngfx = 'REDACTED_LOCAL_TOOL_PATH'

$targetExe = 'C:\path\to\target.exe'
$targetDir = 'C:\path\to'
$targetArgs = '--your --target --arguments'

$out = 'C:\app\captures\example_live_gputrace_ts300k_YYYYMMDD_HHMM'
New-Item -ItemType Directory -Force -Path $out | Out-Null

& $ngfx `
  --activity 'GPU Trace Profiler' `
  --output-dir $out `
  --exe $targetExe `
  --dir $targetDir `
  --args $targetArgs `
  --start-after-frames 80 `
  --limit-to-frames 1 `
  --max-duration-ms 1000 `
  --auto-export `
  --allocated-timestamps 300000 `
  --allocated-event-buffer-memory-kb 60000
```

Expected export files:

```text
<capture>\BASE\D3DPERF_EVENTS.xls
<capture>\BASE\FRAME.xls
<capture>\BASE\GPUTRACE_FRAME.xls
<capture>\BASE\GPUTRACE_REGIMES.xls
<capture>\BASE\REPRO_INFO.xls
<capture>\ReportGeneratorTags.txt
<capture>\*.ngfx-gputrace
```

For pass marker timing, use:

```text
<capture>\BASE\D3DPERF_EVENTS.xls
```

Avoid using `FRAME.xls` for per-pass comparisons; it is broader frame/report data, not the pass-marker table.

## Capture/Replay Fallback

If the target app cannot exit by itself after a fixed frame count, use a two-stage fallback:

1. Capture one graphics frame with `ngfx-capture.exe` and terminate the target after capture.
2. Launch `ngfx-replay.exe` through `ngfx.exe` GPU Trace Profiler, keep the replayer alive for several loops, trace one replayed frame, auto-export the report, and let `ngfx.exe` terminate the replay process after export.

This is not identical to tracing the original live app. It measures Nsight's replayer workload, so reset/replay overhead, captured driver state, and replay determinism can affect timings. Use it as a practical fallback for apps without automation hooks, and prefer direct live GPU Trace when the app can self-exit.

Stage 1: capture a specific warmed-up frame and terminate the target:

```powershell
$toolDir = 'REDACTED_LOCAL_TOOL_PATH'
$ngfxCapture = Join-Path $toolDir 'ngfx-capture.exe'

$targetExe = 'C:\path\to\target.exe'
$targetDir = 'C:\path\to'
$targetArgs = '--your --target --arguments'

$captureDir = 'C:\app\captures\example_frame_capture_YYYYMMDD_HHMM'
$captureName = 'target_frame_0081.ngfx-capture'
New-Item -ItemType Directory -Force -Path $captureDir | Out-Null

& $ngfxCapture `
  --exe $targetExe `
  --working-dir $targetDir `
  --args $targetArgs `
  --output-dir $captureDir `
  --output-file $captureName `
  --capture-frame 81 `
  --frame-count 1 `
  --terminate-after-capture `
  --no-hud
```

Notes:

- `--capture-frame` is 1-based and should be greater than 1.
- Use a capture frame that corresponds to the same warmup point you would normally trace. For example, `--start-after-frames 80` roughly maps to capturing frame `81`.
- `--terminate-after-capture` is available on `ngfx-capture.exe`, not on the live GPU Trace `ngfx.exe` path.

Stage 2: replay the captured frame under GPU Trace Profiler:

```powershell
$ngfx = Join-Path $toolDir 'ngfx.exe'
$ngfxReplay = Join-Path $toolDir 'ngfx-replay.exe'
$captureFile = Join-Path $captureDir $captureName

$traceOut = 'C:\app\captures\example_replay_gputrace_ts300k_YYYYMMDD_HHMM'
New-Item -ItemType Directory -Force -Path $traceOut | Out-Null

$replayArgs = "`"$captureFile`" --loop-count 60 --present-wb --vsync-off --no-timeout"

& $ngfx `
  --activity 'GPU Trace Profiler' `
  --output-dir $traceOut `
  --exe $ngfxReplay `
  --dir $toolDir `
  --args $replayArgs `
  --start-after-frames 1 `
  --limit-to-frames 1 `
  --max-duration-ms 1000 `
  --trace-timeout 300 `
  --auto-export `
  --allocated-timestamps 300000 `
  --allocated-event-buffer-memory-kb 60000 `
  --no-timeout
```

Expected export:

```text
<traceOut>\BASE\D3DPERF_EVENTS.xls
```

Notes:

- Keep `--loop-count` comfortably above `1`. With a single replay loop, the replayer can finish before GPU Trace attaches and starts fetching data.
- `--present-wb` has been more reliable for GPU Trace collection than `--present-hidden` in this fallback path.
- `ngfx.exe` terminates the replay process after a successful one-frame trace/export, so the high replay loop count is only there to keep the target alive long enough for attachment.

### Validated Fallback Result

This fallback was validated on 2026-05-11 with a DX12 target that did support app-level automation, so the fallback could be checked against a known-good live workflow.

Stage 1 succeeded with `ngfx-capture.exe`:

```text
--capture-frame 30
--frame-count 1
--terminate-after-capture
--no-hud
```

The target process was terminated after capture, and a single `.ngfx-capture` file was produced:

```text
C:\app\captures\fallback_test_frame_capture_20260511_225742\fallback_target_frame_0030.ngfx-capture
```

The first Stage 2 attempt used a short hidden replay:

```text
ngfx-replay.exe <capture> --loop-count 1 --present-hidden --no-timeout
```

That attempt failed. The replay process completed too quickly for GPU Trace to attach and begin fetching data, producing:

```text
Operation timeout, Data fetching never started
```

The associated crash stack involved `D3D12Core.dll`, `WarpVizTarget.dll`, `WarpViz.Injection.dll`, and `ngfx-replay.exe`. Treat this as a replay/GPU Trace attachment failure mode, not as proof that the original application crashed.

The successful Stage 2 retry kept the replayer alive and used a real workbench present path:

```text
ngfx-replay.exe <capture> --loop-count 60 --present-wb --vsync-off --no-timeout
ngfx.exe --start-after-frames 1 --limit-to-frames 1 --trace-timeout 300 --auto-export --no-timeout
```

This produced a populated GPU Trace export:

```text
C:\app\captures\fallback_test_replay_gputrace_ts300k_loop60_20260511_225941\BASE\D3DPERF_EVENTS.xls
```

Example exported pass timings from that fallback replay trace:

```text
Frame             2.86918 ms
GBuffer           0.424479 ms
RT Shadow         0.211999 ms
RT AO             0.233823 ms
RT Reflection     0.520735 ms
RT Diffuse GI     1.17405 ms
Temporal Denoise  0.171551 ms
Lighting          0.091774 ms
Temporal AA       0.025217 ms
Tone Map          0.015359 ms
```

Operational rule of thumb:

- Use `ngfx-capture.exe --terminate-after-capture` to solve the "target app cannot auto-exit" problem.
- Use replay only as a bridge back into `ngfx.exe` GPU Trace export.
- Keep the replay process alive long enough for attachment; `--loop-count 60 --present-wb --vsync-off --no-timeout` worked in this validation.
- Compare fallback replay results against other fallback replay results, not directly against live app traces, unless the replay/live delta has been characterized for that workload.

### Fallback Replay vs Direct Live Trace

The same DX12 target workload was captured two ways for a quick sanity check:

1. Fallback path: `ngfx-capture.exe` captured frame 30, then `ngfx-replay.exe` was traced by `ngfx.exe`.
2. Direct live path: `ngfx.exe` traced the live target with `--start-after-frames 29`, `--limit-to-frames 1`, and app-level auto-exit enabled.

Compared files:

```text
Fallback replay:
C:\app\captures\fallback_test_replay_gputrace_ts300k_loop60_20260511_225941\BASE\D3DPERF_EVENTS.xls

Direct live:
C:\app\captures\fallback_compare_direct_live_gputrace_ts300k_frame30_20260511_2308\BASE\D3DPERF_EVENTS.xls
```

Result:

| Event | Fallback replay ms | Direct live ms | Delta ms | Delta |
|---|---:|---:|---:|---:|
| Frame | 2.869180 | 3.204900 | -0.335720 | -10.48% |
| GBuffer | 0.424479 | 0.471616 | -0.047137 | -9.99% |
| RT Shadow | 0.211999 | 0.345951 | -0.133952 | -38.72% |
| RT AO | 0.233823 | 0.240447 | -0.006624 | -2.75% |
| RT Reflection | 0.520735 | 0.516191 | +0.004544 | +0.88% |
| RT Diffuse GI | 1.174050 | 1.280480 | -0.106430 | -8.31% |
| Temporal Denoise | 0.171551 | 0.174463 | -0.002912 | -1.67% |
| Lighting | 0.091774 | 0.107007 | -0.015233 | -14.24% |
| Temporal AA | 0.025217 | 0.024576 | +0.000641 | +2.61% |
| Tone Map | 0.015359 | 0.044128 | -0.028769 | -65.19% |

The fallback replay trace also emitted this replay-only marker:

```text
[NSIGHT] Blit from replay to real swapchain    0.011327 ms
```

Interpretation:

- The fallback replay path produced a valid, populated pass-marker table.
- The fallback `Frame` row was about 10.5% lower than direct live for this workload. Even if the replay-only blit marker is added manually, the fallback total is still about 10.1% lower.
- RT Reflection matched closely in this sample, but RT Shadow, RT Diffuse GI, GBuffer, Lighting, and Tone Map did not.
- This confirms that fallback replay is useful for automated capture/export when the app cannot self-exit, but its timings should be treated as a separate measurement mode.
- For feature comparisons, compare direct-live runs against direct-live runs, or fallback-replay runs against fallback-replay runs. Do not mix the two as if they were equivalent.

### 1000-Frame Warmup Retest

The fallback/direct comparison was repeated with a larger warmup window:

- Fallback path captured frame `1001` with `ngfx-capture.exe --capture-frame 1001 --terminate-after-capture`.
- The captured frame was replayed with `ngfx-replay.exe --loop-count 60 --present-wb --vsync-off --no-timeout` and exported through `ngfx.exe` GPU Trace.
- Direct live path used `ngfx.exe --start-after-frames 1000 --limit-to-frames 1`; the target process was allowed to run beyond the capture point with app-level auto-exit.

Compared files:

```text
Fallback replay:
C:\app\captures\fallback_warmup1000_replay_gputrace_ts300k_20260511_230517\BASE\D3DPERF_EVENTS.xls

Direct live:
C:\app\captures\direct_live_warmup1000_gputrace_ts300k_20260511_230556\BASE\D3DPERF_EVENTS.xls
```

Result:

| Event | Fallback replay ms | Direct live ms | Delta ms | Delta |
|---|---:|---:|---:|---:|
| Frame | 2.886210 | 3.126630 | -0.240420 | -7.69% |
| GBuffer | 0.447583 | 0.449600 | -0.002017 | -0.45% |
| RT Shadow | 0.210335 | 0.490687 | -0.280352 | -57.13% |
| RT AO | 0.234495 | 0.249855 | -0.015360 | -6.15% |
| RT Reflection | 0.511039 | 0.516095 | -0.005056 | -0.98% |
| RT Diffuse GI | 1.180990 | 1.095900 | +0.085090 | +7.76% |
| Temporal Denoise | 0.171647 | 0.167711 | +0.003936 | +2.35% |
| Lighting | 0.090366 | 0.089886 | +0.000480 | +0.53% |
| Temporal AA | 0.024417 | 0.022880 | +0.001537 | +6.72% |
| Tone Map | 0.015135 | 0.044000 | -0.028865 | -65.60% |

The fallback replay trace also emitted:

```text
[NSIGHT] Blit from replay to real swapchain    0.010239 ms
```

Interpretation:

- Increasing warmup from about 30 frames to about 1000 frames reduced the overall `Frame` gap from roughly `-10.48%` to `-7.69%`.
- Adding the replay-only blit marker manually gives `2.896449 ms`, still `-7.36%` below the direct live frame time.
- Several passes became very close (`GBuffer`, `RT Reflection`, `Lighting`), but others still diverged significantly (`RT Shadow`, `Tone Map`, and `RT Diffuse GI`).
- The larger warmup helps, but does not make fallback replay equivalent to direct live tracing.
- Treat fallback replay as a consistent automation fallback, not as a drop-in replacement for live timing.

Practical comparison guidance:

- Treat single-frame fallback/direct comparisons as a smoke test, not a final performance conclusion.
- For real evaluation, use a long warmup, then average hundreds of measured frames.
- Record at least mean, median, p95, min/max, and standard deviation per pass.
- A replay loop over one captured frame is not the same as a multi-frame workload average. It measures repeatability of one captured frame.
- For closer fallback/live calibration, capture a multi-frame window with `ngfx-capture.exe --frame-count <N>` after warmup, then replay/export that window if the toolchain supports the desired report.
- Compare direct-live runs against direct-live runs and fallback-replay runs against fallback-replay runs. Use a separate calibration pass before mixing the two modes.

## Reusable PowerShell Function

```powershell
function Invoke-PylonGpuTrace {
  param(
    [Parameter(Mandatory=$true)] [string] $OutputDir,
    [Parameter(Mandatory=$true)] [string] $TargetExe,
    [Parameter(Mandatory=$true)] [string] $TargetDir,
    [Parameter(Mandatory=$true)] [string] $TargetArgs,
    [int] $StartAfterFrames = 80,
    [int] $LimitToFrames = 1,
    [int] $MaxDurationMs = 1000,
    [int] $AllocatedTimestamps = 300000,
    [int] $EventBufferKb = 60000
  )

  $ngfx = 'REDACTED_LOCAL_TOOL_PATH'

  New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

  & $ngfx `
    --activity 'GPU Trace Profiler' `
    --output-dir $OutputDir `
    --exe $TargetExe `
    --dir $TargetDir `
    --args $TargetArgs `
    --start-after-frames $StartAfterFrames `
    --limit-to-frames $LimitToFrames `
    --max-duration-ms $MaxDurationMs `
    --auto-export `
    --allocated-timestamps $AllocatedTimestamps `
    --allocated-event-buffer-memory-kb $EventBufferKb
}
```

Example use:

```powershell
Invoke-PylonGpuTrace `
  -OutputDir 'C:\app\captures\feature_off_live_gputrace_ts300k_20260511_2200' `
  -TargetExe 'C:\path\to\target.exe' `
  -TargetDir 'C:\path\to' `
  -TargetArgs '--stable-workload --feature off --exit-after-frames 160'

Invoke-PylonGpuTrace `
  -OutputDir 'C:\app\captures\feature_on_live_gputrace_ts300k_20260511_2201' `
  -TargetExe 'C:\path\to\target.exe' `
  -TargetDir 'C:\path\to' `
  -TargetArgs '--stable-workload --feature on --exit-after-frames 160'
```

## Naming Convention

Use names that make captures sortable and self-describing:

```text
<app-or-suite>_<feature>_<mode>_live_gputrace_ts300k_<YYYYMMDD_HHMM>
```

Examples:

```text
renderer_feature_off_live_gputrace_ts300k_20260511_2200
renderer_feature_on_live_gputrace_ts300k_20260511_2201
renderer_feature_on_pair2_live_gputrace_ts300k_20260511_2202
renderer_feature_off_pair2_live_gputrace_ts300k_20260511_2203
```

## Pairwise Analysis

`D3DPERF_EVENTS.xls` is tab-delimited and can be read with `Import-Csv -Delimiter "`t"`.

```powershell
$offPath = 'C:\app\captures\feature_off\BASE\D3DPERF_EVENTS.xls'
$onPath = 'C:\app\captures\feature_on\BASE\D3DPERF_EVENTS.xls'

$off = Import-Csv -Delimiter "`t" -LiteralPath $offPath
$on = Import-Csv -Delimiter "`t" -LiteralPath $onPath

$rows = foreach ($o in $off) {
  $name = $o.event_text.Trim()
  $m = $on | Where-Object { $_.event_text.Trim() -eq $name } | Select-Object -First 1
  if ($m) {
    $offMs = [double]$o.time_ms
    $onMs = [double]$m.time_ms
    [pscustomobject]@{
      Pass = $name
      OffMs = '{0:N6}' -f $offMs
      OnMs = '{0:N6}' -f $onMs
      DeltaMs = '{0:N6}' -f ($onMs - $offMs)
      DeltaPct = '{0:N2}%' -f ((($onMs - $offMs) / $offMs) * 100.0)
    }
  }
}

$rows | Format-Table -AutoSize
```

## Multi-Sample Analysis

Use at least two matched pairs for pass-level conclusions. Single-frame GPU Trace captures can include unrelated variance in adjacent passes.

```powershell
$samples = @(
  @{Mode='off'; Pair=1; Path='C:\app\captures\feature_off_pair1\BASE\D3DPERF_EVENTS.xls'},
  @{Mode='on';  Pair=1; Path='C:\app\captures\feature_on_pair1\BASE\D3DPERF_EVENTS.xls'},
  @{Mode='on';  Pair=2; Path='C:\app\captures\feature_on_pair2\BASE\D3DPERF_EVENTS.xls'},
  @{Mode='off'; Pair=2; Path='C:\app\captures\feature_off_pair2\BASE\D3DPERF_EVENTS.xls'}
)

$rows = foreach ($s in $samples) {
  Import-Csv -Delimiter "`t" -LiteralPath $s.Path | ForEach-Object {
    [pscustomobject]@{
      Pair = $s.Pair
      Mode = $s.Mode
      Pass = $_.event_text.Trim()
      Ms = [double]$_.time_ms
      Path = $s.Path
    }
  }
}

$targetPass = 'TARGET PASS NAME'

'Per-sample target pass:'
$rows |
  Where-Object { $_.Pass -eq $targetPass } |
  Sort-Object Pair,Mode |
  Format-Table Pair,Mode,Pass,@{n='Ms';e={'{0:N6}' -f $_.Ms}} -AutoSize

'Pair deltas (on - off):'
$deltaRows = foreach ($pair in (($rows | Select-Object -ExpandProperty Pair -Unique) | Sort-Object)) {
  $off = $rows | Where-Object { $_.Pair -eq $pair -and $_.Mode -eq 'off' -and $_.Pass -eq $targetPass }
  $on = $rows | Where-Object { $_.Pair -eq $pair -and $_.Mode -eq 'on' -and $_.Pass -eq $targetPass }
  if ($off -and $on) {
    [pscustomobject]@{
      Pair = $pair
      Pass = $targetPass
      OffMs = '{0:N6}' -f $off.Ms
      OnMs = '{0:N6}' -f $on.Ms
      DeltaMs = '{0:N6}' -f ($on.Ms - $off.Ms)
      DeltaPct = '{0:N2}%' -f ((($on.Ms - $off.Ms) / $off.Ms) * 100.0)
    }
  }
}
$deltaRows | Format-Table -AutoSize

'Average target pass delta:'
$offAvg = (($rows | Where-Object { $_.Mode -eq 'off' -and $_.Pass -eq $targetPass }) | Measure-Object Ms -Average).Average
$onAvg = (($rows | Where-Object { $_.Mode -eq 'on' -and $_.Pass -eq $targetPass }) | Measure-Object Ms -Average).Average
[pscustomobject]@{
  Pass = $targetPass
  OffAvgMs = '{0:N6}' -f $offAvg
  OnAvgMs = '{0:N6}' -f $onAvg
  DeltaMs = '{0:N6}' -f ($onAvg - $offAvg)
  DeltaPct = '{0:N2}%' -f ((($onAvg - $offAvg) / $offAvg) * 100.0)
} | Format-Table -AutoSize
```

## Export Validation

After capture, confirm the report has the expected files:

```powershell
$capture = 'C:\app\captures\your_capture'
Get-ChildItem -LiteralPath $capture -Recurse -Depth 2 |
  Select-Object FullName,Length,LastWriteTime |
  Sort-Object FullName
```

Quickly inspect pass markers:

```powershell
Get-Content -LiteralPath 'C:\app\captures\your_capture\BASE\D3DPERF_EVENTS.xls'
```

## Troubleshooting

If `ngfx.exe` says `No such output directory`, create the folder first:

```powershell
New-Item -ItemType Directory -Force -Path 'C:\app\captures\your_capture_name' | Out-Null
```

If marker timings are empty, missing, or corrupt, increase allocation:

```text
--allocated-timestamps 300000
--allocated-event-buffer-memory-kb 60000
```

If the target takes a long time to become traceable, add:

```text
--no-timeout
```

If attach/search behavior needs debugging, add:

```text
--verbose
```

Nsight may print:

```text
The D3D12 Debug Layer was force disabled due to launch settings.
```

That warning can appear in successful captures and does not necessarily invalidate the export.

If the target app remains alive after capture, check and stop only the process you launched intentionally:

```powershell
Get-Process <process-name> -ErrorAction SilentlyContinue | Select-Object Id,ProcessName,StartTime,CPU
Get-Process -Id <pid> -ErrorAction SilentlyContinue | Stop-Process -Force
```

## Report Checklist

Each performance report should include:

- Pylon/Nsight CLI path and version if available.
- GPU and driver version.
- Full `ngfx.exe` command line or `Invoke-PylonGpuTrace` parameters.
- Target executable path, working directory, and exact target arguments.
- Capture folder paths.
- `BASE\D3DPERF_EVENTS.xls` table.
- Target pass name.
- Pair-by-pair deltas.
- Average target-pass delta.
- Notes about unrelated pass or frame marker noise.
- Any warnings, missing markers, timestamp overflow symptoms, or export failures.
