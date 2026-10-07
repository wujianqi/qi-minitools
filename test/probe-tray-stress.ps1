# probe-tray-stress.ps1 - repeat the whole tray -> capture -> annotate -> copy
# cycle many times in one process, checking the app survives every round.
#
#   .\test\probe-tray-stress.ps1 [-Exe .\bin\shot.exe] [-Rounds 20]
#
# A user takes dozens of captures a day in one long-lived process, so a defect
# that only shows up after N cycles (a leaked GDI object, a stale handle, state
# left over from the previous capture) is invisible to a single-shot probe.
#
# NOTE: keep this file pure ASCII - Windows PowerShell 5.1 reads a .ps1 without
# a BOM as ANSI, and mojibake in a comment can swallow the next statement.

[CmdletBinding()]
param([string]$Exe, [int]$Rounds = 20)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Exe) { $Exe = Join-Path $root '..\bin\shot.exe' }
if (-not (Test-Path $Exe)) { throw "shot.exe not found at $Exe (build it first)" }
$Exe = (Resolve-Path $Exe).Path

Add-Type -Namespace StressProbe -Name U -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("kernel32.dll")] public static extern bool GetProcessHandleCount(IntPtr p, out uint c);
[DllImport("user32.dll")] public static extern uint GetGuiResources(IntPtr p, uint f);
public delegate bool EnumProc(IntPtr h, IntPtr p);
'@

$WM_APP = 0x8000; $WM_LBUTTONUP = 0x0202
$WM_LBUTTONDOWN = 0x0201; $WM_MOUSEMOVE = 0x0200; $WM_CHAR = 0x0102; $WM_KEYDOWN = 0x0100
$MK_LBUTTON = 1; $VK_RETURN = 0x0D; $VK_ESCAPE = 0x1B
$HOST_CLASS = 'MuskShotHost'; $OVERLAY_CLASS = 'ShotOverlayCls'

function Get-ClassName([IntPtr]$h) {
    $sb = New-Object System.Text.StringBuilder 256
    [void][StressProbe.U]::GetClassNameW($h, $sb, 256)
    $sb.ToString()
}
$script:found = [IntPtr]::Zero
function Find-Window([int]$procId, [string]$class) {
    $script:found = [IntPtr]::Zero
    $cb = [StressProbe.U+EnumProc] {
        param($h, $x)
        $pid2 = 0
        [void][StressProbe.U]::GetWindowThreadProcessId($h, [ref]$pid2)
        if ($pid2 -eq $procId -and (Get-ClassName $h) -eq $class) { $script:found = $h; return $false }
        return $true
    }
    [void][StressProbe.U]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:found
}
function Wait-Window([int]$procId, [string]$class, [int]$ms) {
    $end = (Get-Date).AddMilliseconds($ms)
    while ((Get-Date) -lt $end) {
        $h = Find-Window $procId $class
        if ($h -ne [IntPtr]::Zero) { return $h }
        Start-Sleep -Milliseconds 100
    }
    [IntPtr]::Zero
}
function Post-Mouse([IntPtr]$h, [uint32]$m, [int]$x, [int]$y, [int]$w) {
    $lp = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
    [void][StressProbe.U]::PostMessageW($h, $m, [IntPtr]$w, $lp)
}

$logDir = Join-Path $root '..\build'
$errLog = Join-Path $logDir 'probe-shotstress-stderr.txt'
$proc = Start-Process -FilePath $Exe -PassThru `
        -RedirectStandardError $errLog -RedirectStandardOutput 'NUL'
$fail = 0
# The first few captures pay a one-time cost (GDI+ font/brush caches, the
# clipboard's format list). A leak shows up as a *slope* over the later rounds,
# not as the absolute delta, so the check below compares round 5 with the end.
$slopeH = 0; $slopeG = 0; $slopeU = 0
try {
    $hostWin = Wait-Window $proc.Id $HOST_CLASS 5000
    if ($hostWin -eq [IntPtr]::Zero) { throw "host window never appeared (pid $($proc.Id))" }

    $h0 = 0; [void][StressProbe.U]::GetProcessHandleCount($proc.Handle, [ref]$h0)
    $g0 = [StressProbe.U]::GetGuiResources($proc.Handle, 0)  # GR_GDIOBJECTS
    $u0 = [StressProbe.U]::GetGuiResources($proc.Handle, 1)  # GR_USEROBJECTS
    Write-Host "start: handles=$h0 gdi=$g0 user=$u0"

    for ($i = 1; $i -le $Rounds; $i++) {
        # 1. tray click
        [void][StressProbe.U]::PostMessageW($hostWin, ($WM_APP + 1), [IntPtr]::Zero, [IntPtr]$WM_LBUTTONUP)
        $ov = Wait-Window $proc.Id $OVERLAY_CLASS 8000
        if ($ov -eq [IntPtr]::Zero) {
            Write-Host ("FAIL round {0}: the tray click produced no overlay" -f $i) -ForegroundColor Red
            $fail++
            break
        }
        # 2. select a region
        $sx = 100 + ($i * 7) % 200
        $sy = 80  + ($i * 5) % 120
        Post-Mouse $ov $WM_LBUTTONDOWN $sx $sy $MK_LBUTTON
        Post-Mouse $ov $WM_MOUSEMOVE  ($sx + 240) ($sy + 180) $MK_LBUTTON
        Post-Mouse $ov $WM_MOUSEMOVE  ($sx + 420) ($sy + 300) $MK_LBUTTON
        Post-Mouse $ov $WM_LBUTTONUP   ($sx + 420) ($sy + 300) 0
        Start-Sleep -Milliseconds 200
        # 3. annotate with two different tools
        Post-Mouse $ov $WM_LBUTTONDOWN ($sx + 60) ($sy + 40) $MK_LBUTTON
        Post-Mouse $ov $WM_MOUSEMOVE  ($sx + 200) ($sy + 150) $MK_LBUTTON
        Post-Mouse $ov $WM_LBUTTONUP   ($sx + 200) ($sy + 150) 0
        Start-Sleep -Milliseconds 120
        [void][StressProbe.U]::PostMessageW($ov, $WM_KEYDOWN, [IntPtr][int][char]'R', [IntPtr]::Zero)
        Post-Mouse $ov $WM_LBUTTONDOWN ($sx + 120) ($sy + 90) $MK_LBUTTON
        Post-Mouse $ov $WM_LBUTTONUP   ($sx + 300) ($sy + 200) 0
        Start-Sleep -Milliseconds 150
        # 4. finish: compose + clipboard + close
        [void][StressProbe.U]::PostMessageW($ov, $WM_CHAR, [IntPtr]$VK_RETURN, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 600

        $proc.Refresh()
        if ($proc.HasExited) {
            $code = $proc.ExitCode
            $kind = if ($code -eq 0) { 'CLEAN EXIT' }
                    elseif ($code -eq 3221225477) { 'ACCESS VIOLATION' }
                    elseif ($code -eq 3221226505) { 'HEAP CORRUPTION' }
                    else { "exit $code" }
            Write-Host ("FAIL round {0}: the app died ({1})" -f $i, $kind) -ForegroundColor Red
            $fail++
            break
        }
        if ($i % 5 -eq 0) {
            $h = 0; [void][StressProbe.U]::GetProcessHandleCount($proc.Handle, [ref]$h)
            $g = [StressProbe.U]::GetGuiResources($proc.Handle, 0)
            $u = [StressProbe.U]::GetGuiResources($proc.Handle, 1)
            Write-Host ("  round {0,2}: handles {1,4} ({2,4:+#;-#;0})  gdi {3,4} ({4,4:+#;-#;0})  user {5,4} ({6,4:+#;-#;0})" -f `
                $i, $h, ($h - $h0), $g, ($g - $g0), $u, ($u - $u0))
            if ($i -eq 5) { $slopeH = $h; $slopeG = $g; $slopeU = $u }
        }
    }

    if ($fail -eq 0) {
        $h = 0; [void][StressProbe.U]::GetProcessHandleCount($proc.Handle, [ref]$h)
        $g = [StressProbe.U]::GetGuiResources($proc.Handle, 0)
        $u = [StressProbe.U]::GetGuiResources($proc.Handle, 1)
        $dh = $h - $h0; $dg = $g - $g0; $du = $u - $u0
        # growth after the one-time warm-up: this is the part a leak shows in
        $lh = $h - $slopeH; $lg = $g - $slopeG; $lu = $u - $slopeU
        Write-Host ""
        Write-Host ("after {0} rounds: handles {1,4:+#;-#;0}  gdi {2,4:+#;-#;0}  user {3,4:+#;-#;0}" -f $Rounds, $dh, $dg, $du)
        Write-Host ("since round 5:     handles {0,4:+#;-#;0}  gdi {1,4:+#;-#;0}  user {2,4:+#;-#;0}" -f $lh, $lg, $lu)
        if ($lh -gt 8 -or $lg -gt 8 -or $lu -gt 4) {
            Write-Host "FAIL: resources still grow per capture - something is not released" -ForegroundColor Red
            $fail++
        } else {
            Write-Host "OK: resources are flat across $Rounds captures (round 5 onward)" -ForegroundColor Green
        }
    }
}
finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
}

Write-Host ""
if ($fail -eq 0) { Write-Host "RESULT: $Rounds consecutive captures, app healthy throughout" -ForegroundColor Green; exit 0 }
else { Write-Host "RESULT: $fail check(s) failed" -ForegroundColor Red; exit 1 }
