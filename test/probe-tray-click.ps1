# probe-tray-click.ps1 - black-box check that clicking the notification-area
# icon starts a capture and that cancelling it leaves the app alive.
#
#   .\test\probe-tray-click.ps1 [-Exe .\bin\shot.exe]
#
# Drives the real app with posted messages (no cursor hijacking): the tray
# callback is delivered to the hidden host window exactly the way Explorer
# delivers it, so this exercises the same path a real click takes. Exits 0 when
# the overlay comes up, cancelling returns, and the process is still running.

[CmdletBinding()]
param([string]$Exe)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Exe) { $Exe = Join-Path $root '..\bin\shot.exe' }
if (-not (Test-Path $Exe)) { throw "shot.exe not found at $Exe (build it first)" }
$Exe = (Resolve-Path $Exe).Path

Add-Type -Namespace ShotProbe -Name U -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
public delegate bool EnumProc(IntPtr h, IntPtr p);
'@

$WM_APP           = 0x8000
$WM_LBUTTONUP     = 0x0202
$WM_CHAR          = 0x0102
$VK_ESCAPE        = 0x1B
$HOST_CLASS       = 'MuskShotHost'
$OVERLAY_CLASS    = 'ShotOverlayCls'

function Get-ClassName([IntPtr]$h) {
    $sb = New-Object System.Text.StringBuilder 256
    [void][ShotProbe.U]::GetClassNameW($h, $sb, 256)
    $sb.ToString()
}
# The callback runs as a delegate, so it can only reach a script-scoped
# variable - the function's own locals are out of its scope.
$script:found = [IntPtr]::Zero
function Find-Window([int]$procId, [string]$class) {
    $script:found = [IntPtr]::Zero
    $cb = [ShotProbe.U+EnumProc] {
        param($h, $x)
        $pid2 = 0
        [void][ShotProbe.U]::GetWindowThreadProcessId($h, [ref]$pid2)
        if ($pid2 -eq $procId -and (Get-ClassName $h) -eq $class) {
            $script:found = $h
            return $false
        }
        return $true
    }
    [void][ShotProbe.U]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:found
}
function Wait-Window([int]$procId, [string]$class, [int]$ms) {
    $end = (Get-Date).AddMilliseconds($ms)
    while ((Get-Date) -lt $end) {
        $h = Find-Window $procId $class
        if ($h -ne [IntPtr]::Zero) { return $h }
        Start-Sleep -Milliseconds 200
    }
    [IntPtr]::Zero
}

# --- launch -------------------------------------------------------------
$logDir = Join-Path $root '..\build'
$errLog = Join-Path $logDir 'probe-shot-stderr.txt'
$proc = Start-Process -FilePath $Exe -ArgumentList '--verbose' -PassThru `
        -RedirectStandardError $errLog -RedirectStandardOutput 'NUL'
$fail = 0
try {
    $host1 = Wait-Window $proc.Id $HOST_CLASS 5000
    if ($host1 -eq [IntPtr]::Zero) { throw "host window never appeared (pid $($proc.Id))" }
    Write-Host "host window    = $host1"

    # --- the click ------------------------------------------------------
    # PostMessage, not SendMessage: the host proc runs a nested message loop
    # inside the capture, so a synchronous send would park this script until
    # the whole capture is over.
    [void][ShotProbe.U]::PostMessageW($host1, ($WM_APP + 1), [IntPtr]::Zero, [IntPtr]$WM_LBUTTONUP)

    $overlay = Wait-Window $proc.Id $OVERLAY_CLASS 8000
    if ($overlay -eq [IntPtr]::Zero) {
        Write-Host "FAIL: clicking the tray did not put the capture overlay up" -ForegroundColor Red
        Write-Host ("  process alive: {0}" -f (-not $proc.HasExited))
        $fail++
    } else {
        Write-Host "overlay window = $overlay  OK"
    }

    # --- cancel and check the app is still there ------------------------
    if ($overlay -ne [IntPtr]::Zero) {
        # Esc with no selection yet goes straight to shot_cancel, which
        # destroys the overlay and unwinds the nested loop.
        [void][ShotProbe.U]::PostMessageW($overlay, $WM_CHAR, [IntPtr]$VK_ESCAPE, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 1500
    }

    $proc.Refresh()
    $alive = -not $proc.HasExited
    if (-not $alive) {
        Write-Host ("FAIL: the app exited after the capture (exit code {0})" -f $proc.ExitCode) -ForegroundColor Red
        $fail++
    } else {
        Write-Host "app still running after the capture  OK"
    }

    # --- a second capture must work too ---------------------------------
    if ($alive) {
        [void][ShotProbe.U]::PostMessageW($host1, ($WM_APP + 1), [IntPtr]::Zero, [IntPtr]$WM_LBUTTONUP)
        $overlay2 = Wait-Window $proc.Id $OVERLAY_CLASS 8000
        if ($overlay2 -eq [IntPtr]::Zero) {
            Write-Host "FAIL: the second tray click did not start a capture" -ForegroundColor Red
            $fail++
        } else {
            Write-Host "second overlay = $overlay2  OK"
            [void][ShotProbe.U]::PostMessageW($overlay2, $WM_CHAR, [IntPtr]$VK_ESCAPE, [IntPtr]::Zero)
            Start-Sleep -Milliseconds 1500
        }
        $proc.Refresh()
        if ($proc.HasExited) {
            Write-Host ("FAIL: the app exited after the second capture (exit code {0})" -f $proc.ExitCode) -ForegroundColor Red
            $fail++
        } else {
            Write-Host "app still running after the second capture  OK"
        }
    }

    # --- a full capture: select, annotate, finish ------------------------
    # This is the path a real user walks after clicking the tray, so it is
    # where a "click the tray and it vanishes" bug actually shows up: the
    # drag builds a selection, Enter runs compose + clipboard + close.
    if ($alive) {
        [void][ShotProbe.U]::PostMessageW($host1, ($WM_APP + 1), [IntPtr]::Zero, [IntPtr]$WM_LBUTTONUP)
        $overlay3 = Wait-Window $proc.Id $OVERLAY_CLASS 8000
        if ($overlay3 -eq [IntPtr]::Zero) {
            Write-Host "FAIL: the third tray click did not start a capture" -ForegroundColor Red
            $fail++
        } else {
            Write-Host "third overlay  = $overlay3"
            function Post-Mouse([IntPtr]$h, [uint32]$m, [int]$x, [int]$y, [int]$w) {
                $lp = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
                [void][ShotProbe.U]::PostMessageW($h, $m, [IntPtr]$w, $lp)
            }
            $WM_LBUTTONDOWN = 0x0201; $WM_MOUSEMOVE = 0x0200; $MK_LBUTTON = 1
            # drag out a selection
            Post-Mouse $overlay3 $WM_LBUTTONDOWN 200 150 $MK_LBUTTON
            Start-Sleep -Milliseconds 120
            Post-Mouse $overlay3 $WM_MOUSEMOVE  400 300 $MK_LBUTTON
            Start-Sleep -Milliseconds 120
            Post-Mouse $overlay3 $WM_MOUSEMOVE  700 480 $MK_LBUTTON
            Start-Sleep -Milliseconds 120
            Post-Mouse $overlay3 $WM_LBUTTONUP   700 480 0
            Start-Sleep -Milliseconds 400
            # scribble inside it
            Post-Mouse $overlay3 $WM_LBUTTONDOWN 300 250 $MK_LBUTTON
            Start-Sleep -Milliseconds 80
            Post-Mouse $overlay3 $WM_MOUSEMOVE  450 350 $MK_LBUTTON
            Start-Sleep -Milliseconds 80
            Post-Mouse $overlay3 $WM_MOUSEMOVE  600 420 $MK_LBUTTON
            Start-Sleep -Milliseconds 80
            Post-Mouse $overlay3 $WM_LBUTTONUP   600 420 0
            Start-Sleep -Milliseconds 300
            Write-Host "  selected + annotated"

            # Enter -> shot_finish_and_copy
            [void][ShotProbe.U]::PostMessageW($overlay3, $WM_CHAR, [IntPtr]0x0D, [IntPtr]::Zero)
            Start-Sleep -Milliseconds 2500

            $proc.Refresh()
            if ($proc.HasExited) {
                Write-Host ("FAIL: the app exited while finishing the capture (exit code {0})" -f $proc.ExitCode) -ForegroundColor Red
                $fail++
            } else {
                Write-Host "app survived finishing the capture  OK"
            }
        }
    }
}
finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
}

$crash = Join-Path $env:LOCALAPPDATA 'MuskTools\crash.log'
if (Test-Path $crash) {
    $lines = Get-Content $crash -Tail 40
    $shotCrashes = $lines | Select-String 'shot crash'
    if ($shotCrashes) {
        Write-Host "crash.log records a shot crash:" -ForegroundColor Red
        $lines | Select-Object -Last 25 | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }
        $fail++
    }
}

Write-Host ""
if ($fail -eq 0) { Write-Host "RESULT: tray click starts a capture and the app survives it" -ForegroundColor Green; exit 0 }
else { Write-Host "RESULT: $fail check(s) failed" -ForegroundColor Red; exit 1 }
