# probe-lifecycle.ps1 - the same question for all four programs: does starting
# and stopping one give everything back?
#
#   .\test\probe-lifecycle.ps1 [-Rounds 3] [-Exe <path>]...
#
# The other probes each open their program once and drive it. That is the right
# shape for testing behaviour and the wrong shape for testing resources: a
# handle leak, a tray icon that outlives its process, or a hotkey left
# registered do not show up on the first run. They show up on the third, as a
# program that will not start, a taskbar that keeps a dead entry, or a
# shortcut that stopped working after an unrelated restart.
#
# The counts are read from the live process (GetProcessHandleCount and
# GetGuiResources), not inferred, and the comparison is between the first and
# the last round so a one-off cost at startup - the icon, the font, the GDI+
# startup - is not mistaken for a leak. A few handles of slack is allowed on
# purpose; hundreds is not.
#
# Each program is stopped the way a user stops it, not by killing it: the tray
# command for picker, WM_CLOSE for envtool and slice, and for shot - whose host
# window is a message-only shell for the tray icon and has no close path - a
# kill, which is the only way that program actually ends.
#
# ASCII only (see probe-focus.ps1).

[CmdletBinding()]
param([int]$Rounds = 3, [int]$Slack = 6)

$ErrorActionPreference = 'Continue'
. (Join-Path $PSScriptRoot 'probe-lib.ps1')

# Refuse a round count that cannot test anything, loudly. Every check below is
# of the form "$opened of $Rounds", so zero rounds makes all of them compare 0
# to 0 and report green - a suite that proves nothing and says so in a
# reassuring colour. This probe is the only one in the plan whose first
# parameter is an [int] rather than the [string]$Exe every other probe starts
# with, which is exactly how it came to be handed [int]$null and asked for no
# rounds at all. The harness is fixed; this is the check that the mistake
# cannot come back quietly.
if ($Rounds -lt 1) {
    Write-Host "FAIL: -Rounds was $Rounds; this probe needs at least 1" -ForegroundColor Red
    Write-Host "RESULT: open, close, open again FAILED (nothing was tested)" -ForegroundColor Red
    exit 1
}

$binDir = Split-Path -Parent $PSScriptRoot
$binDir = Join-Path (Split-Path -Parent $PSScriptRoot) 'bin'

# name -> @{ class; stop } where stop is 'kill' or the command id to send.
$PROGRAMS = @(
    @{ name = 'shot';    cls = 'MuskShotHost';   stop = 'kill' },
    @{ name = 'picker';  cls = 'MuskPickerMain'; stop = 'cmd40004' },   # tray: exit
    @{ name = 'envtool'; cls = 'MuskEnvTool';    stop = 'close' },
    @{ name = 'slice';   cls = 'MuskSliceMain';  stop = 'close' }
)

# A window handle is only reusable once the window is really gone, so a stale
# one is evidence of a leak rather than of a slow close.
function Test-WindowGone([int]$ProcId, [string]$Class) {
    Start-Sleep -Milliseconds 250
    -not (@(Get-AppWindows $ProcId) | Where-Object { (Get-ClassName $_) -eq $Class })
}

foreach ($p in $PROGRAMS) {
    $exe = Join-Path $binDir ("{0}.exe" -f $p.name)
    if (-not (Test-Path $exe)) {
        Add-Check ("{0}.exe is built" -f $p.name) $false "not found at $exe (run .\build.ps1 -Only $($p.name))"
        continue
    }
    Write-Host ("`n{0}.exe - {1} round(s)" -f $p.name, $Rounds) -ForegroundColor Cyan
    $crashMark = Get-CrashMark
    $counts = @()
    $opened = 0
    $cleanExit = 0
    $exited = 0
    $staleWindow = 0

    for ($i = 1; $i -le $Rounds; $i++) {
        $app = Start-ProbeApp $exe @() "probe-life-$($p.name)"
        $win = Find-AppWindow $app.Proc.Id $p.cls 10000
        if ($win -eq [IntPtr]::Zero) {
            $app.Proc.Refresh()
            Add-Check ("{0}.exe round {1} starts and shows a window" -f $p.name, $i) $false `
                ("no '{0}' window (exited={1}, exit={2})" -f $p.cls, $app.Proc.HasExited, `
                 $(if ($app.Proc.HasExited) { $app.Proc.ExitCode } else { '-' }))
            Stop-ProbeApp $app
            break
        }
        $opened++
        Start-Sleep -Milliseconds 700
        $res = Get-ProcResources $app.Proc.Id
        if ($res) { $counts += $res }

        if ($p.stop -eq 'kill') {
            # No way to ask: the host window is the tray icon's own window and
            # the program ends when it is stopped.
            Stop-Process -Id $app.Proc.Id -Force -ErrorAction SilentlyContinue
            $ok = $app.Proc.WaitForExit(6000)
            $exited++
        } else {
            if ($p.stop -eq 'cmd40004') {
                [void][ProbeLib.U]::PostMessageW($win, $script:WM_COMMAND, [IntPtr]40004, [IntPtr]::Zero)
            } else {
                [void][ProbeLib.U]::PostMessageW($win, $script:WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
            }
            $ok = $app.Proc.WaitForExit(8000)
            $exited++
            if ($ok) {
                $code = $app.Proc.ExitCode
                if ($code -eq 0) { $cleanExit++ }
                else { Add-Check ("{0}.exe round {1} exits cleanly" -f $p.name, $i) $false "exit code $code" }
            }
        }
        if (-not $ok) {
            Add-Check ("{0}.exe round {1} stops when asked" -f $p.name, $i) $false 'still running after the stop'
            Stop-ProbeApp $app
            break
        }
        if (-not (Test-WindowGone $app.Proc.Id $p.cls)) { $staleWindow++ }
    }

    Add-Check ("{0}.exe opened its window on every round" -f $p.name) ($opened -eq $Rounds) `
        "$opened of $Rounds"
    Add-Check ("{0}.exe stopped on every round" -f $p.name) ($exited -eq $Rounds) "$exited of $Rounds"
    if ($p.stop -ne 'kill') {
        Add-Check ("{0}.exe exited with 0 every time" -f $p.name) ($cleanExit -eq $Rounds) `
            "$cleanExit of $Rounds"
    }
    Add-Check ("{0}.exe left no window of its own behind" -f $p.name) ($staleWindow -eq 0) `
        "$staleWindow stale"

    if ($counts.Count -ge 2) {
        $first = $counts[0]
        $last = $counts[$counts.Count - 1]
        $dH = $last.Handles - $first.Handles
        $dG = $last.Gdi - $first.Gdi
        Add-Check ("{0}.exe does not leak handles across {1} restarts" -f $p.name, $Rounds) `
            ($dH -le $Slack) `
            ("{0} -> {1} over {2} rounds (allowed {3})" -f $first.Handles, $last.Handles, $Rounds, $Slack)
        Add-Check ("{0}.exe does not leak GDI objects across {1} restarts" -f $p.name, $Rounds) `
            ($dG -le $Slack) `
            ("{0} -> {1} (allowed {2})" -f $first.Gdi, $last.Gdi, $Slack)
    } else {
        Add-Skip ("{0}.exe does not leak handles" -f $p.name) 'the process could not be counted'
    }

    Add-CrashCheck $crashMark $p.name ("{0} logged no crash across {1} restarts" -f $p.name, $Rounds)
}

Write-Checks 'open, close, open again'
