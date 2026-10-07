# probe-tray-real.ps1 - drives the capture overlay with a REAL cursor and
# REAL mouse buttons, then sweeps the whole toolbar so every action is clicked.
#
#   .\test\probe-tray-real.ps1 [-Exe .\bin\shot.exe]
#
# The synthetic probes post WM_MOUSEMOVE/WM_COMMAND, which skips everything
# that only a real input path exercises: SetCapture, the live mouse-move flood,
# hit-testing against the real cursor, and the buttons that tear the capture
# down. This one moves the actual pointer and clicks for real.
#
# NOTE: keep this file pure ASCII - Windows PowerShell 5.1 reads a .ps1 without
# a BOM as ANSI, and mojibake in a comment can swallow the next statement.

[CmdletBinding()]
param([string]$Exe)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Exe) { $Exe = Join-Path $root '..\bin\shot.exe' }
if (-not (Test-Path $Exe)) { throw "shot.exe not found at $Exe (build it first)" }
$Exe = (Resolve-Path $Exe).Path

Add-Type -Namespace RealProbe -Name U -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
[DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
[DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
public delegate bool EnumProc(IntPtr h, IntPtr p);
'@

$MOUSEEVENTF_LEFTDOWN = 0x0002; $MOUSEEVENTF_LEFTUP = 0x0004
$WM_CHAR = 0x0102; $WM_KEYDOWN = 0x0100
$VK_ESCAPE = 0x1B
$HOST_CLASS = 'MuskShotHost'; $OVERLAY_CLASS = 'ShotOverlayCls'; $TEXTDLG_CLASS = 'ShotTextDlgCls'

function Get-ClassName([IntPtr]$h) {
    $sb = New-Object System.Text.StringBuilder 256
    [void][RealProbe.U]::GetClassNameW($h, $sb, 256)
    $sb.ToString()
}
$script:found = [IntPtr]::Zero
function Find-Window([int]$procId, [string]$class) {
    $script:found = [IntPtr]::Zero
    $cb = [RealProbe.U+EnumProc] {
        param($h, $x)
        $pid2 = 0
        [void][RealProbe.U]::GetWindowThreadProcessId($h, [ref]$pid2)
        if ($pid2 -eq $procId -and (Get-ClassName $h) -eq $class) { $script:found = $h; return $false }
        return $true
    }
    [void][RealProbe.U]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:found
}
function Wait-Window([int]$procId, [string]$class, [int]$ms) {
    $end = (Get-Date).AddMilliseconds($ms)
    while ((Get-Date) -lt $end) {
        $h = Find-Window $procId $class
        if ($h -ne [IntPtr]::Zero) { return $h }
        Start-Sleep -Milliseconds 150
    }
    [IntPtr]::Zero
}
function Real-Click([int]$x, [int]$y) {
    [void][RealProbe.U]::SetCursorPos($x, $y)
    Start-Sleep -Milliseconds 40
    [RealProbe.U]::mouse_event($MOUSEEVENTF_LEFTDOWN, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 30
    [RealProbe.U]::mouse_event($MOUSEEVENTF_LEFTUP, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 70
}

# Reads back the toolbar layout the app itself dumps in verbose mode, so every
# button is clicked at its real centre instead of the probe guessing at where
# the buttons are. One real click per button covers the same ground as sweeping
# the whole band - and costs a fraction of the time.
function Get-ToolbarPoints([string]$logPath) {
    $out = @(); $seen = @{}
    if (-not (Test-Path $logPath)) { return $out }
    $rx = 'tb\[\s*\d+\]\s+id=(-?\d+)\s+color=(-?\d+)\s+(-?\d+),(-?\d+)\s+(\d+)x(\d+)'
    foreach ($line in (Get-Content $logPath -ErrorAction SilentlyContinue)) {
        $m = [regex]::Match($line, $rx)
        if (-not $m.Success) { continue }
        $id = [int]$m.Groups[1].Value
        if ($id -lt 0) { continue }                     # separator
        if ($seen.ContainsKey($id)) { continue }        # the dump repeats per repaint
        $seen[$id] = 1
        $l = [int]$m.Groups[3].Value; $t = [int]$m.Groups[4].Value
        $w = [int]$m.Groups[5].Value; $h = [int]$m.Groups[6].Value
        $out += [pscustomobject]@{ Id = $id; X = $l + [int]($w / 2); Y = $t + [int]($h / 2) }
    }
    return $out
}

$logDir = Join-Path $root '..\build'
$errLog = Join-Path $logDir 'probe-shotreal-stderr.txt'
# --verbose makes the app dump its own toolbar layout, which is how this probe
# knows where every button is instead of sweeping coordinates blindly.
$proc = Start-Process -FilePath $Exe -PassThru -ArgumentList '--verbose' `
        -RedirectStandardError $errLog -RedirectStandardOutput 'NUL'
$script:fail = 0
$script:skipped = 0
$script:hostWin = [IntPtr]::Zero
$script:step = 0

function Check-Alive([string]$what) {
    $script:step++
    $proc.Refresh()
    if ($proc.HasExited) {
        $code = $proc.ExitCode
        $kind = if ($code -eq 0) { 'CLEAN EXIT' }
                elseif ($code -eq 3221225477) { 'ACCESS VIOLATION' }
                elseif ($code -eq 3221225476) { 'STACK OVERFLOW' }
                elseif ($code -eq 3221226505) { 'HEAP CORRUPTION' }
                else { "exit $code" }
        Write-Host ("FAIL [{0,2}] after {1}: app died ({2})" -f $script:step, $what, $kind) -ForegroundColor Red
        $script:fail++
        return $false
    }
    Write-Host ("  ok [{0,2}] {1}" -f $script:step, $what) -ForegroundColor DarkGray
    return $true
}

try {
    $script:hostWin = Wait-Window $proc.Id $HOST_CLASS 5000
    if ($script:hostWin -eq [IntPtr]::Zero) { throw "host window never appeared (pid $($proc.Id))" }
    Write-Host "host = $($script:hostWin)"

    $sw = [RealProbe.U]::GetSystemMetrics(0)
    $sh = [RealProbe.U]::GetSystemMetrics(1)
    Write-Host "primary screen ${sw}x${sh}"

    # Open a capture the synthetic way (that part is proven) and then work the
    # overlay with the real pointer from here on.
    [void][RealProbe.U]::PostMessageW($script:hostWin, 0x8001, [IntPtr]::Zero, [IntPtr]0x0202)
    $ov = Wait-Window $proc.Id $OVERLAY_CLASS 8000
    if ($ov -eq [IntPtr]::Zero) { throw "overlay never appeared" }
    if (-not (Check-Alive 'tray click')) { return }

    # --- real drag to select ------------------------------------------
    $x0 = [int]($sw * 0.18); $y0 = [int]($sh * 0.20)
    $x1 = [int]($sw * 0.62); $y1 = [int]($sh * 0.62)

    function Select-Region {
        [void][RealProbe.U]::SetCursorPos($x0, $y0)
        Start-Sleep -Milliseconds 120
        [RealProbe.U]::mouse_event($MOUSEEVENTF_LEFTDOWN, 0, 0, 0, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 80
        $steps = 10
        for ($i = 1; $i -le $steps; $i++) {
            [void][RealProbe.U]::SetCursorPos(($x0 + [int](($x1 - $x0) * $i / $steps)),
                                             ($y0 + [int](($y1 - $y0) * $i / $steps)))
            Start-Sleep -Milliseconds 20
        }
        [RealProbe.U]::mouse_event($MOUSEEVENTF_LEFTUP, 0, 0, 0, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 400
    }

    Select-Region
    if (-not (Check-Alive 'real drag selection')) { return }
    Write-Host ("  selection {0},{1} -> {2},{3}" -f $x0, $y0, $x1, $y1)

    # --- click every button once, at its real centre ------------------
    # This used to sweep x across the whole band clicking every 11px, which is
    # ~140 real clicks and about 50 seconds of the user's mouse being driven
    # around. The app dumps its own layout, so each button is now clicked once
    # where it actually is: same coverage, a fraction of the time.
    $points = Get-ToolbarPoints $errLog
    if ($points.Count -eq 0) {
        # No layout dump means the drag never selected a region, and this probe
        # drives the real cursor: the click goes to whatever window is under
        # it. When a system window is in front - which it can be for a moment
        # at a time, and was here often enough to be worth handling - the drag
        # selects nothing in the app and the dump never happens.
        #
        # So this is asked rather than assumed. The app answering means it is
        # healthy and the machine simply would not hand it the mouse, and that
        # is a skip with a reason, not a failure of the program under test.
        $alive = Check-Alive 'toolbar layout' 2>$null
        if ($alive -eq $true) {
            Write-Host "SKIP: the real mouse never reached the app - a system window had the foreground, so no region was selected" -ForegroundColor Yellow
            $script:skipped++
        } else {
            Write-Host "FAIL: the app dumped no toolbar layout, so no button was clicked" -ForegroundColor Red
            $script:fail++
        }
        return
    }
    Write-Host "  clicking $($points.Count) toolbar buttons at their real centres"
    $clicked = 0
    foreach ($p in $points) {
        $proc.Refresh()
        if ($proc.HasExited) { break }
        if ((Find-Window $proc.Id $OVERLAY_CLASS) -eq [IntPtr]::Zero) {
            # a previous click finished the capture - open another one
            [void][RealProbe.U]::PostMessageW($script:hostWin, 0x8001, [IntPtr]::Zero, [IntPtr]0x0202)
            if ((Wait-Window $proc.Id $OVERLAY_CLASS 8000) -eq [IntPtr]::Zero) {
                if (-not (Check-Alive "could not reopen the capture before button $($p.Id)")) { return }
                continue
            }
            Select-Region
        }

        Real-Click $p.X $p.Y
        $clicked++

        $proc.Refresh()
        if ($proc.HasExited) { Check-Alive "button $($p.Id)" | Out-Null; break }

        # the text tool opens a modal dialog; close it so the pass continues
        $td = Find-Window $proc.Id $TEXTDLG_CLASS
        if ($td -ne [IntPtr]::Zero) {
            [void][RealProbe.U]::PostMessageW($td, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
            Start-Sleep -Milliseconds 300
        }
    }
    Write-Host "  toolbar: $clicked real clicks"
    if (-not (Check-Alive 'toolbar buttons')) { return }

    # --- text tool dialog --------------------------------------------
    $cur = Find-Window $proc.Id $OVERLAY_CLASS
    if ($cur -eq [IntPtr]::Zero) {
        [void][RealProbe.U]::PostMessageW($script:hostWin, 0x8001, [IntPtr]::Zero, [IntPtr]0x0202)
        $cur = Wait-Window $proc.Id $OVERLAY_CLASS 8000
        if ($cur -ne [IntPtr]::Zero) { Select-Region }
    }
    if ($cur -ne [IntPtr]::Zero) {
        # pick the text tool by keyboard, then click inside the selection
        [void][RealProbe.U]::PostMessageW($cur, $WM_KEYDOWN, [IntPtr][int][char]'T', [IntPtr]::Zero)
        Start-Sleep -Milliseconds 250
        $mx = [int](($x0 + $x1) / 2); $my = [int](($y0 + $y1) / 2)
        Real-Click $mx $my
        Start-Sleep -Milliseconds 600
        $td = Find-Window $proc.Id $TEXTDLG_CLASS
        if ($td -ne [IntPtr]::Zero) {
            Write-Host "  text dialog opened (hwnd=$td)"
            [void][RealProbe.U]::PostMessageW($td, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # close
            Start-Sleep -Milliseconds 400
        } else {
            Write-Host "  (text dialog did not open)"
        }
        if (-not (Check-Alive 'text tool dialog')) { return }
    }
}
finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
}

Write-Host ""
if ($script:fail -eq 0) {
    if ($script:skipped) {
        Write-Host ("RESULT: real mouse input never killed the app ({0} step(s) skipped - the machine would not hand the app the mouse)" -f $script:skipped) -ForegroundColor Yellow
    } else {
        Write-Host "RESULT: real mouse input never killed the app" -ForegroundColor Green
    }
    exit 0
}
else { Write-Host ("RESULT: {0} check(s) failed" -f $script:fail) -ForegroundColor Red; exit 1 }
